'use strict';

const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

// All levels are vertex-aligned, just like the 129x129 height tiles. Filtering
// happens on the global field BEFORE slicing: shared edges are byte-identical.
function normalsFromHeights(heights, width, depth, spacing, heightScale) {
    const normals = new Float32Array(width * depth * 3);
    const scale = heightScale / 65535;
    for (let z = 0; z < depth; ++z) for (let x = 0; x < width; ++x) {
        const x0 = Math.max(x - 1, 0), x1 = Math.min(x + 1, width - 1);
        const z0 = Math.max(z - 1, 0), z1 = Math.min(z + 1, depth - 1);
        const dx = (heights[z * width + x1] - heights[z * width + x0]) * scale / ((x1 - x0) * spacing);
        const dz = (heights[z1 * width + x] - heights[z0 * width + x]) * scale / ((z1 - z0) * spacing);
        const length = Math.hypot(dx, 1, dz), i = (z * width + x) * 3;
        normals[i] = -dx / length;
        normals[i + 1] = 1 / length; // terrain-local +Y up; rows run along +Z
        normals[i + 2] = -dz / length;
    }
    return normals;
}

function downsampleNormals(src, width, depth) {
    const nextWidth = (width + 1) / 2, nextDepth = (depth + 1) / 2;
    const dst = new Float32Array(nextWidth * nextDepth * 3);
    for (let z = 0; z < nextDepth; ++z) for (let x = 0; x < nextWidth; ++x) {
        let nx = 0, ny = 0, nz = 0;
        // Centered [1,2,1] filter keeps the samples aligned to height vertices.
        for (let dz = -1; dz <= 1; ++dz) for (let dx = -1; dx <= 1; ++dx) {
            const sx = Math.max(0, Math.min(width - 1, x * 2 + dx));
            const sz = Math.max(0, Math.min(depth - 1, z * 2 + dz));
            const i = (sz * width + sx) * 3;
            const w = (dx === 0 ? 2 : 1) * (dz === 0 ? 2 : 1);
            nx += src[i] * w; ny += src[i + 1] * w; nz += src[i + 2] * w;
        }
        const length = Math.hypot(nx, ny, nz), i = (z * nextWidth + x) * 3;
        dst[i] = nx / length; dst[i + 1] = ny / length; dst[i + 2] = nz / length;
    }
    return { normals: dst, width: nextWidth, depth: nextDepth };
}

function encodeTile(normals, width, x, z, resolution) {
    const bytes = Buffer.alloc(resolution * resolution * 3);
    const edge = resolution - 1;
    for (let j = 0; j < resolution; ++j) for (let i = 0; i < resolution; ++i) {
        const source = ((z * edge + j) * width + x * edge + i) * 3;
        for (let c = 0; c < 3; ++c) bytes[(j * resolution + i) * 3 + c] =
            Math.round(Math.max(0, Math.min(255, (normals[source + c] * 0.5 + 0.5) * 255)));
    }
    return PNG.sync.write({ width: resolution, height: resolution, data: bytes },
        { colorType: 2, inputColorType: 2, bitDepth: 8, deflateLevel: 6 });
}

function generateNormalTiles(manifestPath, output) {
    const original = fs.readFileSync(manifestPath, 'utf8');
    const manifest = JSON.parse(original), root = path.dirname(manifestPath);
    const { sectorSizeMeters: sectorSize, sectorCount, maxLevel, minTileLevel, nodeTileResolution: resolution, heightScale } = manifest;
    if (!Array.isArray(sectorCount) || sectorCount.length !== 2 || sectorCount.some(v => !Number.isInteger(v) || v < 1)
        || !Number.isInteger(maxLevel) || maxLevel > 8 || !Number.isInteger(minTileLevel) || minTileLevel < 0 || minTileLevel > maxLevel
        || !Number.isInteger(resolution) || resolution < 3 || resolution % 2 !== 1
        || !Number.isFinite(sectorSize) || sectorSize <= 0 || !Number.isFinite(heightScale) || heightScale <= 0) {
        throw new Error('Unsupported height tile layout');
    }
    const divisions = 2 ** (maxLevel - minTileLevel), edge = resolution - 1;
    const nx = sectorCount[0] * divisions, nz = sectorCount[1] * divisions;
    let width = nx * edge + 1, depth = nz * edge + 1;
    if (width * depth > 20000000) throw new Error('Normal generator currently supports at most 20 million source samples');
    let heights = new Uint16Array(width * depth);
    for (let z = 0; z < nz; ++z) for (let x = 0; x < nx; ++x) {
        const png = PNG.sync.read(fs.readFileSync(path.join(root, `nodes/L${minTileLevel}/h_${x}_${z}.png`)), { skipRescale: true });
        if (png.width !== resolution || png.height !== resolution || png.depth !== 16 || png.colorType !== 0) throw new Error('Expected uint16 height PNG');
        const channels = png.data.length / (resolution * resolution);
        for (let j = 0; j < resolution; ++j) for (let i = 0; i < resolution; ++i) {
            const at = (z * edge + j) * width + x * edge + i, h = png.data[(j * resolution + i) * channels];
            if (((x > 0 && i === 0) || (z > 0 && j === 0)) && heights[at] !== h) throw new Error('Height border mismatch');
            heights[at] = h;
        }
    }
    console.log(`Normal source: ${width}x${depth}; shared height borders verified`);
    let normals = normalsFromHeights(heights, width, depth, sectorSize / divisions / edge, heightScale);
    heights = null;
    let tiles = 0;
    for (let level = minTileLevel; level <= maxLevel; ++level) {
        const dir = path.join(output, `nodes/L${level}`);
        fs.mkdirSync(dir, { recursive: true });
        for (let z = 0; z < (depth - 1) / edge; ++z) for (let x = 0; x < (width - 1) / edge; ++x) {
            const png = encodeTile(normals, width, x, z, resolution);
            fs.writeFileSync(path.join(dir, `n_${x}_${z}.png`), png);
            ++tiles;
        }
        console.log(`L${level}: RGB8 PNG normals written (${width}x${depth} global samples)`);
        if (level < maxLevel) ({ normals, width, depth } = downsampleNormals(normals, width, depth));
    }
    const normalMap = { format: 'RGB8', fileFormat: 'PNG', resolution, space: 'terrain-local', upAxis: 'Y',
        path: 'nodes/L{level}/n_{x}_{z}.png', minTileLevel, maxTileLevel: maxLevel };
    // Preserve the existing manifest formatting and its large height arrays.
    let updated;
    if (manifest.normalMap !== undefined) {
        if (JSON.stringify(manifest.normalMap) !== JSON.stringify(normalMap)) throw new Error('Existing normalMap layout differs');
        updated = original;
    } else {
        updated = original.replace(/\s*}\s*$/, ',\n  "normalMap": ' + JSON.stringify(normalMap, null, 2).replace(/\n/g, '\n  ') + '\n}\n');
    }
    fs.writeFileSync(path.join(output, path.basename(manifestPath)), updated);
    console.log(`Generated ${tiles} normal tiles; same resolution and spacing as height tiles`);
    return tiles;
}

module.exports = { normalsFromHeights, downsampleNormals, encodeTile, generateNormalTiles };
if (require.main === module) {
    try {
        const args = process.argv.slice(2);
        if (args.length !== 4 || args[0] !== '--manifest' || args[2] !== '--output') throw new Error('Usage: --manifest <file> --output <directory>');
        generateNormalTiles(path.resolve(args[1]), path.resolve(args[3]));
    } catch (error) { console.error(error); process.exitCode = 1; }
}

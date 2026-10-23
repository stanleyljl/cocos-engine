'use strict';

const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

// Keep these dimensions in sync with LandscapeConfig.h and the asset importer.
const TILE_RESOLUTION = 129;
const NORMAL_BORDER = 8;
const NORMAL_RESOLUTION = TILE_RESOLUTION + 2 * NORMAL_BORDER;

// Interiors are vertex-aligned with the height tiles. Filter the global field
// before slicing padded tiles so every overlapping sample is byte-identical.
function normalsFromHeights(heights, width, depth, spacing, heightScale) {
    const normals = new Float32Array(width * depth * 3);
    const scale = heightScale / 65535;
    for (let z = 0; z < depth; ++z) {
        for (let x = 0; x < width; ++x) {
            const x0 = Math.max(x - 1, 0);
            const x1 = Math.min(x + 1, width - 1);
            const z0 = Math.max(z - 1, 0);
            const z1 = Math.min(z + 1, depth - 1);
            const dx = (heights[z * width + x1] - heights[z * width + x0]) * scale / ((x1 - x0) * spacing);
            const dz = (heights[z1 * width + x] - heights[z0 * width + x]) * scale / ((z1 - z0) * spacing);
            const length = Math.hypot(dx, 1, dz);
            const i = (z * width + x) * 3;
            normals[i] = -dx / length;
            normals[i + 1] = 1 / length; // terrain-local +Y up; rows run along +Z
            normals[i + 2] = -dz / length;
        }
    }
    return normals;
}

function downsampleNormals(src, width, depth) {
    const nextWidth = (width + 1) / 2;
    const nextDepth = (depth + 1) / 2;
    const dst = new Float32Array(nextWidth * nextDepth * 3);
    for (let z = 0; z < nextDepth; ++z) {
        for (let x = 0; x < nextWidth; ++x) {
            let nx = 0;
            let ny = 0;
            let nz = 0;
            // Centered [1,2,1] filter keeps the samples aligned to height vertices.
            for (let dz = -1; dz <= 1; ++dz) {
                for (let dx = -1; dx <= 1; ++dx) {
                    const sx = Math.max(0, Math.min(width - 1, x * 2 + dx));
                    const sz = Math.max(0, Math.min(depth - 1, z * 2 + dz));
                    const i = (sz * width + sx) * 3;
                    const w = (dx === 0 ? 2 : 1) * (dz === 0 ? 2 : 1);
                    nx += src[i] * w;
                    ny += src[i + 1] * w;
                    nz += src[i + 2] * w;
                }
            }
            const length = Math.hypot(nx, ny, nz);
            const i = (z * nextWidth + x) * 3;
            dst[i] = nx / length;
            dst[i + 1] = ny / length;
            dst[i + 2] = nz / length;
        }
    }
    return { normals: dst, width: nextWidth, depth: nextDepth };
}

function encodeTile(normals, width, depth, x, z, resolution) {
    const padded = resolution + 2 * NORMAL_BORDER;
    const bytes = Buffer.alloc(padded * padded * 3);
    const edge = resolution - 1;
    for (let j = 0; j < padded; ++j) {
        const sz = Math.max(0, Math.min(depth - 1, z * edge + j - NORMAL_BORDER));
        for (let i = 0; i < padded; ++i) {
            // Clamp only at the outer landscape boundary, never at tile borders.
            const sx = Math.max(0, Math.min(width - 1, x * edge + i - NORMAL_BORDER));
            const source = (sz * width + sx) * 3;
            for (let c = 0; c < 3; ++c) {
                bytes[(j * padded + i) * 3 + c] = Math.round(Math.max(0, Math.min(255, (normals[source + c] * 0.5 + 0.5) * 255)));
            }
        }
    }
    return PNG.sync.write({ width: padded, height: padded, data: bytes },
        { colorType: 2, inputColorType: 2, bitDepth: 8, deflateLevel: 6 });
}

function generateNormalTiles(manifestPath, output) {
    const original = fs.readFileSync(manifestPath, 'utf8');
    const manifest = JSON.parse(original);
    const root = path.dirname(manifestPath);
    const { sectorSizeMeters: sectorSize, sectorCount, maxLevel, minTileLevel, heightScale } = manifest;
    if (!Array.isArray(sectorCount) || sectorCount.length !== 2 || sectorCount.some(v => !Number.isInteger(v) || v < 1)
        || !Number.isInteger(maxLevel) || maxLevel < 0 || maxLevel > 8 || !Number.isInteger(minTileLevel) || minTileLevel < 0 || minTileLevel > maxLevel
        || typeof sectorSize !== 'number' || sectorSize <= 0 || typeof heightScale !== 'number' || heightScale <= 0) {
        throw new Error('Unsupported height tile layout');
    }
    const resolution = TILE_RESOLUTION;
    const divisions = 2 ** (maxLevel - minTileLevel);
    const edge = resolution - 1;
    const nx = sectorCount[0] * divisions;
    const nz = sectorCount[1] * divisions;
    let width = nx * edge + 1;
    let depth = nz * edge + 1;
    if (width * depth > 20000000) {
        throw new Error('Normal generator currently supports at most 20 million source samples');
    }
    let heights = new Uint16Array(width * depth);
    for (let z = 0; z < nz; ++z) {
        for (let x = 0; x < nx; ++x) {
            const png = PNG.sync.read(fs.readFileSync(path.join(root, `nodes/L${minTileLevel}/h_${x}_${z}.png`)), { skipRescale: true });
            if (png.width !== resolution || png.height !== resolution || png.depth !== 16 || png.colorType !== 0) {
                throw new Error('Expected uint16 height PNG');
            }
            const channels = png.data.length / (resolution * resolution);
            for (let j = 0; j < resolution; ++j) {
                for (let i = 0; i < resolution; ++i) {
                    const at = (z * edge + j) * width + x * edge + i;
                    const h = png.data[(j * resolution + i) * channels];
                    if (((x > 0 && i === 0) || (z > 0 && j === 0)) && heights[at] !== h) {
                        throw new Error('Height border mismatch');
                    }
                    heights[at] = h;
                }
            }
        }
    }
    console.log(`Normal source: ${width}x${depth}; shared height borders verified`);
    let normals = normalsFromHeights(heights, width, depth, sectorSize / divisions / edge, heightScale);
    heights = null;
    let tiles = 0;
    for (let level = minTileLevel; level <= maxLevel; ++level) {
        const dir = path.join(output, `nodes/L${level}`);
        fs.mkdirSync(dir, { recursive: true });
        for (let z = 0; z < (depth - 1) / edge; ++z) {
            for (let x = 0; x < (width - 1) / edge; ++x) {
                const png = encodeTile(normals, width, depth, x, z, resolution);
                fs.writeFileSync(path.join(dir, `n_${x}_${z}.png`), png);
                ++tiles;
            }
        }
        console.log(`L${level}: RGB8 PNG normals written (${width}x${depth} global samples)`);
        if (level < maxLevel) {
            ({ normals, width, depth } = downsampleNormals(normals, width, depth));
        }
    }
    fs.writeFileSync(path.join(output, path.basename(manifestPath)), original);
    console.log(`Generated ${tiles} ${NORMAL_RESOLUTION}x${NORMAL_RESOLUTION} normal tiles; ${resolution}x${resolution} interiors and ${NORMAL_BORDER}-sample borders`);
    return tiles;
}

module.exports = { NORMAL_BORDER, NORMAL_RESOLUTION, normalsFromHeights, downsampleNormals, encodeTile, generateNormalTiles };
if (require.main === module) {
    try {
        const args = process.argv.slice(2);
        if (args.length !== 4 || args[0] !== '--manifest' || args[2] !== '--output') {
            throw new Error('Usage: --manifest <file> --output <directory>');
        }
        generateNormalTiles(path.resolve(args[1]), path.resolve(args[3]));
    } catch (error) {
        console.error(error);
        process.exitCode = 1;
    }
}

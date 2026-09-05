'use strict';

const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

const DEFAULT_SECTOR_SIZE = 4096;
const DEFAULT_TILE_SIZE = 129;
const DEFAULT_MAX_LEVEL = 5;
const DEFAULT_MIN_TILE_LEVEL = 0;
const DEFAULT_HEIGHT_SCALE = 2000;
const DEFAULT_HEIGHT_BIAS = 0;
const HEIGHT_MAX = 0xffff;

function fail (message) {
    throw new Error(message);
}

// Pretty-prints the manifest, but keeps each heightRange.{min,max} number array
// on a single line (they are large and unreadable when expanded one-per-line).
function stringifyManifest (manifest) {
    const inlineArrays = [];
    if (Array.isArray(manifest.levels)) {
        for (const lv of manifest.levels) {
            if (!lv || !lv.heightRange) continue;
            for (const key of ['min', 'max']) {
                if (Array.isArray(lv.heightRange[key])) {
                    const token = `@@INLINE_${inlineArrays.length}@@`;
                    inlineArrays.push(lv.heightRange[key]); // keep the real array
                    lv.heightRange[key] = token;            // stringify a placeholder
                }
            }
        }
    }
    let json = JSON.stringify(manifest, null, 2);
    json = json.replace(/"@@INLINE_(\d+)@@"/g, (_, i) => JSON.stringify(inlineArrays[Number(i)]));
    // Restore the object so the caller's manifest is left untouched.
    let idx = 0;
    if (Array.isArray(manifest.levels)) {
        for (const lv of manifest.levels) {
            if (!lv || !lv.heightRange) continue;
            for (const key of ['min', 'max']) {
                if (typeof lv.heightRange[key] === 'string' && lv.heightRange[key].startsWith('@@INLINE_')) {
                    lv.heightRange[key] = inlineArrays[idx];
                    ++idx;
                }
            }
        }
    }
    return `${json}\n`;
}

function parseArgs (argv) {
    const result = {};
    for (let i = 0; i < argv.length; ++i) {
        const arg = argv[i];
        if (arg === '--help' || arg === '-h') {
            result.help = true;
            continue;
        }
        if (!arg.startsWith('--')) {
            fail(`Unexpected argument: ${arg}`);
        }
        const key = arg.slice(2).replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
        const next = argv[i + 1];
        if (next === undefined || next.startsWith('--')) {
            result[key] = true;
        } else {
            result[key] = next;
            ++i;
        }
    }
    return result;
}

function intOption (options, name, defaultValue, minimum = 1) {
    const rawValue = options[name];
    const value = rawValue === undefined ? defaultValue : Number(rawValue);
    if (typeof rawValue === 'boolean' || !Number.isFinite(value) || !Number.isSafeInteger(value) || value < minimum) {
        const description = minimum === 0 ? 'a non-negative safe integer' : 'a positive safe integer';
        fail(`--${name.replace(/[A-Z]/g, letter => `-${letter.toLowerCase()}`)} must be ${description}`);
    }
    return value;
}

function numberOption (options, name, defaultValue) {
    const rawValue = options[name];
    const value = rawValue === undefined ? defaultValue : Number(rawValue);
    if (typeof rawValue === 'boolean' || !Number.isFinite(value) || (name === 'heightScale' && value <= 0)) {
        fail(name === 'heightScale' ? '--heightScale must be a positive finite number' : `--${name} must be a finite number`);
    }
    return value;
}

function safeProduct (a, b, description) {
    const value = a * b;
    if (!Number.isSafeInteger(value)) {
        fail(`${description} exceeds the safe integer range`);
    }
    return value;
}

function safeDimension (a, b, description) {
    const value = safeProduct(a, b, description) + 1;
    if (!Number.isSafeInteger(value)) {
        fail(`${description} exceeds the safe integer range`);
    }
    return value;
}

function clamp01 (value) {
    return Math.max(0, Math.min(1, value));
}

// Discover the material-library albedo layers: PNGs directly under
// <assetDir>/materials (sorted; the raw/ archive subdir is ignored). Layer id =
// array index, capped at 32 (splat ids are 5-bit).
function scanMaterialLayers (assetDir) {
    const dir = path.join(assetDir, 'materials');
    if (!fs.existsSync(dir)) {
        return [];
    }
    return fs.readdirSync(dir)
        .filter((f) => /\.png$/i.test(f) && fs.statSync(path.join(dir, f)).isFile())
        .sort()
        .slice(0, 32);
}

function printHelp () {
    console.log(`Usage:
  node scripts/landscape/heightmap-tiles.js --input source.png --out D:\\work\\editors\\projects\\landscape\\assets\\landscape --name terrain
  node scripts/landscape/heightmap-tiles.js --procedural --out D:\\work\\editors\\projects\\landscape\\assets\\landscape --name demo

Input:
  --input <png>       8-bit or 16-bit grayscale PNG, one meter per source sample
  --procedural        Generate a deterministic test height field without an input file
Output:
  --out <dir>         Parent directory for assets/landscape/<name>
  --name <name>       Landscape asset directory and .json manifest name
  --force             Allow replacing an existing .json manifest

Layout options:
  --sectors-x <n>     Sector count on X (default: 1)
  --sectors-y <n>     Sector count on Y (default: 1)
  --sector-size <m>   Sector size in meters (default: 4096)
  --max-level <n>     Quadtree levels L0..Ln (default: 5)
  --min-tile-level <n> Finest level with a generated tile (default: 0).
                      Levels finer than this (L0..n-1) get NO height tile
                      and permanently sample the L<n> ancestor at runtime.
  --tile-size <n>     Height tile resolution (default: 129)
  --height-scale <m>  Physical height range represented by uint16 (default: 2000)
  --height-bias <m>   Physical height at encoded value 0 (default: 0)

The output uses 16-bit grayscale PNG tiles (PNG color type 0, bit depth 16).
After decoding, each sample is an R16UI uint16 height value in [0, 65535].
Node coordinates are global per level.
`);
}

function readHeightPng (filePath) {
    const png = PNG.sync.read(fs.readFileSync(filePath), { skipRescale: true });
    if (png.depth !== 8 && png.depth !== 16) {
        fail(`Height source must be 8-bit or 16-bit PNG: ${filePath}`);
    }
    // Accept grayscale (0/4) and RGB(A) (2/6). Height is read from channel 0;
    // RGB heightmaps whose channels are equal (grayscale-in-RGB) work directly.
    if (![0, 2, 4, 6].includes(png.colorType)) {
        fail(`Height source must be a grayscale or RGB(A) PNG: ${filePath}`);
    }

    const sampleCount = safeProduct(png.width, png.height, `PNG dimensions for ${filePath}`);
    if (sampleCount < 1) {
        fail(`Invalid PNG dimensions: ${filePath}`);
    }
    const channelCount = png.data.length / sampleCount;
    if (!Number.isInteger(channelCount) || channelCount < 1) {
        fail(`Invalid PNG sample layout: ${filePath}`);
    }
    const maxValue = png.depth === 16 ? 65535 : 255;
    return {
        width: png.width,
        height: png.height,
        sampleAt (x, y) {
            const index = (y * png.width + x) * channelCount;
            return png.data[index] / maxValue;
        },
    };
}

function proceduralSample (x, y, width, height) {
    const u = x / (width - 1);
    const v = y / (height - 1);
    const broad = 0.18 * Math.sin(u * Math.PI * 3.0) * Math.cos(v * Math.PI * 2.0);
    const ridge = 0.10 * Math.sin((u + v) * Math.PI * 11.0);
    const basin = 0.08 * Math.cos(u * Math.PI * 7.0 + 0.4) * Math.cos(v * Math.PI * 5.0);
    return clamp01(0.5 + broad + ridge + basin);
}

function writeHeightTile (filePath, tileSize, sampleAt) {
    // 16-bit grayscale PNG (color type 0, bit depth 16), directly viewable as a
    // heightmap. After decoding, each sample is an R16UI uint16 height value.
    // Returns the tile's [min,max] quantized height values so the manifest can
    // carry a tight per-node vertical bound for culling / LOD.
    const values = new Uint16Array(tileSize * tileSize);
    let minValue = HEIGHT_MAX;
    let maxValue = 0;
    for (let y = 0; y < tileSize; ++y) {
        for (let x = 0; x < tileSize; ++x) {
            const value = Math.round(clamp01(sampleAt(x, y)) * HEIGHT_MAX);
            if (value < minValue) minValue = value;
            if (value > maxValue) maxValue = value;
            values[y * tileSize + x] = value;
        }
    }

    const encoded = PNG.sync.write({
        width: tileSize,
        height: tileSize,
        data: Buffer.from(values.buffer),
    }, {
        colorType: 0,
        inputColorType: 0,
        bitDepth: 16,
        deflateLevel: 9,
    });
    fs.writeFileSync(filePath, encoded);
    return { minValue, maxValue };
}

// Computes a level's per-node tight height bounds directly from the source
// heightmap (no tile written). Used for levels finer than --min-tile-level,
// which have no tile of their own but still need tight bounds so the CDLOD
// distance rings don't blow up (a coarse global fallback would force the
// finest level's range past the whole world). Samples the source at its native
// 1m resolution over each node's integer extent; cheap for the small fine nodes.
function nodeBoundsFromSource (nodeSize, originX, originY, heightAt) {
    let minValue = HEIGHT_MAX;
    let maxValue = 0;
    for (let ty = 0; ty <= nodeSize; ++ty) {
        for (let tx = 0; tx <= nodeSize; ++tx) {
            const value = Math.round(clamp01(heightAt(originX + tx, originY + ty)) * HEIGHT_MAX);
            if (value < minValue) minValue = value;
            if (value > maxValue) maxValue = value;
        }
    }
    return { minValue, maxValue };
}

function removeExistingHeightTiles (assetDir) {
    const nodesDir = path.join(assetDir, 'nodes');
    if (!fs.existsSync(nodesDir)) {
        return;
    }
    for (const levelName of fs.readdirSync(nodesDir)) {
        const levelDir = path.join(nodesDir, levelName);
        if (!fs.statSync(levelDir).isDirectory()) {
            continue;
        }
        for (const fileName of fs.readdirSync(levelDir)) {
            if (/^h_\d+_\d+\.(?:png|r16|bin)$/i.test(fileName)) {
                fs.unlinkSync(path.join(levelDir, fileName));
            }
        }
    }
}

function makeManifest (options) {
    const {
        name,
        sectorsX,
        sectorsY,
        sectorSize,
        tileSize,
        maxLevel,
        minTileLevel,
        heightScale,
        heightBias,
        materialLayers,
        bounds,
    } = options;
    const levels = [];
    for (let level = 0; level <= maxLevel; ++level) {
        levels.push({
            level,
            // Per-node quantized height bounds, row-major iz*nx+ix (global coords).
            // Runtime builds a tight node AABB: y = heightBias + (v/65535)*heightScale.
            heightRange: {
                min: bounds[level].min,
                max: bounds[level].max,
            },
        });
    }

    return {
        version: 1,
        name,
        sectorSizeMeters: sectorSize,
        sectorCount: [sectorsX, sectorsY],
        maxLevel,
        minTileLevel,
        nodeTileResolution: tileSize,
        heightScale,
        heightBias,
        materialLibrary: {
            count: materialLayers.length,
            dir: 'materials',
            layers: materialLayers.map((file, id) => ({ id, file })),
        },
        levels,
    };
}

function generate (rawOptions) {
    const options = rawOptions;
    if (options.help) {
        printHelp();
        return;
    }

    const name = options.name || 'heightmap-demo';
    if (!/^[a-zA-Z0-9_-]+$/.test(name)) {
        fail('--name may contain only letters, numbers, underscore and hyphen');
    }
    if (Boolean(options.input) === Boolean(options.procedural)) {
        fail('Choose exactly one of --input or --procedural');
    }

    const sectorsX = intOption(options, 'sectorsX', 1);
    const sectorsY = intOption(options, 'sectorsY', 1);
    const sectorSize = intOption(options, 'sectorSize', DEFAULT_SECTOR_SIZE);
    const tileSize = intOption(options, 'tileSize', DEFAULT_TILE_SIZE);
    const maxLevel = intOption(options, 'maxLevel', DEFAULT_MAX_LEVEL, 0);
    if (maxLevel > 7) {
        fail('--max-level must be <= 7');
    }
    const minTileLevel = intOption(options, 'minTileLevel', DEFAULT_MIN_TILE_LEVEL, 0);
    const heightScale = numberOption(options, 'heightScale', DEFAULT_HEIGHT_SCALE);
    const heightBias = numberOption(options, 'heightBias', DEFAULT_HEIGHT_BIAS);

    if (tileSize < 2 || (tileSize - 1) % 2 !== 0) {
        fail('--tile-size must be 2n+1 so adjacent LOD samples align');
    }
    if (sectorSize % (1 << maxLevel) !== 0) {
        fail('--sector-size must be divisible by 2^max-level');
    }
    if (minTileLevel > maxLevel) {
        fail('--min-tile-level must be <= --max-level');
    }
    // Only tile-generating levels (>= minTileLevel) need an integer source
    // sample step; finer levels have no tile and only need per-node bounds.
    for (let level = minTileLevel; level <= maxLevel; ++level) {
        const divisions = 1 << (maxLevel - level);
        const nodeSize = sectorSize / divisions;
        if (nodeSize % (tileSize - 1) !== 0) {
            fail(`Level L${level} does not have an integer source sample step`);
        }
    }

    const outParent = path.resolve(options.out || path.join('assets', 'landscape'));
    const assetDir = path.join(outParent, name);
    const manifestPath = path.join(assetDir, `${name}.json`);
    if (fs.existsSync(manifestPath) && !options.force) {
        fail(`Output already exists: ${manifestPath}. Use --force to replace it.`);
    }
    fs.mkdirSync(assetDir, { recursive: true });

    const sourceWidth = safeDimension(sectorsX, sectorSize, 'Source width');
    const sourceHeight = safeDimension(sectorsY, sectorSize, 'Source height');
    const source = options.procedural
        ? { type: 'procedural' }
        : readHeightPng(path.resolve(options.input));
    if (source.type !== 'procedural') {
        if (source.width !== sourceWidth || source.height !== sourceHeight) {
            fail(`Input size must be ${sourceWidth}x${sourceHeight}, got ${source.width}x${source.height}`);
        }
    }

    const heightAt = (x, y) => source.type === 'procedural'
        ? proceduralSample(x, y, sourceWidth, sourceHeight)
        : source.sampleAt(x, y);
    if (options.force) {
        removeExistingHeightTiles(assetDir);
    }

    const materialLayers = scanMaterialLayers(assetDir);
    if (materialLayers.length > 0) {
        console.log(`Material library: ${materialLayers.length} layers (${materialLayers.join(', ')})`);
    }

    let totalTiles = 0;
    // Per-level, per-node quantized height bounds (row-major iz*nx+ix, global
    // per-level coords). L0 (finest) bounds come from each tile; coarser levels
    // are merged bottom-up below so a node's box encloses its whole subtree.
    const bounds = [];
    for (let level = 0; level <= maxLevel; ++level) {
        const divisions = 1 << (maxLevel - level);
        const nx = safeProduct(sectorsX, divisions, `Level L${level} X dimensions`);
        const nz = safeProduct(sectorsY, divisions, `Level L${level} Y dimensions`);
        bounds.push({
            nx,
            nz,
            min: new Array(nx * nz).fill(HEIGHT_MAX),
            max: new Array(nx * nz).fill(0),
        });
    }

    for (let level = 0; level <= maxLevel; ++level) {
        const divisions = 1 << (maxLevel - level); // L0 = finest = most divisions
        const nodeSize = sectorSize / divisions;
        const hasTile = level >= minTileLevel;
        const sampleStep = hasTile ? nodeSize / (tileSize - 1) : 1; // bounds-only fine levels sample at 1m
        const levelDir = path.join(assetDir, 'nodes', `L${level}`);
        if (hasTile) {
            fs.mkdirSync(levelDir, { recursive: true });
        }
        const nxLevel = bounds[level].nx;

        for (let y = 0; y < sectorsY * divisions; ++y) {
            for (let x = 0; x < sectorsX * divisions; ++x) {
                const originX = x * nodeSize;
                const originY = y * nodeSize;
                let range;
                if (hasTile) {
                    range = writeHeightTile(
                        path.join(levelDir, `h_${x}_${y}.png`),
                        tileSize,
                        (tileX, tileY) => heightAt(originX + tileX * sampleStep, originY + tileY * sampleStep),
                    );
                    ++totalTiles;
                } else {
                    // No tile for this fine level; compute tight bounds from the
                    // source directly so CDLOD ranges stay sane.
                    range = nodeBoundsFromSource(nodeSize, originX, originY, heightAt);
                }
                bounds[level].min[y * nxLevel + x] = range.minValue;
                bounds[level].max[y * nxLevel + x] = range.maxValue;
            }
        }
        console.log(`L${level}: ${sectorsX * divisions}x${sectorsY * divisions} nodes${hasTile ? ' (height)' : ' (bounds only, no tile)'}`);
    }

    // Bottom-up merge: a node's bound must enclose its 4 children so hierarchical
    // frustum culling never rejects a subtree whose finer detail pokes out. L0 is
    // finest, so a level-L node encloses its four children at level L-1.
    for (let level = 1; level <= maxLevel; ++level) {
        const nx = bounds[level].nx;
        const nz = bounds[level].nz;
        const cnx = bounds[level - 1].nx;
        for (let y = 0; y < nz; ++y) {
            for (let x = 0; x < nx; ++x) {
                let mn = bounds[level].min[y * nx + x];
                let mx = bounds[level].max[y * nx + x];
                for (let dy = 0; dy < 2; ++dy) {
                    for (let dx = 0; dx < 2; ++dx) {
                        const ci = (y * 2 + dy) * cnx + (x * 2 + dx);
                        if (bounds[level - 1].min[ci] < mn) mn = bounds[level - 1].min[ci];
                        if (bounds[level - 1].max[ci] > mx) mx = bounds[level - 1].max[ci];
                    }
                }
                bounds[level].min[y * nx + x] = mn;
                bounds[level].max[y * nx + x] = mx;
            }
        }
    }

    const manifest = makeManifest({
        name,
        sectorsX,
        sectorsY,
        sectorSize,
        tileSize,
        maxLevel,
        minTileLevel,
        heightScale,
        heightBias,
        materialLayers,
        bounds,
    });
    fs.writeFileSync(manifestPath, stringifyManifest(manifest));
    console.log(`Wrote ${totalTiles} height tiles to ${assetDir}`);
    console.log(`Manifest: ${manifestPath}`);
}

try {
    generate(parseArgs(process.argv.slice(2)));
} catch (error) {
    console.error(`heightmap-tiles: ${error.message}`);
    process.exitCode = 1;
}

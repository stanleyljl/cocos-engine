'use strict';

// Recomputes per-node height bounds (levels[].heightRange[index] = [min, max])
// from generated height tiles. Levels finer than minTileLevel retain the bounds
// exported from the source heightmap because they have no tiles on disk.
//
//   node scripts/landscape/patch-height-range.js --manifest <path-to>.json
//
// heightRange values are quantized uint16 (0..65535), row-major iz*nx+ix in
// global per-level coords; a node encloses its subtree (merged bottom-up).

const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

const HEIGHT_MAX = 0xffff;

function fail (message) {
    console.error(`patch-height-range: ${message}`);
    process.exitCode = 1;
    throw new Error(message);
}

function parseArgs (argv) {
    const out = {};
    for (let i = 0; i < argv.length; ++i) {
        const arg = argv[i];
        if (!arg.startsWith('--')) fail(`Unexpected argument: ${arg}`);
        const key = arg.slice(2);
        const next = argv[i + 1];
        if (next === undefined || next.startsWith('--')) { out[key] = true; } else { out[key] = next; ++i; }
    }
    return out;
}

function safeInteger (value, name, minimum = 0, maximum = Number.MAX_SAFE_INTEGER) {
    if (typeof value === 'boolean' || !Number.isSafeInteger(value) || value < minimum || value > maximum) {
        fail(`${name} must be a safe integer in [${minimum}, ${maximum}]`);
    }
    return value;
}

function safeProduct (a, b, name) {
    const value = a * b;
    if (!Number.isSafeInteger(value)) fail(`${name} exceeds the safe integer range`);
    return value;
}

function readNumber (value, name) {
    if (typeof value !== 'number') fail(`${name} must be a number`);
    return value;
}

// Keep height-bound arrays and individual decal instances on one line.
// The replacer leaves the caller's manifest unchanged.
function stringifyManifest (manifest) {
    const inlineValues = [];
    return JSON.stringify(manifest, function (key, value) {
        if (this === manifest.decals) {
            const fields = Object.entries(value).map(([name, field]) => `${JSON.stringify(name)}: ${JSON.stringify(field)}`);
            inlineValues.push(`{${fields.join(', ')}}`);
            return `@@INLINE_${inlineValues.length - 1}@@`;
        }
        if (key === 'heightRange' && Array.isArray(value)) {
            inlineValues.push(JSON.stringify(value));
            return `@@INLINE_${inlineValues.length - 1}@@`;
        }
        return value;
    }, 2).replace(/"@@INLINE_(\d+)@@"/g, (_, i) => inlineValues[Number(i)]) + '\n';
}

// Bounds cover only the 129x129 interior; neighboring padding belongs to other nodes.
const HEIGHT_BORDER = 4;
function tileBounds (filePath, tileSize) {
    const resolution = tileSize + 2 * HEIGHT_BORDER;
    let png;
    try {
        png = PNG.sync.read(fs.readFileSync(filePath), { skipRescale: true });
    } catch (error) {
        fail(`Unable to read or parse tile ${filePath}: ${error.message}`);
    }
    if (!png || png.depth !== 16 || png.colorType !== 0 || !Number.isSafeInteger(png.width) || !Number.isSafeInteger(png.height)
        || png.width !== resolution || png.height !== resolution) {
        fail(`Tile ${filePath} must be a 16-bit grayscale PNG of ${resolution}x${resolution}`);
    }
    const sampleCount = safeProduct(png.width, png.height, `Tile dimensions for ${filePath}`);
    const channels = png.data.length / sampleCount;
    if (channels !== 1 && channels !== 4 || !(png.data instanceof Uint16Array)) {
        fail(`Tile ${filePath} has an invalid 16-bit grayscale sample layout`);
    }
    let minValue = HEIGHT_MAX;
    let maxValue = 0;
    for (let z = 0; z < tileSize; ++z) {
        for (let x = 0; x < tileSize; ++x) {
            const value = png.data[((z + HEIGHT_BORDER) * resolution + x + HEIGHT_BORDER) * channels];
            minValue = Math.min(minValue, value);
            maxValue = Math.max(maxValue, value);
        }
    }
    return { minValue, maxValue };
}

function main () {
    const options = parseArgs(process.argv.slice(2));
    if (!options.manifest || options.manifest === true) {
        fail('Pass --manifest <path-to-manifest.json>');
    }
    const manifestPath = path.resolve(options.manifest);
    const assetDir = path.dirname(manifestPath);
    let manifest;
    try {
        manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    } catch (error) {
        fail(`Unable to read or parse manifest ${manifestPath}: ${error.message}`);
    }
    if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) {
        fail('Manifest must contain a JSON object');
    }
    if (!Array.isArray(manifest.sectorCount) || manifest.sectorCount.length !== 2) {
        fail('Manifest sectorCount must be a two-element array');
    }
    const sectorsX = safeInteger(manifest.sectorCount[0], 'Manifest sectorCount[0]', 1);
    const sectorsY = safeInteger(manifest.sectorCount[1], 'Manifest sectorCount[1]', 1);
    const maxTileLevel = safeInteger(manifest.maxTileLevel, 'Manifest maxTileLevel', 0, 8);
    const tileSize = 129;
    const minTileLevel = safeInteger(manifest.minTileLevel, 'Manifest minTileLevel', 0, maxTileLevel);
    const scaleValue = manifest.heightScale;
    const biasValue = manifest.heightBias;
    const scale = readNumber(scaleValue, 'Manifest heightScale');
    const bias = readNumber(biasValue, 'Manifest heightBias');
    if (scale <= 0) fail('Manifest heightScale must be positive');
    if (!Array.isArray(manifest.levels) || manifest.levels.length !== maxTileLevel + 1) fail('Manifest requires maxTileLevel + 1 levels');
    const levelEntries = new Map();
    for (const [level, lv] of manifest.levels.entries()) {
        if (!lv || typeof lv !== 'object' || Array.isArray(lv)) fail('Manifest levels[] contains a malformed entry');
        levelEntries.set(level, lv);
    }
    for (let level = 0; level <= maxTileLevel; ++level) {
        if (!levelEntries.has(level)) fail(`Manifest is missing level L${level}`);
    }

    const bounds = [];
    for (let level = 0; level <= maxTileLevel; ++level) {
        const divisions = 1 << (maxTileLevel - level); // L0 = finest = most divisions
        const nx = safeProduct(sectorsX, divisions, `Level L${level} X dimensions`);
        const nz = safeProduct(sectorsY, divisions, `Level L${level} Z dimensions`);
        const count = safeProduct(nx, nz, `Level L${level} bounds`);
        const min = new Array(count).fill(HEIGHT_MAX);
        const max = new Array(count).fill(0);
        const hasTile = level >= minTileLevel;
        if (hasTile) {
            const levelDir = path.join(assetDir, 'nodes', `L${level}`);
            for (let y = 0; y < nz; ++y) {
                for (let x = 0; x < nx; ++x) {
                    const tilePath = path.join(levelDir, `h_${x}_${y}.png`);
                    if (!fs.existsSync(tilePath)) fail(`Missing tile: ${tilePath}`);
                    const r = tileBounds(tilePath, tileSize);
                    min[y * nx + x] = r.minValue;
                    max[y * nx + x] = r.maxValue;
                }
            }
        } else {
            // No tiles exist for sub-minTile levels; their bounds were emitted by
            // heightmap-tiles.js and are already in the manifest. Copy them through
            // so the bottom-up merge below can refine the coarser levels.
            const existing = levelEntries.get(level);
            const expectedCount = safeProduct(nx, nz, `Level L${level} bounds`);
            if (existing && Array.isArray(existing.heightRange) && existing.heightRange.length === expectedCount) {
                for (let i = 0; i < expectedCount; ++i) {
                    const range = existing.heightRange[i];
                    if (!Array.isArray(range) || range.length !== 2) {
                        fail(`Level L${level} has invalid heightRange at index ${i}`);
                    }
                    const minValue = range[0];
                    const maxValue = range[1];
                    if (!Number.isSafeInteger(minValue) || !Number.isSafeInteger(maxValue)
                        || minValue < 0 || minValue > HEIGHT_MAX || maxValue < 0 || maxValue > HEIGHT_MAX
                        || minValue > maxValue) {
                        fail(`Level L${level} has invalid heightRange at index ${i}`);
                    }
                    min[i] = minValue;
                    max[i] = maxValue;
                }
            } else {
                fail(`Level L${level} has no tile and no existing heightRange in manifest; cannot derive bounds.`);
            }
        }
        bounds.push({ nx, nz, min, max });
        console.log(`L${level}: ${nx}x${nz} ${hasTile ? 'tiles scanned' : 'bounds carried over'}`);
    }

    // Bottom-up merge so a node's box encloses its whole subtree. L0 is finest,
    // so a level-L node encloses its four children at level L-1.
    for (let level = 1; level <= maxTileLevel; ++level) {
        const { nx, nz, min, max } = bounds[level];
        const cnx = bounds[level - 1].nx;
        const cmin = bounds[level - 1].min;
        const cmax = bounds[level - 1].max;
        for (let y = 0; y < nz; ++y) {
            for (let x = 0; x < nx; ++x) {
                let mn = min[y * nx + x];
                let mx = max[y * nx + x];
                for (let dy = 0; dy < 2; ++dy) {
                    for (let dx = 0; dx < 2; ++dx) {
                        const ci = (y * 2 + dy) * cnx + (x * 2 + dx);
                        if (cmin[ci] < mn) mn = cmin[ci];
                        if (cmax[ci] > mx) mx = cmax[ci];
                    }
                }
                min[y * nx + x] = mn;
                max[y * nx + x] = mx;
            }
        }
    }

    // Array indices define levels, with L0 finest.
    if (!Array.isArray(manifest.levels) || manifest.levels.length !== maxTileLevel + 1) fail('Manifest requires maxTileLevel + 1 levels');
    for (const [level, lv] of manifest.levels.entries()) {
        if (level < 0 || level > maxTileLevel) fail(`Unexpected level ${level}`);
        lv.heightRange = bounds[level].min.map((min, index) => [min, bounds[level].max[index]]);
    }
    fs.writeFileSync(manifestPath, stringifyManifest(manifest));
    // Report the world-space span of the root so the effect is easy to verify.
    const rMin = bias + (bounds[maxTileLevel].min[0] / HEIGHT_MAX) * scale;
    const rMax = bias + (bounds[maxTileLevel].max[0] / HEIGHT_MAX) * scale;
    console.log(`Patched ${manifestPath}`);
    console.log(`Root L${maxTileLevel} node[0] height span: ${rMin.toFixed(1)}m .. ${rMax.toFixed(1)}m (global range is ${bias.toFixed(1)}..${(bias + scale).toFixed(1)})`);
}

try { main(); } catch (e) { /* message already printed */ }

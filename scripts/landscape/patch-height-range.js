'use strict';

// Patches an existing landscape manifest with per-node height bounds
// (levels[].heightRange.{min,max}) computed from the already-generated height
// tiles. Use this when tiles were produced by an older heightmap-tiles.js that
// did not emit heightRange, so you don't need the original source PNG.
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

function finiteNumber (value, name) {
    if (!Number.isFinite(value)) fail(`${name} must be a finite number`);
    return value;
}

// Pretty-prints the manifest but keeps each heightRange.{min,max} number array
// on a single line (they are large and unreadable when expanded one-per-line).
function stringifyManifest (manifest) {
    const inlineArrays = [];
    if (Array.isArray(manifest.levels)) {
        for (const lv of manifest.levels) {
            if (!lv || !lv.heightRange) continue;
            for (const key of ['min', 'max']) {
                if (Array.isArray(lv.heightRange[key])) {
                    const token = `@@INLINE_${inlineArrays.length}@@`;
                    inlineArrays.push(lv.heightRange[key]);
                    lv.heightRange[key] = token;
                }
            }
        }
    }
    let json = JSON.stringify(manifest, null, 2);
    json = json.replace(/"@@INLINE_(\d+)@@"/g, (_, i) => JSON.stringify(inlineArrays[Number(i)]));
    return `${json}\n`;
}

// Returns { minValue, maxValue } over a height tile's samples (channel 0).
function tileBounds (filePath, tileSize) {
    let png;
    try {
        png = PNG.sync.read(fs.readFileSync(filePath), { skipRescale: true });
    } catch (error) {
        fail(`Unable to read or parse tile ${filePath}: ${error.message}`);
    }
    if (!png || png.depth !== 16 || png.colorType !== 0 || !Number.isSafeInteger(png.width) || !Number.isSafeInteger(png.height)
        || png.width !== tileSize || png.height !== tileSize) {
        fail(`Tile ${filePath} must be a 16-bit grayscale PNG of ${tileSize}x${tileSize}`);
    }
    const sampleCount = safeProduct(png.width, png.height, `Tile dimensions for ${filePath}`);
    const channels = png.data.length / sampleCount;
    if (channels !== 1 && channels !== 4 || !(png.data instanceof Uint16Array)) {
        fail(`Tile ${filePath} has an invalid 16-bit grayscale sample layout`);
    }
    let minValue = HEIGHT_MAX;
    let maxValue = 0;
    for (let i = 0; i < sampleCount; ++i) {
        const value = png.data[i * channels];
        if (value < minValue) minValue = value;
        if (value > maxValue) maxValue = value;
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
    const maxLevel = safeInteger(manifest.maxLevel, 'Manifest maxLevel', 0, 7);
    const tileSize = safeInteger(manifest.nodeTileResolution, 'Manifest nodeTileResolution', 2);
    const minTileLevel = manifest.minTileLevel === undefined
        ? 0 : safeInteger(manifest.minTileLevel, 'Manifest minTileLevel', 0, maxLevel);
    const scaleValue = manifest.heightScale;
    const biasValue = manifest.heightBias;
    const scale = finiteNumber(scaleValue, 'Manifest heightScale');
    const bias = finiteNumber(biasValue, 'Manifest heightBias');
    if (scale <= 0) fail('Manifest heightScale must be positive');
    if (!Array.isArray(manifest.levels)) fail('Manifest has no levels[] array');
    const levelEntries = new Map();
    for (const lv of manifest.levels) {
        if (!lv || typeof lv !== 'object' || Array.isArray(lv)) fail('Manifest levels[] contains a malformed entry');
        const level = safeInteger(lv.level, 'Manifest level', 0, maxLevel);
        if (levelEntries.has(level)) fail(`Manifest contains duplicate level ${level}`);
        levelEntries.set(level, lv);
    }
    for (let level = 0; level <= maxLevel; ++level) {
        if (!levelEntries.has(level)) fail(`Manifest is missing level L${level}`);
    }

    const bounds = [];
    for (let level = 0; level <= maxLevel; ++level) {
        const divisions = 1 << (maxLevel - level); // L0 = finest = most divisions
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
            if (existing && existing.heightRange
                && Array.isArray(existing.heightRange.min) && Array.isArray(existing.heightRange.max)
                && existing.heightRange.min.length === expectedCount && existing.heightRange.max.length === expectedCount) {
                for (let i = 0; i < expectedCount; ++i) {
                    const minValue = existing.heightRange.min[i];
                    const maxValue = existing.heightRange.max[i];
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
    for (let level = 1; level <= maxLevel; ++level) {
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

    // Inject into the manifest, keyed by each level entry's `level` field.
    if (!Array.isArray(manifest.levels)) fail('Manifest has no levels[] array');
    for (const lv of manifest.levels) {
        const level = lv.level;
        if (level < 0 || level > maxLevel) fail(`Unexpected level ${level}`);
        lv.heightRange = { min: bounds[level].min, max: bounds[level].max };
    }
    // Remove descriptions and derived fields no longer consumed by the loader.
    // Keep materialLibrary for the retained material/VT pipeline.
    for (const key of ['worldSizeMeters', 'nodeCoordinateSpace', 'sectorFromNode', 'heightRangeEncoding', 'height', 'splat']) {
        delete manifest[key];
    }
    for (const lv of manifest.levels) {
        for (const key of ['nodeSizeMeters', 'hasTile', 'sampleStepMeters', 'nodeCount']) {
            delete lv[key];
        }
    }

    fs.writeFileSync(manifestPath, stringifyManifest(manifest));
    // Report the world-space span of the root so the effect is easy to verify.
    const rMin = bias + (bounds[maxLevel].min[0] / HEIGHT_MAX) * scale;
    const rMax = bias + (bounds[maxLevel].max[0] / HEIGHT_MAX) * scale;
    console.log(`Patched ${manifestPath}`);
    console.log(`Root L${maxLevel} node[0] height span: ${rMin.toFixed(1)}m .. ${rMax.toFixed(1)}m (global range is ${bias.toFixed(1)}..${(bias + scale).toFixed(1)})`);
}

try { main(); } catch (e) { /* message already printed */ }

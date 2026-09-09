'use strict';

// Generate paired integer splat tiles from the actual height tile pyramid.
// Output is staged separately so an incomplete run cannot publish a manifest.
const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const mix = (a, b, t) => a + (b - a) * t;
function smooth (lo, hi, v) {
    const t = clamp((v - lo) / (hi - lo), 0, 1);
    return t * t * (3 - 2 * t);
}

function readTile (file, resolution) {
    const png = PNG.sync.read(fs.readFileSync(file), { skipRescale: true });
    if (png.width !== resolution || png.height !== resolution || png.depth !== 16 || png.colorType !== 0) {
        throw new Error(`Expected ${resolution}x${resolution} grayscale uint16 PNG: ${file}`);
    }
    const channels = png.data.length / (resolution * resolution);
    const values = new Uint16Array(resolution * resolution);
    for (let i = 0; i < values.length; ++i) values[i] = png.data[i * channels];
    return values;
}

function writeTile (file, resolution, values) {
    const buffer = PNG.sync.write({ width: resolution, height: resolution, data: Buffer.from(values.buffer) }, {
        colorType: 0, inputColorType: 0, bitDepth: 16, deflateLevel: 6,
    });
    fs.writeFileSync(file, buffer);
    // Verify the encoded PNG, including endianness, rather than just the input.
    const decoded = readTile(file, resolution);
    for (let i = 0; i < values.length; ++i) {
        if (decoded[i] !== values[i]) throw new Error(`PNG round-trip mismatch: ${file}, pixel ${i}`);
    }
    return buffer.length;
}

function noise (x, z, scale, seed) {
    x /= scale;
    z /= scale;
    const ix = Math.floor(x), iz = Math.floor(z);
    const tx = smooth(0, 1, x - ix), tz = smooth(0, 1, z - iz);
    const hash = (a, b) => {
        let n = Math.imul(a, 374761393) ^ Math.imul(b, 668265263) ^ seed;
        n = Math.imul(n ^ (n >>> 13), 1274126177);
        return ((n ^ (n >>> 16)) >>> 0) / 4294967295;
    };
    return mix(mix(hash(ix, iz), hash(ix + 1, iz), tx),
        mix(hash(ix, iz + 1), hash(ix + 1, iz + 1), tx), tz) * 2 - 1;
}

function stringifyManifest (manifest) {
    // Keep the existing large height-bound arrays compact, without modifying them.
    const arrays = [];
    return JSON.stringify(manifest, (key, value) => {
        if ((key === 'min' || key === 'max') && Array.isArray(value)) {
            arrays.push(value);
            return `@@SPLAT_ARRAY_${arrays.length - 1}@@`;
        }
        return value;
    }, 2).replace(/"@@SPLAT_ARRAY_(\d+)@@"/g, (_, i) => JSON.stringify(arrays[Number(i)])) + '\n';
}

function main () {
    const args = {};
    for (let i = 2; i < process.argv.length; ++i) {
        const arg = process.argv[i];
        if (!['--manifest', '--output', '--seed', '--height-root'].includes(arg) || !process.argv[i + 1]) {
            throw new Error('Usage: node scripts/landscape/splatmap-tiles.js --manifest <dataset.json> --output <new-directory> [--seed 1337] [--height-root <dataset-directory>]');
        }
        args[arg.slice(2)] = process.argv[++i];
    }
    if (!args.manifest || !args.output) throw new Error('--manifest and --output are required');
    const manifestPath = path.resolve(args.manifest), output = path.resolve(args.output);
    const root = path.resolve(args['height-root'] || path.dirname(manifestPath));
    const original = fs.readFileSync(manifestPath, 'utf8');
    const manifest = JSON.parse(original);
    const seed = args.seed === undefined ? 1337 : Number(args.seed);
    if (!Number.isInteger(seed) || seed < 0 || seed > 0xffffffff) throw new Error('Seed must be uint32');
    if (fs.existsSync(output)) throw new Error('Output must be a new staging directory');
    const { sectorSizeMeters: sectorSize, sectorCount, maxLevel, minTileLevel: minLevel,
        nodeTileResolution: resolution, heightScale, heightBias } = manifest;
    if (!Number.isInteger(maxLevel) || !Number.isInteger(minLevel) || minLevel < 0 || maxLevel < minLevel
        || maxLevel > 7 || resolution !== 129 || !Number.isInteger(sectorSize) || sectorSize <= 0
        || !Array.isArray(sectorCount) || sectorCount.length !== 2
        || sectorCount.some(v => !Number.isInteger(v) || v <= 0)
        || !Number.isFinite(heightScale) || heightScale <= 0 || !Number.isFinite(heightBias)) {
        throw new Error('Invalid landscape dimensions or height encoding');
    }
    // This ecological preset is intentionally tied to the demo's chosen order.
    const names = ['rocky_trail_02', 'rocky_trail', 'grass_path_2', 'rocky_terrain_02',
        'rocky_terrain', 'rocky_terrain_03', 'gray_rocks', 'rocks_ground_01',
        'rocks_ground_02', 'rocks_ground_06', 'rocks_ground_05', 'rocks_ground_04'];
    for (const [id, name] of names.entries()) {
        const layer = manifest.materialLibrary?.layers[id];
        if (!layer || layer.id !== id || layer.name !== name) throw new Error(`Preset requires layer ${id}: ${name}`);
    }
    const materialLayers = manifest.materialLibrary.layers;
    const regional = materialLayers.length > 13;
    if (regional && (materialLayers[13]?.id !== 13 || materialLayers[13]?.name !== 'coral_ground_02'
        || materialLayers[14]?.id !== 14 || materialLayers[14]?.name !== 'brown_mud_leaves_01')) {
        throw new Error('Regional preset requires layers 13 coral_ground_02 and 14 brown_mud_leaves_01');
    }
    const divisions = 2 ** (maxLevel - minLevel);
    const nx = sectorCount[0] * divisions, nz = sectorCount[1] * divisions;
    const edge = resolution - 1, width = nx * edge + 1, depth = nz * edge + 1;
    const spacing = sectorSize / divisions / edge;
    if (!Number.isFinite(spacing) || spacing <= 0 || width * depth > 100000000) {
        throw new Error('Unsupported finest height grid dimensions');
    }
    const heights = new Uint16Array(width * depth);
    let minimum = 65535, maximum = 0;
    for (let z = 0; z < nz; ++z) {
        for (let x = 0; x < nx; ++x) {
            const values = readTile(path.join(root, `nodes/L${minLevel}/h_${x}_${z}.png`), resolution);
            for (let j = 0; j < resolution; ++j) {
                for (let i = 0; i < resolution; ++i) {
                    const index = (z * edge + j) * width + x * edge + i;
                    const value = values[j * resolution + i];
                    if (((x > 0 && i === 0) || (z > 0 && j === 0)) && heights[index] !== value) {
                        throw new Error(`Height border mismatch at ${x * edge + i}, ${z * edge + j}`);
                    }
                    heights[index] = value;
                    minimum = Math.min(minimum, value);
                    maximum = Math.max(maximum, value);
                }
            }
        }
    }
    if (minimum === maximum) throw new Error('This elevation preset requires a non-flat height field');
    console.log(`Reconstructed ${width}x${depth} height samples, spacing ${spacing}m; borders verified`);
    const splats = new Uint16Array(heights.length);
    const coverage = new Float64Array(materialLayers.length);
    const radius = Math.max(1, Math.round(4 / spacing));
    const metersPerUnit = heightScale / 65535;
    for (let z = 0; z < depth; ++z) {
        const z0 = Math.max(0, z - radius), z1 = Math.min(depth - 1, z + radius);
        for (let x = 0; x < width; ++x) {
            const x0 = Math.max(0, x - radius), x1 = Math.min(width - 1, x + radius);
            const index = z * width + x;
            const dx = (heights[z * width + x1] - heights[z * width + x0]) * metersPerUnit / ((x1 - x0) * spacing);
            const dz = (heights[z1 * width + x] - heights[z0 * width + x]) * metersPerUnit / ((z1 - z0) * spacing);
            const slope = Math.atan(Math.hypot(dx, dz)) * 180 / Math.PI;
            const elevation = (heights[index] - minimum) / (maximum - minimum);
            const variation = noise(x * spacing, z * spacing, 320, seed) * 0.9
                + noise(x * spacing, z * spacing, 96, seed ^ 0x5bd1e995) * 0.45;
            const ground = clamp(elevation * 11 + variation, 0, 11);
            const rock = clamp(8.5 + 2.5 * elevation + variation * 0.4, 8, 11);
            const layer = clamp(mix(ground, Math.max(ground, rock), smooth(25, 55, slope)), 0, 11);
            // Adjacent ecological layers form a continuous material sequence:
            // at an integer boundary the outgoing/incoming pair shares its full layer.
            let lower = Math.floor(layer), upper = Math.min(11, lower + 1);
            let blend = layer - lower;
            if (regional) {
                // Continuous regional mask: greener soil in low, gentle patches;
                // pale weathered debris on other low/mid-elevation gentle slopes.
                const region = noise(x * spacing, z * spacing, 220, seed ^ 0x27d4eb2d);
                const mud = smooth(0.05, 0.5, region) * (1 - smooth(0.4, 0.65, elevation))
                    * (1 - smooth(12, 32, slope));
                const debris = smooth(0.1, 0.55, -region) * smooth(0.15, 0.3, elevation)
                    * (1 - smooth(0.6, 0.8, elevation)) * (1 - smooth(20, 40, slope));
                const overlay = Math.max(mud, debris);
                const a = (1 - blend) * (1 - overlay), b = blend * (1 - overlay);
                // Keep the strongest two contributions and renormalize, respecting
                // the two-layer encoding even at a regional/altitude intersection.
                if (overlay > Math.min(a, b)) {
                    if (b > a) lower = upper;
                    upper = mud >= debris ? 14 : 13;
                    blend = overlay / (Math.max(a, b) + overlay);
                }
            }
            const weight = Math.round(blend * 63);
            splats[index] = lower | (upper << 5) | (weight << 10);
            coverage[lower] += 1 - weight / 63;
            coverage[upper] += weight / 63;
        }
        if (z % 1024 === 0) console.log(`Classified row ${z}/${depth}`);
    }
    fs.mkdirSync(output, { recursive: true });
    fs.writeFileSync(path.join(output, 'manifest.before.json'), original);
    const levels = [];
    let total = 0, bytes = 0;
    for (let level = minLevel; level <= maxLevel; ++level) {
        const step = 2 ** (level - minLevel), countX = nx / step, countZ = nz / step;
        const dir = path.join(output, `nodes/L${level}`);
        fs.mkdirSync(dir, { recursive: true });
        for (let z = 0; z < countZ; ++z) {
            for (let x = 0; x < countX; ++x) {
                const height = readTile(path.join(root, `nodes/L${level}/h_${x}_${z}.png`), resolution);
                const values = new Uint16Array(resolution * resolution);
                for (let j = 0; j < resolution; ++j) {
                    for (let i = 0; i < resolution; ++i) {
                        const source = (z * edge + j) * step * width + (x * edge + i) * step;
                        const target = j * resolution + i;
                        if (height[target] !== heights[source]) throw new Error(`Height pyramid mismatch L${level}/${x}/${z}, pixel ${target}`);
                        // Subsample the global integer field; never average packed IDs.
                        values[target] = splats[source];
                    }
                }
                bytes += writeTile(path.join(dir, `s_${x}_${z}.png`), resolution, values);
                ++total;
            }
        }
        levels.push({ level, count: countX * countZ, sampleSpacingMeters: spacing * step });
        console.log(`L${level}: ${countX * countZ} paired tiles written and decoded for verification`);
    }
    manifest.splatMap = {
        format: 'R16UI', fileFormat: 'PNG', resolution,
        path: 'nodes/L{level}/s_{x}_{z}.png', minTileLevel: minLevel, maxTileLevel: maxLevel,
        encoding: { layer0: { offset: 0, bits: 5 }, layer1: { offset: 5, bits: 5 },
            layer1Weight: { offset: 10, bits: 6, divisor: 63 } },
    };
    fs.writeFileSync(path.join(output, path.basename(manifestPath)), stringifyManifest(manifest));
    const report = {
        preset: regional ? 'demo-altitude-slope-regions-v2' : 'demo-altitude-slope-v1', seed, tiles: total, bytes, resolution, levels,
        heightRangeMeters: [heightBias + minimum * metersPerUnit, heightBias + maximum * metersPerUnit],
        slopeRadiusMeters: radius * spacing, rockSlopeDegrees: [25, 55], noiseScalesMeters: [320, 96],
        excludedLayers: [12],
        regionalNoiseScaleMeters: regional ? 220 : null,
        weightedCoverage: materialLayers.map(({name, id}) => ({ id, name, percent: coverage[id] / splats.length * 100 })),
        verification: 'All PNG pixels round-tripped; height borders and every pyramid sample checked; splats share one global field.',
    };
    fs.writeFileSync(path.join(output, 'splat-report.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report, null, 2));
}

try { main(); } catch (error) {
    console.error(`splatmap-tiles: ${error.message}`);
    process.exitCode = 1;
}

/* Generate terrain-aligned modulation data for Landscape VT composition.
 * Source rows follow +Z, matching heightmap-tiles.js; do not flip or tile them.
 * RGB is raw UNORM gain data (128 = neutral), Alpha is regional influence.
 * Source diffuse colors only guide variation; snow/rock whites and blue water
 * are not transferred into the material color. No illumination is baked.
 * Usage: node global-color-map.js --source <diffuse.png> --heightmap <height.png>
 *        --width <meters> --depth <meters> --height-scale <meters> --output <map.png>
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

function resampleColorMap(source, resolution) {
    if (!Number.isInteger(resolution) || resolution < 1 || resolution > 4096) {
        throw new Error('Resolution must be an integer in [1, 4096]');
    }
    const result = new PNG({ width: resolution, height: resolution });
    const linear = Array.from({ length: 256 }, (_, index) => {
        const value = index / 255;
        return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
    });
    const stepX = source.width / resolution;
    const stepY = source.height / resolution;
    for (let y = 0; y < resolution; ++y) {
        const top = y * stepY;
        const bottom = (y + 1) * stepY;
        for (let x = 0; x < resolution; ++x) {
            const left = x * stepX;
            const right = (x + 1) * stepX;
            const sums = [0, 0, 0];
            for (let sy = Math.floor(top); sy < Math.min(Math.ceil(bottom), source.height); ++sy) {
                const height = Math.min(bottom, sy + 1) - Math.max(top, sy);
                for (let sx = Math.floor(left); sx < Math.min(Math.ceil(right), source.width); ++sx) {
                    const weight = height * (Math.min(right, sx + 1) - Math.max(left, sx));
                    const src = (sy * source.width + sx) * 4;
                    for (let c = 0; c < 3; ++c) {
                        sums[c] += linear[source.data[src + c]] * weight;
                    }
                }
            }
            const dst = (y * resolution + x) * 4;
            for (let c = 0; c < 3; ++c) {
                const value = sums[c] / (stepX * stepY);
                const srgb = value <= 0.0031308 ? value * 12.92 : 1.055 * value ** (1 / 2.4) - 0.055;
                result.data[dst + c] = Math.round(Math.min(1, Math.max(0, srgb)) * 255);
            }
            result.data[dst + 3] = 255;
        }
    }
    return result;
}

// Separable box filtering keeps generation linear in image size, even for
// large world-space radii. Clamp at the landscape edges; never wrap opposite sides.
function blur(values, size, radius) {
    const horizontal = new Float32Array(values.length);
    const result = new Float32Array(values.length);
    const clamp = value => Math.max(0, Math.min(size - 1, value));
    const count = radius * 2 + 1;
    for (let y = 0; y < size; ++y) {
        let sum = 0;
        for (let k = -radius; k <= radius; ++k) {
            sum += values[y * size + clamp(k)];
        }
        for (let x = 0; x < size; ++x) {
            horizontal[y * size + x] = sum / count;
            sum += values[y * size + clamp(x + radius + 1)] - values[y * size + clamp(x - radius)];
        }
    }
    for (let x = 0; x < size; ++x) {
        let sum = 0;
        for (let k = -radius; k <= radius; ++k) {
            sum += horizontal[clamp(k) * size + x];
        }
        for (let y = 0; y < size; ++y) {
            result[y * size + x] = sum / count;
            sum += horizontal[clamp(y + radius + 1) * size + x] - horizontal[clamp(y - radius) * size + x];
        }
    }
    return result;
}

function createBiomeMap(source, heightmap, { resolution = 2048, width, depth, heightScale }) {
    if (!(width > 0) || !(depth > 0) || !(heightScale > 0)) {
        throw new Error('Terrain width, depth and height scale must be positive');
    }
    const result = resampleColorMap(source, resolution);
    const heights = new Float32Array(resolution * resolution);
    const vegetation = new Float32Array(heights.length);
    const clamp01 = value => Math.max(0, Math.min(1, value));
    const smooth = (a, b, value) => {
        const t = clamp01((value - a) / (b - a));
        return t * t * (3 - 2 * t);
    };
    const maximum = heightmap.depth === 16 ? 65535 : 255;
    let minHeight = Infinity;
    let maxHeight = -Infinity;
    for (let y = 0; y < resolution; ++y) {
        const sy = (y + 0.5) / resolution * (heightmap.height - 1);
        const iy = Math.floor(sy);
        const fy = sy - iy;
        for (let x = 0; x < resolution; ++x) {
            const sx = (x + 0.5) / resolution * (heightmap.width - 1);
            const ix = Math.floor(sx);
            const fx = sx - ix;
            const sample = (dx, dy) => heightmap.data[(Math.min(iy + dy, heightmap.height - 1) * heightmap.width
                + Math.min(ix + dx, heightmap.width - 1)) * 4] / maximum * heightScale;
            const h = (sample(0, 0) * (1 - fx) + sample(1, 0) * fx) * (1 - fy)
                + (sample(0, 1) * (1 - fx) + sample(1, 1) * fx) * fy;
            const i = y * resolution + x;
            heights[i] = h;
            minHeight = Math.min(minHeight, h);
            maxHeight = Math.max(maxHeight, h);
            const r = result.data[i * 4] / 255;
            const g = result.data[i * 4 + 1] / 255;
            const b = result.data[i * 4 + 2] / 255;
            // Neutral rock/snow and blue rivers do not define vegetation tint.
            vegetation[i] = smooth(0.005, 0.08, g - Math.max(r, b)) * (1 - smooth(0.75, 0.95, g));
        }
    }
    const spacing = Math.min(width, depth) / resolution;
    const radius = Math.max(1, Math.round(16 / spacing));
    const cover = blur(vegetation, resolution, radius);
    const regionalHeight = blur(heights, resolution, Math.max(1, Math.round(96 / spacing)));
    const elevationRange = Math.max(1, maxHeight - minHeight);
    for (let y = 0; y < resolution; ++y) {
        for (let x = 0; x < resolution; ++x) {
            const i = y * resolution + x;
            const left = Math.max(0, x - 1);
            const right = Math.min(resolution - 1, x + 1);
            const top = Math.max(0, y - 1);
            const bottom = Math.min(resolution - 1, y + 1);
            const dx = (heights[y * resolution + right] - heights[y * resolution + left])
                / Math.max(width / resolution, (right - left) * width / resolution);
            const dz = (heights[bottom * resolution + x] - heights[top * resolution + x])
                / Math.max(depth / resolution, (bottom - top) * depth / resolution);
            const slope = Math.hypot(dx, dz);
            const altitude = (heights[i] - minHeight) / elevationRange;
            // A terrain-driven artistic moisture proxy, not a hydrology simulation.
            const valley = clamp01(0.5 + (regionalHeight[i] - heights[i]) / 40);
            const wetness = clamp01(0.55 * valley + 0.3 * cover[i] + 0.15 * (1 - altitude));
            const gains = [1.15 - 0.35 * wetness, 1.01 + 0.03 * wetness, 0.91 - 0.06 * wetness];
            const influence = (1 - smooth(0.4, 1.1, slope))
                * (1 - smooth(0.65, 0.95, altitude)) * (0.4 + 0.6 * cover[i]);
            for (let c = 0; c < 3; ++c) {
                result.data[i * 4 + c] = Math.round(128 * gains[c]);
            }
            result.data[i * 4 + 3] = Math.round(255 * influence);
        }
    }
    return result;
}

if (require.main === module) {
    const args = require('yargs/yargs')(process.argv.slice(2))
        .option('source', { type: 'string', demandOption: true })
        .option('heightmap', { type: 'string', demandOption: true })
        .option('width', { type: 'number', demandOption: true })
        .option('depth', { type: 'number', demandOption: true })
        .option('height-scale', { type: 'number', demandOption: true })
        .option('output', { type: 'string', demandOption: true })
        .option('resolution', { type: 'number', default: 2048 })
        .strict().parse();
    if ([args.source, args.heightmap].some(file => path.resolve(file) === path.resolve(args.output))) {
        throw new Error('Keep source images unchanged');
    }
    const source = PNG.sync.read(fs.readFileSync(args.source));
    const heightmap = PNG.sync.read(fs.readFileSync(args.heightmap), { skipRescale: true });
    const result = createBiomeMap(source, heightmap, args);
    fs.writeFileSync(args.output, PNG.sync.write(result, { colorType: 6, bitDepth: 8 }));
    console.log(`Biome modulation: ${args.resolution}x${args.resolution}, RGB neutral=128, A=regional influence`);
}

module.exports = { resampleColorMap, createBiomeMap };

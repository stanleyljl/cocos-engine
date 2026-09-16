/* Create the resident global color map used by Landscape's VT composition.
 * Source rows follow +Z, matching heightmap-tiles.js; do not flip or tile them.
 * Usage: node global-color-map.js --source <diffuse.png> --output <global.png>
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
                    for (let c = 0; c < 3; ++c) sums[c] += linear[source.data[src + c]] * weight;
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

if (require.main === module) {
    const args = require('yargs/yargs')(process.argv.slice(2))
        .option('source', { type: 'string', demandOption: true })
        .option('output', { type: 'string', demandOption: true })
        .option('resolution', { type: 'number', default: 2048 })
        .strict().parse();
    if (path.resolve(args.source) === path.resolve(args.output)) throw new Error('Keep the source image unchanged');
    // pngjs converts 16-bit sources to RGBA8; alpha is intentionally discarded.
    const source = PNG.sync.read(fs.readFileSync(args.source));
    const result = resampleColorMap(source, args.resolution);
    fs.writeFileSync(args.output, PNG.sync.write(result, { colorType: 6, bitDepth: 8 }));
    console.log(`Global color map: ${source.width}x${source.height} -> ${args.resolution}x${args.resolution} RGBA8`);
}

module.exports = { resampleColorMap };

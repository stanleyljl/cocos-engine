'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { PNG } = require('pngjs');
const { NORMAL_BORDER, NORMAL_RESOLUTION, normalsFromHeights, downsampleNormals, encodeTile } = require('./normalmap-tiles');

test('planar slopes retain their direction at outer edges and every pyramid level', () => {
    const width = 17, heights = new Uint16Array(width * width);
    for (let z = 0; z < width; ++z) for (let x = 0; x < width; ++x) heights[z * width + x] = 1000 + x * 2 - z * 3;
    let field = { normals: normalsFromHeights(heights, width, width, 1, 65535), width, depth: width };
    const length = Math.hypot(2, 1, 3), expected = [-2 / length, 1 / length, 3 / length];
    for (let level = 0; level < 4; ++level) {
        for (let i = 0; i < field.normals.length; ++i) assert.ok(Math.abs(field.normals[i] - expected[i % 3]) < 1e-6);
        field = downsampleNormals(field.normals, field.width, field.depth);
    }
});

test('padded RGB tiles preserve interiors and share real neighboring samples at every level', () => {
    const width = 257;
    const heights = new Uint16Array(width * width);
    for (let z = 0; z < width; ++z) {
        for (let x = 0; x < width; ++x) {
            heights[z * width + x] = 2000 + Math.round((x * x + z * z + 2 * x * z) * 0.1);
        }
    }
    let field = { normals: normalsFromHeights(heights, width, width, 1, 3000), width, depth: width };
    for (let level = 0; level < 3; ++level) {
        const resolution = (field.width + 1) / 2;
        const edge = resolution - 1;
        const padded = resolution + 2 * NORMAL_BORDER;
        const tiles = [0, 1, 2, 3].map(q => PNG.sync.read(encodeTile(field.normals, field.width, field.depth, q & 1, q >> 1, resolution)));
        if (level === 0) {
            assert.equal(padded, NORMAL_RESOLUTION);
            assert.equal(padded, 145);
        }
        const pixel = (t, x, z) => [...t.data.subarray((z * padded + x) * 4, (z * padded + x) * 4 + 3)];
        for (let q = 0; q < tiles.length; ++q) {
            const t = tiles[q];
            assert.equal(t.colorType, 2);
            assert.equal(t.depth, 8);
            assert.equal(t.width, padded);
            assert.equal(t.height, padded);
            for (let z = 0; z < padded; ++z) {
                for (let x = 0; x < padded; ++x) {
                    const sx = Math.max(0, Math.min(field.width - 1, (q & 1) * edge + x - NORMAL_BORDER));
                    const sz = Math.max(0, Math.min(field.depth - 1, (q >> 1) * edge + z - NORMAL_BORDER));
                    const at = (sz * field.width + sx) * 3;
                    const expected = [0, 1, 2].map(c => Math.round((field.normals[at + c] * 0.5 + 0.5) * 255));
                    assert.deepEqual(pixel(t, x, z), expected, 'interior and padding retain the globally filtered field');
                }
            }
        }
        for (let i = 0; i < padded; ++i) {
            for (let overlap = 0; overlap <= 2 * NORMAL_BORDER; ++overlap) {
                for (let row = 0; row < 2; ++row) {
                    assert.deepEqual(pixel(tiles[row * 2], edge + overlap, i), pixel(tiles[row * 2 + 1], overlap, i));
                }
                for (let col = 0; col < 2; ++col) {
                    assert.deepEqual(pixel(tiles[col], i, edge + overlap), pixel(tiles[col + 2], i, overlap));
                }
            }
        }
        // On a curved surface the shared boundary must include slope from BOTH
        // sides, rather than independent clamped per-tile height derivatives.
        const i = (Math.floor(field.depth / 2) * field.width + Math.floor(field.width / 2)) * 3;
        assert.ok(field.normals[i] < -0.1 && field.normals[i+2] < -0.1);
        field = downsampleNormals(field.normals, field.width, field.depth);
    }
});

test('coarse normals average vectors instead of subsampling fine detail', () => {
    const src = new Float32Array(9 * 9 * 3);
    for (let z = 0; z < 9; ++z) for (let x = 0; x < 9; ++x) {
        const i = (z*9+x)*3; src[i] = x % 2 === 0 ? 0.8 : -0.8; src[i+1] = 0.6;
    }
    const { normals, width } = downsampleNormals(src, 9, 9);
    for (let z = 1; z < width-1; ++z) for (let x = 1; x < width-1; ++x) {
        const i = (z*width+x)*3;
        assert.ok(Math.abs(normals[i]) < 1e-6);
        assert.ok(Math.abs(normals[i+1]-1) < 1e-6);
    }
});

'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { PNG } = require('pngjs');
const { normalsFromHeights, downsampleNormals, encodeTile } = require('./normalmap-tiles');

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

test('RGB PNG tiles share identical borders and corners on a curved height field', () => {
    const width = 33, heights = new Uint16Array(width * width);
    for (let z = 0; z < width; ++z) for (let x = 0; x < width; ++x) heights[z * width + x] = 2000 + x*x + z*z + 2*x*z;
    let field = { normals: normalsFromHeights(heights, width, width, 1, 3000), width, depth: width };
    for (let level = 0; level < 3; ++level) {
        const resolution = (field.width + 1) / 2;
        const tiles = [0, 1, 2, 3].map(q => PNG.sync.read(encodeTile(field.normals, field.width, q & 1, q >> 1, resolution)));
        for (const t of tiles) { assert.equal(t.colorType, 2); assert.equal(t.depth, 8); assert.equal(t.width, resolution); }
        const pixel = (t, x, z) => [...t.data.subarray((z*resolution+x)*4, (z*resolution+x)*4+3)];
        for (let i = 0; i < resolution; ++i) {
            for (let z = 0; z < 2; ++z) assert.deepEqual(pixel(tiles[z*2], resolution-1, i), pixel(tiles[z*2+1], 0, i));
            for (let x = 0; x < 2; ++x) assert.deepEqual(pixel(tiles[x], i, resolution-1), pixel(tiles[x+2], i, 0));
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

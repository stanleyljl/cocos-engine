'use strict';

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { resampleColorMap } = require('./global-color-map');

test('global color map preserves image orientation and removes source alpha', () => {
    const source = { width: 2, height: 2, data: Buffer.from([
        255, 0, 0, 0, 0, 255, 0, 20,
        0, 0, 255, 100, 255, 255, 255, 255,
    ]) };
    const result = resampleColorMap(source, 2);
    for (let pixel = 0; pixel < 4; ++pixel) {
        assert.deepEqual(result.data.subarray(pixel * 4, pixel * 4 + 3), source.data.subarray(pixel * 4, pixel * 4 + 3));
        assert.equal(result.data[pixel * 4 + 3], 255);
    }
});

test('downsampling averages in linear space, including odd-sized image edges', () => {
    const source = { width: 3, height: 3, data: Buffer.alloc(3 * 3 * 4, 0) };
    // Three white texels in the final column must contribute exactly 1/3.
    for (let y = 0; y < 3; ++y) source.data.fill(255, (y * 3 + 2) * 4, (y * 3 + 3) * 4);
    const result = resampleColorMap(source, 1);
    assert.deepEqual([...result.data], [156, 156, 156, 255]);
});

test('global color map rejects unsupported sizes', () => {
    const source = { width: 1, height: 1, data: Buffer.alloc(4, 255) };
    for (const size of [0, -1, 0.5, 4097, NaN]) assert.throws(() => resampleColorMap(source, size));
});

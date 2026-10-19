'use strict';

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { resampleColorMap, createBiomeMap } = require('./global-color-map');

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


test('biome data is deterministic, bounded and does not transfer white mountains or blue water', () => {
    const source = { width: 3, height: 3, data: Buffer.alloc(36, 255) };
    const heightmap = { width: 3, height: 3, depth: 16, data: new Uint16Array(36).fill(10000) };
    const options = { resolution: 3, width: 192, depth: 192, heightScale: 3000 };
    source.data.set([40, 80, 240, 255], 0);
    const result = createBiomeMap(source, heightmap, options);
    assert.deepEqual(result.data, createBiomeMap(source, heightmap, options).data);
    for (let i = 0; i < 9; ++i) {
        for (let c = 0; c < 3; ++c) {
            assert.ok(result.data[i * 4 + c] >= 100 && result.data[i * 4 + c] <= 148);
        }
        assert.ok(result.data[i * 4 + 3] > 0);
    }
    // A flat white source and blue water both carry no vegetation signal.
    assert.deepEqual(result.data.subarray(0, 4), result.data.subarray(32, 36));
    heightmap.data[16] = 65535;
    const mountain = createBiomeMap(source, heightmap, options);
    assert.equal(mountain.data[19], 0, 'steep/high summit suppresses modulation');
    assert.throws(() => createBiomeMap(source, heightmap, { ...options, width: 0 }));
});

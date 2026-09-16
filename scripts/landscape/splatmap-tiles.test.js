'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { classifySplat } = require('./splatmap-tiles');

function weights (packed) {
    const result = new Float64Array(15);
    const a = packed & 31, b = (packed >>> 5) & 31, t = (packed >>> 10) / 63;
    assert.ok(a < 15 && b < 15 && a !== 12 && b !== 12, 'Only natural demo materials are allowed');
    result[a] += 1 - t;
    result[b] += t;
    return result;
}

test('regional overlays retain substrate after two-layer reduction and quantization', () => {
    let regional = 0;
    for (let z = 0; z <= 4096; z += 16) {
        for (let x = 0; x <= 4096; x += 16) {
            const packed = classifySplat(x, z, 0.35, 5, 1337, true);
            const w = weights(packed);
            assert.ok(w[13] <= 53 / 63 && w[14] <= 53 / 63, 'Regional material must not cover all substrate');
            if (w[13] + w[14] > 0.5) ++regional;
        }
    }
    assert.ok(regional > 1000, 'Regional ecological coverage must remain visible');
});

test('altitude sequence stays continuous across material-pair changes', () => {
    let previous = weights(classifySplat(715, 923, 0, 0, 1337, false));
    for (let i = 1; i <= 10000; ++i) {
        const next = weights(classifySplat(715, 923, i / 10000, 0, 1337, false));
        const change = next.reduce((sum, w, layer) => sum + Math.abs(w - previous[layer]), 0);
        assert.ok(change <= 2 / 63 + 1e-9, 'No hard minimum-weight seam when an ID pair changes');
        previous = next;
    }
});

test('altitude extremes do not collapse into broad pure-layer plateaus', () => {
    for (const elevation of [0, 1]) {
        for (const slope of [0, 60, 85]) {
            let pure = 0, total = 0;
            for (let x = 0; x < 4096; x += 17) {
                const w = weights(classifySplat(x, x * 0.5, elevation, slope, 1337, false));
                if (Math.max(...w) === 1) ++pure;
                ++total;
            }
            assert.ok(pure / total < 0.05, 'Pure crossings may remain, but not large saturated plateaus');
        }
    }
});

test('fixed coordinates and seed regenerate the same splats with spatial variation', () => {
    const values = new Set();
    let changedSeed = 0;
    for (let x = 0; x < 1024; ++x) {
        const packed = classifySplat(x, 456, 0.35, 5, 1337, true);
        assert.equal(packed, classifySplat(x, 456, 0.35, 5, 1337, true));
        values.add(packed);
        if (packed !== classifySplat(x, 456, 0.35, 5, 42, true)) ++changedSeed;
    }
    assert.ok(values.size > 30, 'Flat regions should have local weight variation');
    assert.ok(changedSeed > 512, 'Authored seed must affect the distribution');
});

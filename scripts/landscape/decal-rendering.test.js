'use strict';

// Compile builtin-landscape-vt-compose with Creator's Effect compiler first.
// LANDSCAPE_EFFECT_JSON points to that compiler's JSON output (including glsl3).
// CHROME_PATH points to Chrome/Chromium. No editor or running project is changed.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { spawnSync } = require('node:child_process');

const configured = process.env.LANDSCAPE_EFFECT_JSON && process.env.LANDSCAPE_BASE_EFFECT_JSON && process.env.CHROME_PATH;
test('RVT decal material and conforming near mesh on WebGL2', {
    skip: configured ? false : 'Set LANDSCAPE_EFFECT_JSON, LANDSCAPE_BASE_EFFECT_JSON and CHROME_PATH to run the GPU checks',
}, () => {
    const effect = JSON.parse(fs.readFileSync(process.env.LANDSCAPE_EFFECT_JSON, 'utf8'));
    assert.equal(effect.shaders.length, 2, 'Compile both compose and mip techniques');
    assert.doesNotMatch(effect.shaders[0].glsl3.frag, /v_source.w < 0.0/, 'VT contains only normal terrain composition');
    const baseEffect = JSON.parse(fs.readFileSync(process.env.LANDSCAPE_BASE_EFFECT_JSON, 'utf8'));
    const source = fs.readFileSync(path.join(__dirname, '../../editor/assets/effects/builtin-landscape.effect'), 'utf8');
    const vertexChunk = source.split('CCProgram landscape-vertex %{')[1].split('}%')[0];
    const harness = fs.readFileSync(path.join(__dirname, 'decal-rendering.webgl.js'), 'utf8');
    new (require('node:vm').Script)(harness); // Report syntax errors before launching Chrome.
    const prefix = path.join(os.tmpdir(), 'landscape-decal-test-');
    const directory = fs.mkdtempSync(prefix);
    try {
        const html = path.join(directory, 'test.html');
        fs.writeFileSync(html, '<pre id="result">RUNNING</pre><script>const effect='
            + JSON.stringify(effect).replace(/</g, '\\u003c') + ';const baseEffect='
            + JSON.stringify(baseEffect).replace(/</g, '\\u003c') + ';const vertexChunk='
            + JSON.stringify(vertexChunk).replace(/</g, '\\u003c') + ';\n' + harness + '</script>');
        const run = spawnSync(process.env.CHROME_PATH, [
            '--headless', '--no-sandbox', '--disable-gpu-sandbox', '--use-angle=swiftshader',
            '--enable-unsafe-swiftshader', '--no-first-run', '--user-data-dir=' + path.join(directory, 'profile'),
            '--dump-dom', pathToFileURL(html).href,
        ], { encoding: 'utf8', timeout: 45000, windowsHide: true, maxBuffer: 4 * 1024 * 1024 });
        assert.ifError(run.error);
        assert.equal(run.status, 0, run.stderr);
        const result = run.stdout.match(/<pre id="result">([\s\S]*?)<\/pre>/);
        assert.ok(result, run.stderr);
        assert.match(result[1], /^PASS:/);
        console.log(result[1]);
    } finally {
        // Only remove the unique directory allocated for this test invocation.
        assert.ok(path.resolve(directory).startsWith(path.resolve(prefix)));
        fs.rmSync(directory, { recursive: true, force: true, maxRetries: 3 });
    }
});

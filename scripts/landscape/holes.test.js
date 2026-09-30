'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');
const { pathToFileURL } = require('node:url');

test('Landscape hole shaders: color, shadow, decals, LOD masks and NaN probe', {
    skip: process.env.LANDSCAPE_BASE_EFFECT_JSON && process.env.CHROME_PATH ? false : 'Compile Effect and set LANDSCAPE_BASE_EFFECT_JSON and CHROME_PATH',
}, () => {
    const source = fs.readFileSync(path.join(__dirname, '../../editor/assets/effects/builtin-landscape.effect'), 'utf8');
    const chunk = name => source.split(`CCProgram ${name} %{`)[1].split('}%')[0];
    const sampling = chunk('landscape-hole-sampling');
    const fixture = {
        effect: JSON.parse(fs.readFileSync(process.env.LANDSCAPE_BASE_EFFECT_JSON, 'utf8')),
        vertex: chunk('landscape-vertex').replace('#include <landscape-hole-sampling>', sampling),
        fragment: chunk('landscape-hole-fragment').replace('#include <landscape-hole-sampling>', sampling),
    };
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'landscape-holes-'));
    try {
        const html = path.join(directory, 'test.html');
        fs.writeFileSync(html, '<pre id="result">RUNNING</pre><script>const fixture='
            + JSON.stringify(fixture).replace(/</g, '\\u003c') + ';\n'
            + fs.readFileSync(path.join(__dirname, 'holes.webgl.js'), 'utf8') + '</script>');
        const run = spawnSync(process.env.CHROME_PATH, ['--headless', '--no-sandbox', '--disable-gpu-sandbox',
            '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--no-first-run',
            '--user-data-dir=' + path.join(directory, 'profile'), '--dump-dom', pathToFileURL(html).href],
        { encoding: 'utf8', timeout: 45000, windowsHide: true, maxBuffer: 4 * 1024 * 1024 });
        assert.ifError(run.error);
        assert.equal(run.status, 0, run.stderr);
        const result = run.stdout.match(/<pre id="result">([\s\S]*?)<\/pre>/);
        assert.ok(result, run.stderr); assert.match(result[1], /^PASS:/); console.log(result[1]);
    } finally {
        assert.ok(path.resolve(directory).startsWith(path.resolve(path.join(os.tmpdir(), 'landscape-holes-'))));
        fs.rmSync(directory, { recursive: true, force: true, maxRetries: 3 });
    }
});

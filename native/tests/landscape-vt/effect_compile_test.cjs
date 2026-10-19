// Run with Creator's Electron in ELECTRON_RUN_AS_NODE=1 mode:
// CocosCreator.exe effect_compile_test.cjs <Creator resources/app.asar> <engine root>
const fs = require('fs');
const path = require('path');
const { createRequire } = require('module');

async function main() {
    const [editor, engine] = process.argv.slice(2);
    if (!editor || !engine) throw new Error('Expected editor app.asar and engine root');
    const editorRequire = createRequire(path.join(editor, 'index.js'));
    // The editor's offline engine bundle probes browser capabilities at import.
    const { JSDOM } = require(path.join(engine, 'node_modules/jsdom'));
    const { window } = new JSDOM('<div id="GameDiv"><canvas id="GameCanvas"></canvas></div>', { url: 'http://localhost/' });
    window.HTMLCanvasElement.prototype.getContext = () => null;
    window.HTMLCanvasElement.prototype.toDataURL = () => '';
    Object.assign(global, { window, document: window.document, navigator: window.navigator });
    Object.assign(global, { cc: {}, CC_EDITOR: false, CC_DEV: false, CC_TEST: false });
    await editorRequire('cc/preload').default({
        root: engine, editorExtensions: false, requiredModules: ['cc/editor/offline-mappings'],
    });
    const compiler = require(path.join(editor,
        'modules/engine-extensions/extensions/engine-extends/static/effect-compiler'));
    compiler.options.throwOnError = true;
    compiler.options.skipParserTest = false;
    compiler.options.chunkSearchFn = (names) => {
        for (const name of names) {
            const file = path.join(engine, 'editor/assets/chunks', `${name}.chunk`);
            if (fs.existsSync(file)) return { name, content: fs.readFileSync(file, 'utf8') };
        }
        return {};
    };
    const dependencies = JSON.parse(fs.readFileSync(path.join(engine, 'cc.config.json'), 'utf8'))
        .features.landscape.dependentAssets;
    const effectNames = new Set();
    for (const name of ['builtin-landscape', 'builtin-landscape-decal', 'builtin-landscape-vt-compose', 'builtin-landscape-vt-compress']) {
        const source = fs.readFileSync(path.join(engine, 'editor/assets/effects/landscape', `${name}.effect`), 'utf8');
        const effect = compiler.buildEffect(`landscape/${name}`, source);
        if (!effect || !effect.shaders.length) throw new Error(`No shaders produced for ${name}`);
        effectNames.add(effect.name);
        const meta = JSON.parse(fs.readFileSync(path.join(engine,
            'editor/assets/effects/landscape', `${name}.effect.meta`), 'utf8'));
        if (!dependencies.includes(meta.uuid)) throw new Error(`${name} is missing from Landscape build dependencies`);
        if (process.env.LANDSCAPE_EFFECT_OUTPUT) {
            fs.mkdirSync(process.env.LANDSCAPE_EFFECT_OUTPUT, { recursive: true });
            fs.writeFileSync(path.join(process.env.LANDSCAPE_EFFECT_OUTPUT, `${name}.json`), JSON.stringify(effect));
        }
        console.log(`PASS: ${name}, ${effect.shaders.length} shader programs`);
    }
    // Moving builtin effects changes their runtime names, even when UUIDs stay fixed.
    for (const file of ['LandscapeRenderer.cpp', 'VTRenderer.cpp', 'DecalRenderer.cpp']) {
        const source = fs.readFileSync(path.join(engine, 'native/cocos/landscape', file), 'utf8');
        for (const match of source.matchAll(/"(landscape\/builtin-landscape[^"\s]*)"/g)) {
            if (!effectNames.has(match[1])) throw new Error(`${file} references missing effect ${match[1]}`);
        }
    }
    console.log('PASS: native effect names and builtin build dependencies');
}
main().catch((error) => { console.error(error.message || error); process.exitCode = 1; });

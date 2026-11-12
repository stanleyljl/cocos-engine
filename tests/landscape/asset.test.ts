import downloader from '../../cocos/asset/asset-manager/downloader';
import { LandscapeAsset } from '../../cocos/landscape/Asset';
import { captureErrors } from '../utils/log-capture';

function manifest () {
    return {
        sectorCount: [1, 1],
        maxTileLevel: 0,
        minTileLevel: 0,
        sectorSize: 4,
        heightScale: 20,
        heightBias: -5,
        levels: [{ heightRange: [[0, 65535]] }],
        files: { 'materials/color.png': '.lsraw0', 'materials/normal.png': '.lsraw1', 'decal/color.png': '.lsraw2', 'decal/normal.png': '.lsraw3', 'decal/height.png': '.lsraw4' },
        materialLibrary: {
            resolution: 4,
            dir: 'materials',
            layers: [{ name: 'soil', albedoHeight: 'color.png', normalRoughnessAO: 'normal.png', tileSize: 0.25, globalColorInfluence: 0.5 }],
        },
        decalLibrary: {
            resolution: 4,
            nearDistance: 2,
            farDistance: 8,
            layers: [{ albedoAlpha: 'decal/color.png', normalRoughnessAO: 'decal/normal.png', height: 'decal/height.png', heightScale: 0, detailMaterials: [0, 0] }],
        },
        decals: [{ x: 1, z: 2, size: 1, layer: 0 }],
    };
}

afterEach(() => jest.restoreAllMocks());

test('maximum decal height follows successful loads and survives rejected replacements', async () => {
    const populated = manifest();
    const layer = populated.decalLibrary.layers[0];
    Object.assign(layer, { detailMaterials: undefined });
    populated.decalLibrary.layers = [0.5, 2, 1].map((heightScale) => ({ ...layer, heightScale }));
    let document: object = populated;
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, document));
    const asset = new LandscapeAsset();
    const url = 'https://example.test/data/terrain.lsmanifest';
    expect(await asset.load(url)).toBe(true);
    expect(asset.maxDecalHeight()).toBe(2);

    const rejected = manifest();
    rejected.decals[0].layer = 1;
    document = rejected;
    const errors = captureErrors();
    expect(await asset.load(url)).toBe(false);
    expect(asset.maxDecalHeight()).toBe(2);
    errors.stop();

    const lower = manifest();
    Object.assign(lower.decalLibrary.layers[0], { detailMaterials: undefined });
    lower.decalLibrary.layers[0].heightScale = 0.25;
    document = lower;
    expect(await asset.load(url)).toBe(true);
    expect(asset.maxDecalHeight()).toBe(0.25);

    const { decalLibrary, decals, ...withoutDecals } = manifest();
    document = withoutDecals;
    expect(await asset.load(url)).toBe(true);
    expect(asset.maxDecalHeight()).toBe(0);
    asset.destroy();
});

test.each([undefined, [], [[0]], [[0, 1, 2]], [['bad', 65535]]].map((heightRange) => ({ heightRange })))('invalid height range prevents publication (%j)', async ({ heightRange }) => {
    const document = manifest();
    Object.assign(document.levels[0], { heightRange });
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, document));
    const errors = captureErrors();
    const asset = new LandscapeAsset();
    expect(await asset.load('https://example.test/data/terrain.lsmanifest')).toBe(false);
    expect(asset.data().valid()).toBe(false);
    expect(asset.heightRanges()).toEqual([]);
    errors.stop();
    asset.destroy();
});

test('all manifest sections commit together with imported paths, query suffixes and material references', async () => {
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, manifest()));
    const asset = new LandscapeAsset();
    expect(await asset.load('https://example.test/data/terrain.lsmanifest?v=3')).toBe(true);
    expect(asset.data().width()).toBe(4);
    expect(asset.heightRanges()).toEqual([[{ minY: -5, maxY: 15 }]]);
    expect(asset.materialResolution()).toBe(4);
    expect(asset.materialLayers()[0]).toMatchObject({ id: 0, tileSize: 0.25, globalColorInfluence: 0.5, albedoHeight: 'https://example.test/data/terrain.lsraw0?v=3' });
    expect(asset.decalLayers()[0]).toMatchObject({ detailMaterial0: 0, detailMaterial1: 0, modulateColor: false, height: 'https://example.test/data/terrain.lsraw4?v=3' });
    expect(asset.decals()).toEqual([{ x: 1, z: 2, size: 1, layer: 0 }]);
    expect([asset.decalResolution(), asset.decalNearDistance(), asset.decalFarDistance()]).toEqual([4, 2, 8]);
    asset.destroy();
});

test('height ranges use global row order within each level across non-square sectors', async () => {
    const document = manifest();
    document.sectorCount = [2, 3];
    document.maxTileLevel = 1;
    document.heightScale = 65535;
    document.heightBias = -5;
    document.levels = [
        { heightRange: Array.from({ length: 24 }, (_, index) => [index * 2, index * 2 + 1]) },
        { heightRange: Array.from({ length: 6 }, (_, index) => [100 + index * 2, 101 + index * 2]) },
    ];
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, document));
    const asset = new LandscapeAsset();
    expect(await asset.load('https://example.test/data/terrain.json')).toBe(true);
    expect(asset.heightRanges().map((level) => level.length)).toEqual([24, 6]);
    for (let level = 0; level <= 1; ++level) {
        const width = asset.data().nodesX(level);
        for (let z = 0; z < asset.data().nodesZ(level); ++z) {
            for (let x = 0; x < width; ++x) {
                const minY = (level === 0 ? 0 : 100) + (z * width + x) * 2 - 5;
                expect(asset.getHeightRange(level, x, z)).toEqual({ minY, maxY: minY + 1 });
            }
        }
        expect(asset.getHeightRange(level, width, 0)).toBeUndefined();
        expect(asset.getHeightRange(level, 0, asset.data().nodesZ(level))).toBeUndefined();
    }
    expect(asset.getHeightRange(-1, 0, 0)).toBeUndefined();
    expect(asset.getHeightRange(2, 0, 0)).toBeUndefined();
    expect(asset.getHeightRange(0, -1, 0)).toBeUndefined();
    asset.destroy();
});

test('source manifests retain relative paths and optional decal defaults', async () => {
    const { decalLibrary, decals, ...document } = manifest();
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, document));
    const asset = new LandscapeAsset();
    expect(await asset.load('https://example.test/data/terrain.json')).toBe(true);
    expect(asset.materialLayers()[0].albedoHeight).toBe('https://example.test/data/materials/color.png');
    expect(asset.decalLayers()).toEqual([]);
    expect(asset.decals()).toEqual([]);
    expect([asset.decalResolution(), asset.decalNearDistance(), asset.decalFarDistance()]).toEqual([1, 12, 40]);
    asset.destroy();
});

test('a rejected late section does not publish partially parsed asset data', async () => {
    const document = manifest();
    document.decals[0].layer = 1;
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, document));
    const errors = captureErrors();
    const asset = new LandscapeAsset();
    expect(await asset.load('https://example.test/data/terrain.lsmanifest')).toBe(false);
    expect(asset.loadingFailed()).toBe(true);
    expect(asset.data().valid()).toBe(false);
    expect(asset.materialLayers()).toEqual([]);
    expect(asset.decalLayers()).toEqual([]);
    expect(asset.heightRanges()).toEqual([]);
    expect(errors.captured).toHaveLength(1);
    errors.stop();
    asset.destroy();
});

test('a cancelled manifest load cannot publish its completed parse', async () => {
    let complete!: (error: Error | null, value: object) => void;
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, callback) => {
        complete = callback;
    });
    const asset = new LandscapeAsset();
    const pending = asset.load('https://example.test/data/terrain.lsmanifest');
    asset.destroy();
    complete(null, manifest());
    expect(await pending).toBe(false);
    expect(asset.data().valid()).toBe(false);
    expect(asset.materialLayers()).toEqual([]);
});

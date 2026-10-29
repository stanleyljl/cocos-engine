import downloader from '../../cocos/asset/asset-manager/downloader';
import { LandscapeAsset } from '../../cocos/landscape/Asset';
import { captureErrors } from '../utils/log-capture';

function manifest () {
    return {
        sectorCount: [1, 1],
        maxLevel: 0,
        minTileLevel: 0,
        sectorSizeMeters: 4,
        heightScale: 20,
        heightBias: -5,
        levels: [{ heightRange: { min: [0], max: [65535] } }],
        files: { 'materials/color.png': '.lsraw0', 'materials/normal.png': '.lsraw1', 'decal/color.png': '.lsraw2', 'decal/normal.png': '.lsraw3', 'decal/height.png': '.lsraw4' },
        materialLibrary: {
            resolution: 4,
            dir: 'materials',
            layers: [{ name: 'soil', albedoHeight: 'color.png', normalRoughnessAO: 'normal.png', pixelsPerMeter: 16, globalColorInfluence: 0.5 }],
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

test('all manifest sections commit together with imported paths, query suffixes and material references', async () => {
    jest.spyOn(downloader, '_downloadJson').mockImplementation((_url, _options, complete) => complete(null, manifest()));
    const asset = new LandscapeAsset();
    expect(await asset.load('https://example.test/data/terrain.lsmanifest?v=3')).toBe(true);
    expect(asset.data().width()).toBe(4);
    expect(asset.heightRanges()).toEqual([{ minY: -5, maxY: 15 }]);
    expect(asset.materialResolution()).toBe(4);
    expect(asset.materialLayers()[0]).toMatchObject({ id: 0, pixelsPerMeter: 16, globalColorInfluence: 0.5, albedoHeight: 'https://example.test/data/terrain.lsraw0?v=3' });
    expect(asset.decalLayers()[0]).toMatchObject({ detailMaterial0: 0, detailMaterial1: 0, modulateColor: false, height: 'https://example.test/data/terrain.lsraw4?v=3' });
    expect(asset.decals()).toEqual([{ x: 1, z: 2, size: 1, layer: 0 }]);
    expect([asset.decalResolution(), asset.decalNearDistance(), asset.decalFarDistance()]).toEqual([4, 2, 8]);
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

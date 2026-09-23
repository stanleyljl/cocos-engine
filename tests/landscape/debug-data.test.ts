import { CCClass } from '../../cocos/core';
import { Landscape, LandscapeDebugData } from '../../cocos/landscape';
import { deserialize } from '../../cocos/serialization/deserialize';
import { Texture2D } from '../../cocos/asset/assets';

describe('Landscape debug data', () => {
    test('nested scene data preserves all switches and component properties', () => {
        const data = new LandscapeDebugData();
        for (const key of Object.keys(data) as (keyof LandscapeDebugData)[]) data[key] = !data[key];
        const terrain: Landscape = deserialize({
            __type__: 'cc.Landscape', _enabled: false, _lodQualityScale: 2,
            _debugData: { __type__: 'cc.LandscapeDebugData', ...data },
        });
        expect(terrain.debugData).toBeInstanceOf(LandscapeDebugData);
        expect(terrain.debugData).toEqual(data);
        expect(terrain.enabled).toBe(false);
        expect(terrain.lodQualityScale).toBe(2);
    });

    test('debug data objects do not share defaults', () => {
        const first: Landscape = deserialize({
            __type__: 'cc.Landscape',
            _debugData: { __type__: 'cc.LandscapeDebugData', wireframe: false, heightBlendEnabled: false },
        });
        const second = new Landscape();
        expect(first.debugData.wireframe).toBe(false);
        expect(first.debugData.heightBlendEnabled).toBe(false);
        first.debugData.cliffEnabled = false;
        expect(second.debugData.cliffEnabled).toBe(true);
        expect(second.debugData.heightBlendEnabled).toBe(true);
    });

    test('only the aggregate debug data is serialized on the component', () => {
        const properties = (Landscape as unknown as { __values__: string[] }).__values__;
        expect(properties).toContain('_debugData');
        for (const key of Object.keys(new LandscapeDebugData())) {
            expect(properties).not.toContain(key);
            expect(Object.getOwnPropertyDescriptor(Landscape.prototype, key)).toBeUndefined();
            expect(CCClass.Attr.getClassAttrs(LandscapeDebugData)[`${key}$_$visible`]).not.toBe(false);
        }
    });

    test('nested edits synchronize once before update, including during warmup and re-enable', () => {
        const terrain = new Landscape();
        const submitted: LandscapeDebugData[] = [];
        const native = {
            setDebugData: jest.fn(data => submitted.push({ ...data })),
            setAssetPath: jest.fn(), setGlobalColorStrength: jest.fn(), setGlobalColorMap: jest.fn(),
            setCastShadow: jest.fn(), setReceiveShadow: jest.fn(), setQueryCacheCapacity: jest.fn(),
            onEnable: jest.fn(), onDisable: jest.fn(),
            isInitialized: jest.fn(() => true), update: jest.fn(),
            drawDebugBounds: jest.fn(),
        };
        const internal = terrain as unknown as { _native: typeof native; _beforeDraw(): void };
        internal._native = native;
        terrain.debugData.freezeLod = true;
        terrain.debugData.heightBlendEnabled = false;
        terrain.onEnable();
        expect(native.setGlobalColorMap).toHaveBeenLastCalledWith(null);
        expect(submitted).toHaveLength(1);
        expect(submitted[0].freezeLod).toBe(true);
        expect(submitted[0].heightBlendEnabled).toBe(false);
        internal._beforeDraw();
        internal._beforeDraw();
        expect(submitted).toHaveLength(1);

        terrain.debugData.bakeNormalEnabled = false;
        terrain.debugData.showBox = true;
        terrain.debugData.showRanges = true;
        internal._beforeDraw();
        expect(submitted).toHaveLength(2);
        expect(submitted[1].bakeNormalEnabled).toBe(false);
        expect(submitted[1].showRanges).toBe(true);
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(1);

        terrain.debugData.wireframe = true;
        terrain.debugData.showRanges = false;
        internal._beforeDraw();
        expect(submitted[2].wireframe).toBe(true);
        expect(submitted[2].showRanges).toBe(false);
        expect(submitted).toHaveLength(3);
        terrain.onDisable();
        terrain.onEnable();
        expect(submitted).toHaveLength(4);
        expect(submitted[3]).toEqual(submitted[2]);
        terrain.onDisable();
    });

    test('optional global color map can be assigned and cleared; strength stays bounded', () => {
        const terrain = new Landscape();
        const native = { setGlobalColorMap: jest.fn(), setGlobalColorStrength: jest.fn() };
        (terrain as unknown as { _native: typeof native })._native = native;
        expect(terrain.globalColorMap).toBeNull();
        const texture = new Texture2D();
        terrain.globalColorMap = texture;
        expect(terrain.globalColorMap).toBe(texture);
        expect(native.setGlobalColorMap).toHaveBeenLastCalledWith(texture);
        terrain.globalColorMap = null;
        expect(native.setGlobalColorMap).toHaveBeenLastCalledWith(null);
        terrain.globalColorStrength = 2;
        expect(terrain.globalColorStrength).toBe(1);
        terrain.globalColorStrength = -1;
        expect(terrain.globalColorStrength).toBe(0);
        terrain.globalColorStrength = NaN;
        expect(terrain.globalColorStrength).toBe(0);
        texture.destroy();
    });
});

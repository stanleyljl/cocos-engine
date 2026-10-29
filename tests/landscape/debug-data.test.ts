import { CCClass } from '../../cocos/core';
import { Landscape, LandscapeDebugData, LandscapeHoleMode } from '../../cocos/landscape';
import { deserialize } from '../../cocos/serialization/deserialize';
import { Texture2D } from '../../cocos/asset/assets';
import { Component } from '../../cocos/scene-graph/component';

describe('Landscape debug data', () => {
    test.each([undefined, true, false])('diagnostics honor the startup override %s without changing visual controls', (enabled) => {
        const previous = globalThis.CC_LANDSCAPE_DEBUG;
        try {
            globalThis.CC_LANDSCAPE_DEBUG = enabled;
            jest.isolateModules(() => {
                const config = require('../../cocos/landscape/Config');
                expect(config.LANDSCAPE_DEBUG).toBe(enabled ?? true);
            });
            expect(new LandscapeDebugData().triplanarEnabled).toBe(true);
            expect(new LandscapeDebugData().heightBlendEnabled).toBe(true);
        } finally {
            globalThis.CC_LANDSCAPE_DEBUG = previous;
        }
    });

    test.each(['en', 'zh'])('all visible Landscape properties have matching Inspector tooltips in %s', (language) => {
        const catalog = { ENGINE: require(`../../editor/i18n/${language}/localization.js`) };
        const inherited = new Set((Component as unknown as { __props__: string[] }).__props__);
        const groups = [
            { type: Landscape, name: 'Landscape', count: 9 },
            { type: LandscapeDebugData, name: 'LandscapeDebugData', count: 9 },
        ];
        for (const { type, name, count } of groups) {
            const attributes = CCClass.Attr.getClassAttrs(type);
            const properties = (type as unknown as { __props__: string[] }).__props__.filter(property => (
                !inherited.has(property) && attributes[`${property}$_$visible`] !== false
            ));
            expect(properties).toHaveLength(count);
            for (const property of properties) {
                const tooltip = attributes[`${property}$_$tooltip`] as string;
                expect(tooltip).toBe(`i18n:ENGINE.classes.cc.${name}.properties.${property}.tooltip`);
                const text = tooltip.slice('i18n:'.length).split('.').reduce((value, key) => value?.[key], catalog);
                expect(typeof text).toBe('string');
                expect(text).not.toBe('');
                expect(text).not.toMatch(/^(i18n:|ENGINE\.)/);
                const displayName = attributes[`${property}$_$displayName`] as string | undefined;
                if (displayName?.startsWith('i18n:')) {
                    const label = displayName.slice('i18n:'.length).split('.').reduce((value, key) => value?.[key], catalog);
                    expect(typeof label).toBe('string');
                    expect(label).not.toBe('');
                }
            }
        }
    });

    test('hole mode defaults to Fast, serializes and immediately updates native', () => {
        const terrain = new Landscape();
        expect(terrain.holeMode).toBe(LandscapeHoleMode.Fast);
        const native = { setHoleMode: jest.fn() };
        (terrain as any)._runtime = native;
        terrain.holeMode = LandscapeHoleMode.HighQuality;
        expect(native.setHoleMode).toHaveBeenLastCalledWith(1);
        const loaded = deserialize({ __type__: 'cc.Landscape', _holeMode: 1 }) as Landscape;
        expect(loaded.holeMode).toBe(LandscapeHoleMode.HighQuality);
    });
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
        first.debugData.triplanarEnabled = false;
        expect(second.debugData.triplanarEnabled).toBe(true);
        expect(second.debugData.heightBlendEnabled).toBe(true);
    });

    test('only the aggregate debug data is serialized on the component', () => {
        const properties = (Landscape as unknown as { __values__: string[] }).__values__;
        expect(properties).toContain('_debugData');
        for (const key of Object.keys(new LandscapeDebugData())) {
            expect(properties).not.toContain(key);
            expect(Object.getOwnPropertyDescriptor(Landscape.prototype, key)).toBeUndefined();
            const hidden = key === 'showVTAtlas' || key === 'physicsWireframe';
            expect(CCClass.Attr.getClassAttrs(LandscapeDebugData)[`${key}$_$visible`] === false).toBe(hidden);
        }
    });

    test('nested edits synchronize only on changes, including during warmup and re-enable', () => {
        const terrain = new Landscape();
        const submitted: LandscapeDebugData[] = [];
        terrain.queryCacheCapacity = 16;
        const native = {
            setDebugData: jest.fn(data => submitted.push({ ...data })),
            setAssetPath: jest.fn(), setGlobalColorStrength: jest.fn(), setGlobalColorMap: jest.fn(),
            setCastShadow: jest.fn(), setReceiveShadow: jest.fn(), setHoleMode: jest.fn(),
            onEnable: jest.fn(() => Promise.resolve(true)), onDisable: jest.fn(),
            isInitialized: jest.fn(() => true), update: jest.fn(),
            drawDebugBounds: jest.fn(),
        };
        const internal = terrain as unknown as { _runtime: typeof native; _beforeDraw(): void };
        internal._runtime = native;
        terrain.debugData.unlit = true;
        terrain.debugData.heightBlendEnabled = false;
        terrain.onEnable();
        expect(native.onEnable).toHaveBeenLastCalledWith(terrain.node, terrain.lodQualityScale, 16);
        expect(native.setGlobalColorMap).toHaveBeenLastCalledWith(null);
        expect(submitted).toHaveLength(1);
        expect(submitted[0].unlit).toBe(true);
        expect(submitted[0].heightBlendEnabled).toBe(false);
        terrain.update();
        terrain.update();
        expect(submitted).toHaveLength(1);

        native.isInitialized.mockReturnValue(false);
        native.update.mockClear();
        terrain.debugData.refineFarDetail = false;
        terrain.debugData.showBox = true;
        terrain.debugData.showRanges = true;
        terrain.update();
        expect(submitted).toHaveLength(2);
        expect(submitted[1].refineFarDetail).toBe(false);
        expect(submitted[1].showRanges).toBe(true);
        expect(native.update).not.toHaveBeenCalled();

        native.isInitialized.mockReturnValue(true);
        terrain.debugData.wireframe = true;
        terrain.debugData.showRanges = false;
        terrain.update();
        expect(submitted).toHaveLength(3);
        expect(submitted[2].wireframe).toBe(true);
        expect(submitted[2].showRanges).toBe(false);
        expect(native.drawDebugBounds).not.toHaveBeenCalled();
        internal._beforeDraw();
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(1);
        expect(native.setDebugData.mock.invocationCallOrder[2]).toBeLessThan(native.update.mock.invocationCallOrder[0]);
        terrain.onDisable();
        terrain.onEnable();
        expect(native.onEnable).toHaveBeenLastCalledWith(terrain.node, terrain.lodQualityScale, 16);
        expect(submitted).toHaveLength(4);
        expect(submitted[3]).toEqual(submitted[2]);
        terrain.onDisable();
    });

    test('every debug switch detects both transitions and equal replacements do not resubmit', () => {
        const terrain = new Landscape();
        const native = { setDebugData: jest.fn(), isInitialized: jest.fn(() => false) };
        const internal = terrain as unknown as { _runtime: typeof native; _beforeDraw(): void };
        internal._runtime = native;
        terrain.update();
        for (const key of Object.keys(terrain.debugData) as (keyof LandscapeDebugData)[]) {
            for (let transition = 0; transition < 2; ++transition) {
                native.setDebugData.mockClear();
                terrain.debugData[key] = !terrain.debugData[key];
                terrain.update();
                expect(native.setDebugData).toHaveBeenCalledTimes(1);
                terrain.update();
                expect(native.setDebugData).toHaveBeenCalledTimes(1);
            }
        }
        native.setDebugData.mockClear();
        terrain.debugData = new LandscapeDebugData();
        expect(native.setDebugData).not.toHaveBeenCalled();
        const replacement = new LandscapeDebugData();
        replacement.wireframe = true;
        terrain.debugData = replacement;
        expect(native.setDebugData).toHaveBeenCalledTimes(1);
        terrain.update();
        expect(native.setDebugData).toHaveBeenCalledTimes(1);
    });

    test('optional global color map can be assigned and cleared; strength stays bounded', () => {
        const terrain = new Landscape();
        const native = { setGlobalColorMap: jest.fn(), setGlobalColorStrength: jest.fn() };
        (terrain as unknown as { _runtime: typeof native })._runtime = native;
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
        texture.destroy();
    });
});

import { System, SystemPriority } from '../../cocos/core';
import { director, DirectorEvent } from '../../cocos/game/director';
import { Node } from '../../cocos/scene-graph/node';
import { ComponentScheduler } from '../../cocos/scene-graph/component-scheduler';
import { Landscape } from '../../cocos/landscape';
import * as constants from 'internal:constants';

async function fixture (initialized = true) {
    const terrain = new Landscape();
    terrain.node = new Node();
    const source = new Node();
    Object.defineProperty(terrain, 'enabledInHierarchy', { get: () => true });
    Object.defineProperty(source, 'activeInHierarchy', { get: () => true });
    const native = {
        setAssetPath: jest.fn(),
        setHoleMode: jest.fn(),
        setDebugData: jest.fn(),
        setGlobalColorMap: jest.fn(),
        setGlobalColorStrength: jest.fn(),
        setCastShadow: jest.fn(),
        setReceiveShadow: jest.fn(),
        onEnable: jest.fn(() => constants.JSB ? initialized : Promise.resolve(initialized)),
        onDisable: jest.fn(),
        destroy: jest.fn(),
        isInitialized: jest.fn(() => initialized),
        addQuerySource: jest.fn(() => 1),
        removeQuerySource: jest.fn(),
        update: jest.fn(),
        drawDebugBounds: jest.fn(),
    };
    (terrain as unknown as { _runtime: typeof native })._runtime = native;
    terrain.debugData.showBox = true;
    const id = terrain.addQuerySource(source, 0);
    terrain.onEnable();
    await Promise.resolve();
    return { terrain, source, native, id };
}

describe('Landscape tick and draw scheduling', () => {
    const fixtures: Awaited<ReturnType<typeof fixture>>[] = [];
    const systems: System[] = [];

    async function create (initialized = true) {
        const result = await fixture(initialized);
        fixtures.push(result);
        return result;
    }

    afterEach(() => {
        director.resume();
        for (const system of systems.splice(0)) {
            director.unregisterSystem(system);
        }
        for (const { terrain, source } of fixtures.splice(0)) {
            terrain.onDestroy();
            terrain.node.destroy();
            source.destroy();
        }
    });

    test.each([false, true])('initialization failure does not register updates or enable physics (JSB=%s)', async (jsb) => {
        const platform = constants as { JSB: boolean };
        const previous = platform.JSB;
        platform.JSB = jsb;
        try {
            const { terrain, native } = await create(false);
            expect((terrain.physics as unknown as { _enabled: boolean })._enabled).toBe(false);
            director.tick(1 / 60);
            expect(native.update).not.toHaveBeenCalled();
            expect(native.drawDebugBounds).not.toHaveBeenCalled();
        } finally {
            platform.JSB = previous;
        }
    });

    test('a completed cancelled enable cannot register updates after disable', async () => {
        const { terrain, native } = await create();
        terrain.onDisable();
        let finish!: (initialized: boolean) => void;
        native.onEnable.mockImplementation(() => new Promise<boolean>((resolve) => {
            finish = resolve;
        }));
        terrain.onEnable();
        terrain.onDisable();
        finish(true);
        await Promise.resolve();
        director.tick(1 / 60);
        expect(native.update).not.toHaveBeenCalled();
        expect(native.drawDebugBounds).not.toHaveBeenCalled();
    });

    test('ticks after physics camera callbacks and before drawing, regardless of registration order', async () => {
        const { source, native } = await create();
        const order: string[] = [];
        const physics = new System();
        physics.postUpdate = () => {
            source.setWorldPosition(12, 0, 34);
            order.push('physics-camera');
        };
        // Register after Landscape to verify priority rather than insertion order.
        systems.push(physics);
        director.registerSystem('LANDSCAPE_TEST_PHYSICS', physics, SystemPriority.LOW);
        native.update.mockImplementation(() => {
            expect(source.worldPosition.x).toBe(12);
            expect(source.worldPosition.z).toBe(34);
            expect(native.addQuerySource).toHaveBeenCalledWith(source, 0);
            order.push('tick');
        });
        native.drawDebugBounds.mockImplementation(() => order.push('draw'));
        director.tick(1 / 60);
        expect(order).toEqual(['physics-camera', 'tick', 'draw']);
        expect(native.update).toHaveBeenCalledTimes(1);
        expect(native.addQuerySource).toHaveBeenCalledTimes(1);

        director.emit(DirectorEvent.BEFORE_DRAW);
        director.emit(DirectorEvent.BEFORE_DRAW);
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(3);
        expect(native.update).toHaveBeenCalledTimes(1);
        expect(native.addQuerySource).toHaveBeenCalledTimes(1);
    });

    test('component scheduling does not invoke update before the Landscape system', async () => {
        const { terrain, native } = await create();
        const scheduler = new ComponentScheduler();
        scheduler._onEnabled(terrain);
        try {
            scheduler.startPhase();
            scheduler.updatePhase(1 / 60);
            scheduler.lateUpdatePhase(1 / 60);
            expect(native.update).not.toHaveBeenCalled();
            director.tick(1 / 60);
            expect(native.update).toHaveBeenCalledTimes(1);
        } finally {
            scheduler._onDisabled(terrain);
        }
    });

    test('disable, re-enable and destroy remove stale registrations without duplicating ticks', async () => {
        const first = await create();
        const second = await create();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(1);
        expect(second.native.update).toHaveBeenCalledTimes(1);
        first.terrain.onDisable();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(1);
        expect(first.native.drawDebugBounds).toHaveBeenCalledTimes(1);
        expect(second.native.update).toHaveBeenCalledTimes(2);
        first.terrain.onEnable();
        await Promise.resolve();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(2);
        expect(first.native.drawDebugBounds).toHaveBeenCalledTimes(2);
        first.terrain.onDestroy();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(2);
        expect(first.native.drawDebugBounds).toHaveBeenCalledTimes(2);
        expect(second.native.update).toHaveBeenCalledTimes(4);
    });

    test('paused draws advance streaming once; resuming runs one system tick', async () => {
        const { native } = await create();
        director.pause();
        director.tick(1 / 60);
        expect(native.update).toHaveBeenCalledTimes(1);
        expect(native.addQuerySource).toHaveBeenCalledTimes(1);
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(1);
        director.resume();
        director.tick(1 / 60);
        expect(native.update).toHaveBeenCalledTimes(2);
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(2);
    });

    test('tick does not require a render camera and skips native updates until initialized', async () => {
        const { terrain, native } = await create();
        native.isInitialized.mockReturnValue(false);
        terrain.debugData.unlit = true;
        director.tick(1 / 60);
        expect(native.setDebugData).toHaveBeenLastCalledWith(terrain.debugData);
        expect(native.addQuerySource).toHaveBeenCalledTimes(1);
        expect(native.update).not.toHaveBeenCalled();
        native.isInitialized.mockReturnValue(true);
        director.tick(1 / 60);
        expect(native.update).toHaveBeenCalledTimes(1);
    });
});

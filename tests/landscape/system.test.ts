import { System, SystemPriority } from '../../cocos/core';
import { director, DirectorEvent } from '../../cocos/game/director';
import { Node } from '../../cocos/scene-graph/node';
import { ComponentScheduler } from '../../cocos/scene-graph/component-scheduler';
import { Landscape, LandscapeQueryStatus } from '../../cocos/landscape';

function fixture () {
    const terrain = new Landscape();
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
        onEnable: jest.fn(),
        onDisable: jest.fn(),
        isInitialized: jest.fn(() => true),
        setQuerySource: jest.fn(() => LandscapeQueryStatus.NotReady),
        removeQuerySource: jest.fn(),
        update: jest.fn(),
        drawDebugBounds: jest.fn(),
    };
    (terrain as unknown as { _native: typeof native })._native = native;
    terrain.debugData.showBox = true;
    const id = terrain.addQuerySource(source, 0);
    terrain.onEnable();
    return { terrain, source, native, id };
}

describe('Landscape tick and draw scheduling', () => {
    const fixtures: ReturnType<typeof fixture>[] = [];
    const systems: System[] = [];

    function create () {
        const result = fixture();
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
            source.destroy();
        }
    });

    test('ticks after physics camera callbacks and before drawing, regardless of registration order', () => {
        const { source, native, id } = create();
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
            expect(native.setQuerySource).toHaveBeenLastCalledWith(id, 12, 34, 0);
            order.push('tick');
        });
        native.drawDebugBounds.mockImplementation(() => order.push('draw'));
        director.tick(1 / 60);
        expect(order).toEqual(['physics-camera', 'tick', 'draw']);
        expect(native.update).toHaveBeenCalledTimes(1);
        expect(native.setQuerySource).toHaveBeenCalledTimes(1);

        director.emit(DirectorEvent.BEFORE_DRAW);
        director.emit(DirectorEvent.BEFORE_DRAW);
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(3);
        expect(native.update).toHaveBeenCalledTimes(1);
        expect(native.setQuerySource).toHaveBeenCalledTimes(1);
    });

    test('component scheduling does not invoke update before the Landscape system', () => {
        const { terrain, native } = create();
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

    test('disable, re-enable and destroy remove stale registrations without duplicating ticks', () => {
        const first = create();
        const second = create();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(1);
        expect(second.native.update).toHaveBeenCalledTimes(1);
        first.terrain.onDisable();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(1);
        expect(first.native.drawDebugBounds).toHaveBeenCalledTimes(1);
        expect(second.native.update).toHaveBeenCalledTimes(2);
        first.terrain.onEnable();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(2);
        expect(first.native.drawDebugBounds).toHaveBeenCalledTimes(2);
        first.terrain.onDestroy();
        director.tick(1 / 60);
        expect(first.native.update).toHaveBeenCalledTimes(2);
        expect(first.native.drawDebugBounds).toHaveBeenCalledTimes(2);
        expect(second.native.update).toHaveBeenCalledTimes(4);
    });

    test('pausing stops ticks while debug drawing continues; resuming runs one tick', () => {
        const { native } = create();
        director.pause();
        director.tick(1 / 60);
        expect(native.update).not.toHaveBeenCalled();
        expect(native.setQuerySource).not.toHaveBeenCalled();
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(1);
        director.resume();
        director.tick(1 / 60);
        expect(native.update).toHaveBeenCalledTimes(1);
        expect(native.drawDebugBounds).toHaveBeenCalledTimes(2);
    });

    test('tick does not require a render camera and skips native updates until initialized', () => {
        const { terrain, native } = create();
        native.isInitialized.mockReturnValue(false);
        terrain.debugData.unlit = true;
        director.tick(1 / 60);
        expect(native.setDebugData).toHaveBeenLastCalledWith(terrain.debugData);
        expect(native.setQuerySource).toHaveBeenCalledTimes(1);
        expect(native.update).not.toHaveBeenCalled();
        native.isInitialized.mockReturnValue(true);
        director.tick(1 / 60);
        expect(native.update).toHaveBeenCalledTimes(1);
    });
});

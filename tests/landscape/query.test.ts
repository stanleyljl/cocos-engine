import { Vec3 } from '../../cocos/core';
import { Node } from '../../cocos/scene-graph/node';
import { Landscape, LandscapeQueryStatus as Status, LandscapeSurfaceResult } from '../../cocos/landscape';

function fixture () {
    const terrain = new Landscape();
    const node = new Node();
    let active = true;
    Object.defineProperty(node, 'activeInHierarchy', { get: () => active });
    Object.defineProperty(terrain, 'enabledInHierarchy', { get: () => true });
    const native = {
        setQuerySource: jest.fn(() => Status.NotReady),
        removeQuerySource: jest.fn(),
        sampleSurface: jest.fn((_x: number, _z: number, output: Float32Array) => {
            output.set([1, 12, 3, 0, 1, 0, 7, 0.75]);
            return Status.Hit;
        }),
        isInitialized: jest.fn(() => false),
        onDisable: jest.fn(),
    };
    (terrain as unknown as { _native: typeof native })._native = native;
    return { terrain, node, native, deactivate: () => { active = false; } };
}

describe('Landscape CPU surface query API', () => {
    test('registration follows the node, updates radius and releases its handle', () => {
        const { terrain, node, native } = fixture();
        node.setPosition(4, 10, 8);
        const id = terrain.addQuerySource(node, 32);
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.NotReady);
        expect(native.setQuerySource).toHaveBeenLastCalledWith(id, 4, 8, 32);
        node.setPosition(6, 20, 9);
        expect(terrain.setQuerySourceRadius(id, 48)).toBe(true);
        native.setQuerySource.mockReturnValue(Status.Hit);
        expect(terrain.isQuerySourceReady(id)).toBe(true);
        expect(native.setQuerySource).toHaveBeenLastCalledWith(id, 6, 9, 48);
        terrain.removeQuerySource(id);
        expect(native.removeQuerySource).toHaveBeenCalledWith(id);
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.Miss);
        expect(terrain.setQuerySourceRadius(id, 8)).toBe(false);
        terrain.onDestroy(); node.destroy();
    });

    test('inactive and destroyed sources stop protecting the CPU cache', () => {
        const { terrain, node, native, deactivate } = fixture();
        const id = terrain.addQuerySource(node, 0);
        terrain.getQuerySourceStatus(id);
        deactivate();
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.NotReady);
        expect(native.removeQuerySource).toHaveBeenCalledWith(id);
        node.destroy();
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.Miss);
        expect(terrain.setQuerySourceRadius(id, 8)).toBe(false);
        terrain.onDestroy();
    });

    test('query reuses the result and does not change it when data is unavailable', () => {
        const { terrain, node, native } = fixture();
        const output = new LandscapeSurfaceResult();
        const position = output.position;
        const normal = output.normal;
        expect(terrain.sampleSurface(new Vec3(1, 999, 3), output)).toBe(Status.Hit);
        expect(output.height).toBe(12);
        expect(output.position).toBe(position);
        expect(output.normal).toBe(normal);
        expect(output.surfaceType).toBe(7);
        expect(output.surfaceWeight).toBe(0.75);
        const buffer = native.sampleSurface.mock.calls[0][2];
        native.sampleSurface.mockImplementation((_x, _z, data) => {
            data.fill(-99);
            return Status.NotReady;
        });
        expect(terrain.sampleSurface(new Vec3(2, 0, 4), output)).toBe(Status.NotReady);
        expect(native.sampleSurface.mock.calls[1][2]).toBe(buffer);
        expect(output.height).toBe(12);
        expect(output.surfaceType).toBe(7);
        const invalidPosition = new Vec3();
        invalidPosition.x = NaN;
        expect(terrain.sampleSurface(invalidPosition, output)).toBe(Status.Error);
        expect(native.sampleSurface).toHaveBeenCalledTimes(2);
        terrain.onDestroy(); node.destroy();
    });

    test('invalid budgets and radii are rejected and disabled/native-free query is nonblocking', () => {
        const { terrain, node, native } = fixture();
        expect(() => terrain.addQuerySource(node, -1)).toThrow(RangeError);
        expect(() => terrain.addQuerySource(node, Infinity)).toThrow(RangeError);
        expect(() => { terrain.queryCacheCapacity = 0; }).toThrow(RangeError);
        terrain.queryCacheCapacity = 16;
        expect(terrain.queryCacheCapacity).toBe(16);
        native.isInitialized.mockReturnValue(true);
        expect(() => { terrain.queryCacheCapacity = 32; }).toThrow();
        const id = terrain.addQuerySource(node);
        terrain.onDestroy();
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.Miss);
        expect(terrain.sampleSurface(new Vec3(), new LandscapeSurfaceResult())).toBe(Status.NotReady);
        node.destroy();
    });
});

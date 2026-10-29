import { Vec3 } from '../../cocos/core';
import { Node } from '../../cocos/scene-graph/node';
import { Landscape, LandscapeQueryStatus as Status, LandscapeSurfaceResult } from '../../cocos/landscape';
import { LandscapeData } from '../../cocos/landscape/Data';
import { LandscapeQueryTile } from '../../cocos/landscape/Data';
import { LandscapeQuery, LandscapeQueryStatus as RuntimeStatus } from '../../cocos/landscape/Query';
import * as config from '../../cocos/landscape/Config';

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
    (terrain as unknown as { _runtime: typeof native })._runtime = native;
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
        terrain.onDestroy();
        node.destroy();
    });

    test('capacity rejection is distinct from loading and the same source can recover', () => {
        const { terrain, node, native } = fixture();
        const id = terrain.addQuerySource(node, 64);
        native.setQuerySource.mockReturnValue(Status.OutOfCapacity);
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.OutOfCapacity);
        expect(terrain.isQuerySourceReady(id)).toBe(false);
        expect(terrain.setQuerySourceRadius(id, 16)).toBe(true);
        native.setQuerySource.mockReturnValue(Status.NotReady);
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.NotReady);
        expect(native.setQuerySource).toHaveBeenLastCalledWith(id, 0, 0, 16);
        expect(terrain.isQuerySourceReady(id)).toBe(false);
        native.setQuerySource.mockReturnValue(Status.Hit);
        expect(terrain.getQuerySourceStatus(id)).toBe(Status.Hit);
        expect(terrain.isQuerySourceReady(id)).toBe(true);
        terrain.onDestroy();
        node.destroy();
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
        expect(native.sampleSurface).toHaveBeenCalledTimes(2);
        terrain.onDestroy();
        node.destroy();
    });

    test('invalid budgets and radii are rejected and disabled/native-free query is nonblocking', () => {
        const { terrain, node, native } = fixture();
        expect(() => terrain.addQuerySource(node, -1)).toThrow(RangeError);
        for (const capacity of [-1, 0, 1.5, 4097]) {
            expect(() => { terrain.queryCacheCapacity = capacity; }).toThrow(RangeError);
        }
        for (const capacity of [1, 64, 4096]) {
            terrain.queryCacheCapacity = capacity;
            expect(terrain.queryCacheCapacity).toBe(capacity);
        }
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

// Same plane, padded normals, material pair and boundary expectations as
// native/tests/landscape-vt/query_test.cpp; exercises the actual Web query cache.
describe('Landscape runtime query parity', () => {
    function data (): LandscapeData {
        return Object.assign(new LandscapeData(), {
            sectorsX: 2,
            sectorsZ: 1,
            maxLevel: 1,
            minTileLevel: 0,
            sectorSize: 4,
            heightScale: 65535 * 25 / 32,
            heightBias: -100,
        });
    }

    function tile (tx: number, tz: number): LandscapeQueryTile {
        const side = config.SOURCE_TILE_RESOLUTION;
        const result: LandscapeQueryTile = {
            height: new Uint8Array(side * side * 2),
            splat: new Uint16Array(side * side),
            normal: new Uint8Array(config.SOURCE_NORMAL_RESOLUTION ** 2 * 3),
        };
        for (let z = 0; z < side; ++z) {
            for (let x = 0; x < side; ++x) {
                const height = 128 + (tx * (side - 1) + x) * 2 + tz * (side - 1) + z;
                const index = z * side + x;
                result.height[index * 2] = height >> 8;
                result.height[index * 2 + 1] = height & 255;
                result.splat[index] = 3 | (7 << 5) | (10 << 10);
                const normal = ((z + config.SOURCE_NORMAL_BORDER) * config.SOURCE_NORMAL_RESOLUTION + x + config.SOURCE_NORMAL_BORDER) * 3;
                result.normal.set([64, 220, 160], normal);
            }
        }
        return result;
    }

    test('matches native source normals, heights, materials and inclusive tile boundaries', () => {
        const loader = jest.fn((x, z, complete) => complete(tile(x, z)));
        const query = new LandscapeQuery(data(), loader, 8);
        expect(query.sample({ x: 4, z: 2 }).status).toBe(RuntimeStatus.NOT_READY);
        expect(loader).not.toHaveBeenCalled();
        expect(query.addSource(1, { x: 4, z: 2 }, 4)).toBe(RuntimeStatus.NOT_READY);
        expect(loader).toHaveBeenCalledTimes(8);
        query.update();
        expect(query.getSourceStatus(1)).toBe(RuntimeStatus.HIT);
        const hit = query.sample({ x: 0.75, z: 0.25 });
        expect(hit.position.y).toBeCloseTo(87.5, 3);
        const length = Math.hypot(127, 185, 65);
        expect(hit.normal.x).toBeCloseTo(-127 / length, 6);
        expect(hit.normal.y).toBeCloseTo(185 / length, 6);
        expect(hit.normal.z).toBeCloseTo(65 / length, 6);
        expect(hit.surfaceType).toBe(3);
        expect(hit.surfaceWeight).toBeCloseTo(21 / 31, 6);
        expect(query.sample({ x: 8, z: 4 }).position.y).toBeCloseTo(1000, 3);
        expect(query.sample({ x: 0, z: 0 }).position.y).toBeCloseTo(0, 3);
        expect(query.sample({ x: 2, z: 2 }).position.y).toBeCloseTo(300, 3);
        expect(query.sample({ x: 8.01, z: 4 }).status).toBe(RuntimeStatus.MISS);
        query.removeSource(1);
        expect(query.sample({ x: 4, z: 2 }).status).toBe(RuntimeStatus.NOT_READY);
        expect(loader).toHaveBeenCalledTimes(8);
    });

    test('capacity rejection retains old sources and late results cannot replace a new request', () => {
        const jobs: ((value: LandscapeQueryTile | null) => void)[] = [];
        const query = new LandscapeQuery(data(), (_x, _z, complete) => jobs.push(complete), 1);
        query.addSource(1, { x: 1, z: 1 }, 0);
        expect(query.addSource(2, { x: 3, z: 1 }, 0)).toBe(RuntimeStatus.OUT_OF_CAPACITY);
        query.removeSource(1);
        query.addSource(1, { x: 1, z: 1 }, 0);
        jobs[0](tile(0, 0));
        query.update();
        expect(query.getSourceStatus(1)).toBe(RuntimeStatus.NOT_READY);
        jobs[1](tile(0, 0));
        query.update();
        expect(query.getSourceStatus(1)).toBe(RuntimeStatus.HIT);
    });
});

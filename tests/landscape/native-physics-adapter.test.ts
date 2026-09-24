import { readFileSync } from 'fs';
import { resolve } from 'path';
import { runInNewContext } from 'vm';
import { createNativeLandscapeShapeType, LandscapePhysicsHeightfield } from '../../cocos/landscape/landscape-physics-collider';

// Execute the ORIGINAL JSB wrappers, replacing only the native SDK boundary.
// This checks the otherwise untested native event/raycast registry and the
// teardown ordering required when the legacy native TerrainShape is unchanged.
function setup (cookSucceeds = true) {
    const source = readFileSync(resolve(__dirname, '../../platforms/native/engine/jsb-physics.js'), 'utf8');
    const shape = source.slice(source.indexOf('class Shape {'), source.indexOf('class SphereShape extends Shape'));
    const terrain = source.slice(source.indexOf('class TerrainShape extends Shape'), source.indexOf('class Joint {'));
    const nodes = source.slice(source.indexOf('function bookNode'), source.indexOf('function updateCollisionMatrix'));
    const ptrToObj: Record<number, unknown> = {};
    const books: unknown[] = [];
    const order: string[] = [];
    const calls: unknown[][] = [];
    const legacyCook = jest.fn(() => { throw new Error('Landscape must not enter the legacy shared cache'); });
    class NativeShape {
        initialized = false;
        getObjectID () { return 42; }
        initialize () { this.initialized = true; order.push('initialize'); }
        setTerrain (...args: unknown[]) { calls.push(args); }
        onEnable () { order.push('enable'); }
        onDisable () { order.push('disable'); }
        onDestroy () {
            expect(this.initialized).toBe(true);
            expect(ptrToObj[42]).toBeNull();
            expect(books).toHaveLength(0);
            order.push('remove-shape');
        }
    }
    const Base = runInNewContext(`${nodes}\n${shape}\n${terrain}\nTerrainShape`, {
        ptrToObj, books, updateCollisionMatrix () {}, getHeightField: legacyCook,
        jsbPhy: { TerrainShape: NativeShape, CONFIG: { heightScale: 1 / 512 } },
    });
    class Resource {
        create (samples: Uint16Array, resolution: number, id: number) {
            expect(samples).toHaveLength(9); expect(resolution).toBe(3); expect(id).toBe(42);
            order.push('cook'); return cookSucceeds ? 7 : 0;
        }
        adoptShape () { expect(ptrToObj[42]).toBeTruthy(); order.push('adopt'); return true; }
        destroy () { order.push('release'); }
    }
    const Type = createNativeLandscapeShapeType(Base, Resource);
    const wrapper = new Type();
    const heightfield = new LandscapePhysicsHeightfield(new Uint16Array(9).fill(32768), {
        resolution: 3, tileSize: 2, heightScale: 3000, heightBias: 0, tilesX: 1, tilesZ: 1, level: 0, files: {},
    });
    const collider = { node: { updateWorldTransform () {} }, terrain: heightfield } as any;
    return { wrapper, collider, ptrToObj, books, order, calls, legacyCook };
}

test('native Landscape preserves JSB hit/event identity and releases resources after unregistration', () => {
    const s = setup();
    s.wrapper.initialize(s.collider);
    expect(s.ptrToObj[42]).toBe(s.wrapper);
    expect(s.wrapper.collider).toBe(s.collider);
    expect(s.books).toEqual([s.collider.node]);
    expect(s.calls).toEqual([[7, 1, 1, 3000 / 65535]]);
    s.wrapper.onEnable!(); s.wrapper.onDisable!(); s.wrapper.onDestroy!();
    expect(s.order).toEqual(['cook', 'initialize', 'adopt', 'enable', 'disable', 'remove-shape', 'release']);
    expect(s.legacyCook).not.toHaveBeenCalled();
});

test('failed native cooking cleans its resource without destroying an uninitialized legacy shape', () => {
    const s = setup(false);
    expect(() => s.wrapper.initialize(s.collider)).toThrow('Failed to create Landscape native heightfield');
    s.wrapper.onDestroy!();
    expect(s.order).toEqual(['cook', 'release']);
    expect(s.books).toHaveLength(0);
    expect(s.ptrToObj[42]).toBeUndefined();
    expect(s.legacyCook).not.toHaveBeenCalled();
});

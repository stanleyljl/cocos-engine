import { director, game, Game, DirectorEvent } from '../../cocos/game';
import { geometry, Vec3 } from '../../cocos/core';
import { Node, Scene } from '../../cocos/scene-graph';
import { physics, PhysicsMaterial, PhysicsSystem } from '../../exports/physics-framework';
import '../../exports/physics-physx';
import '../../exports/physics-ammo';
import '../../exports/physics-cannon';
import { initPhysXLibs, PX } from '../../cocos/physics/physx/physx-adapter';
import { waitForAmmoInstantiation } from '../../cocos/physics/bullet/instantiated';
import { builtinResMgr } from '../../exports/base';
import type { Landscape } from '../../cocos/landscape/landscape';
import { LandscapePhysics, LandscapePhysicsStatus as Status } from '../../cocos/landscape/landscape-physics';
import { LandscapeHeightLayout, LandscapeHeightLoader } from '../../cocos/landscape/landscape-height-data';
import { TerrainAsset, TERRAIN_HEIGHT_BASE, TERRAIN_HEIGHT_FACTORY } from '../../cocos/terrain/terrain-asset';

const layout: LandscapeHeightLayout = {
    resolution: 3, tilesX: 2, tilesZ: 1, tileSize: 2, heightScale: 65535, heightBias: -32768,
    level: 0, files: {},
};
// One nonplanar cell: h(A,B,C,D) = (0,2,4,10). The B-C diagonal gives 3 at its center, not 5.
function samples () { return new Uint16Array([32768, 32770, 32772, 32772, 32778, 32776, 32776, 32778, 32780]); }
class Loader extends LandscapeHeightLoader {
    public loads = 0;
    public layout = layout;
    public async loadLayout () { return this.layout; }
    public async loadTile () { ++this.loads; return samples(); }
}
async function settle (manager: LandscapePhysics) {
    for (let i = 0; i < 8; ++i) { await Promise.resolve(); manager.update(); }
    PhysicsSystem.instance.syncSceneToPhysics();
}
function hit (x: number, z: number) {
    const ray = new geometry.Ray(x, 100, z, 0, -1, 0);
    const system = PhysicsSystem.instance;
    if (!system.raycastClosest(ray, -1, 200)) return undefined;
    return system.raycastClosestResult.hitPoint.y;
}

game.emit(Game.EVENT_PRE_SUBSYSTEM_INIT);
PhysicsSystem.constructAndRegister();
PhysicsSystem.instance.setDefaultPhysicsMaterial(builtinResMgr.get<PhysicsMaterial>('default-physics-material'));
beforeAll(async () => { await waitForAmmoInstantiation(); await initPhysXLibs(); });

describe.each(['bullet', 'cannon.js', 'physx'])('Landscape physics: %s', backend => {
    let scene: Scene;
    let node: Node;
    let loader: Loader;
    let manager: LandscapePhysics;
    beforeEach(() => {
        physics.selector.switchTo(backend);
        scene = new Scene('landscape physics test'); director.runSceneImmediate(scene);
        node = new Node(); scene.addChild(node);
        loader = new Loader();
        manager = new LandscapePhysics({ node, landscapeAsset: { manifestPath: 'test.lsmanifest' } } as Landscape, loader);
        manager._setEnabled(true);
    });
    afterEach(() => { manager._destroy(); scene.active = false; scene._destroyImmediate(); });

    test('ordinary TerrainAsset keeps its diagonal and shared PhysX cache', async () => {
        const registeredShape = physics.selector.wrapper.TerrainShape;
        manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        await settle(manager);
        expect(physics.selector.wrapper.TerrainShape).toBe(registeredShape);
        expect(hit(-1.5, -0.5)).toBeCloseTo(3, 3);
        const asset = new TerrainAsset();
        asset.tileSize = 1; asset.blockCount = [1, 1];
        const n = asset.getVertexCountI();
        asset.heights = new Uint16Array(n * n).fill(TERRAIN_HEIGHT_BASE);
        asset.heights[1] += 2 / TERRAIN_HEIGHT_FACTORY;
        asset.heights[n] += 4 / TERRAIN_HEIGHT_FACTORY;
        asset.heights[n + 1] += 10 / TERRAIN_HEIGHT_FACTORY;
        const create = (x: number): Node => {
            const tile = new Node('Original Terrain'); tile.active = false;
            scene.addChild(tile); tile.setPosition(x, 0, 20);
            tile.addComponent(physics.TerrainCollider).terrain = asset;
            tile.active = true; return tile;
        };
        const first = create(20);
        const cached = backend === 'physx' ? PX.TERRAIN_STATIC[asset._uuid] : null;
        const second = create(60);
        PhysicsSystem.instance.syncSceneToPhysics();
        // Cannon's original Terrain convention is A-D; Landscape must adapt
        // without silently changing that existing behavior for other assets.
        const expected = backend === 'cannon.js' ? 5 : 3;
        expect(hit(20.5, 20.5)).toBeCloseTo(expected, 3);
        expect(hit(60.5, 20.5)).toBeCloseTo(expected, 3);
        if (backend === 'physx') expect(PX.TERRAIN_STATIC[asset._uuid]).toBe(cached);
        first.active = false; first._destroyImmediate();
        PhysicsSystem.instance.syncSceneToPhysics();
        expect(hit(20.5, 20.5)).toBeUndefined();
        expect(hit(60.5, 20.5)).toBeCloseTo(expected, 3);
        second.active = false; second._destroyImmediate();
        if (backend === 'physx') {
            expect(PX.TERRAIN_STATIC[asset._uuid]).toBe(cached);
            cached.release(); delete PX.TERRAIN_STATIC[asset._uuid];
        }
        asset.destroy();
    });

    test('streamed tiles never populate the ordinary PhysX cache and release their heightfields', async () => {
        if (backend !== 'physx') return;
        const keys = Object.keys(PX.TERRAIN_STATIC).sort();
        const id = manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        await settle(manager);
        let release: jest.SpyInstance | undefined;
        manager.forEachCollider(collider => {
            const field = (collider.shape as any)._ownedHeightField;
            expect(field).toBeTruthy();
            release = jest.spyOn(field, 'release');
        });
        expect(Object.keys(PX.TERRAIN_STATIC).sort()).toEqual(keys);
        manager.removeRegion(id); manager.update();
        expect(release).toHaveBeenCalledTimes(1);
        expect(Object.keys(PX.TERRAIN_STATIC).sort()).toEqual(keys);
        release?.mockRestore();
    });

    test('nonplanar collision matches B-C triangles and unloading removes it', async () => {
        const id = manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        expect(manager.getRegionStatus(id)).toBe(Status.NotReady);
        await settle(manager);
        expect(manager.getRegionError(id)).toBe('');
        expect(manager.isRegionReady(id)).toBe(true);
        expect(hit(-1.5, -0.5)).toBeCloseTo(3, 3);
        expect(hit(-1.8, -0.7)).toBeCloseTo(1.6, 3);
        expect(hit(-1.2, -0.3)).toBeCloseTo(6.4, 3);
        const colliders: physics.TerrainCollider[] = [];
        manager.forEachCollider(collider => colliders.push(collider));
        expect(colliders).toHaveLength(1);
        const collider = colliders[0];
        // Debug geometry must use the backend's centered heightfield and node
        // translation, rather than the camera's current rendering LOD.
        const cornerI = 0;
        const corner = new Vec3(cornerI * collider.terrain!.tileSize, collider.terrain!.getHeight(cornerI, 0), 0);
        Vec3.transformMat4(corner, corner, collider.node.worldMatrix);
        expect(corner.x).toBeCloseTo(-2, 3);
        expect(corner.z).toBeCloseTo(-1, 3);
        expect(corner.y).toBeCloseTo(0, 3);
        expect(collider.terrain!.getHeight(1, 1) + collider.node.worldPosition.y).toBeCloseTo(10, 3);
        manager.removeRegion(id); manager.update();
        PhysicsSystem.instance.syncSceneToPhysics();
        expect(hit(-1.5, -0.5)).toBeUndefined();
        expect(manager.getStats().sourceBytes).toBe(0);
        const visit = jest.fn(); manager.forEachCollider(visit); expect(visit).not.toHaveBeenCalled();
    });

    test('overlapping regions share a tile and capacity admits whole regions only', async () => {
        manager.configure({ maxTiles: 1 });
        const bounds = { minX: -1.8, minZ: -0.8, maxX: -1.2, maxZ: -0.2 };
        const a = manager.addRegion(bounds); const b = manager.addRegion(bounds);
        const c = manager.addRegion({ minX: 0.1, minZ: -0.8, maxX: 1, maxZ: 0.8 });
        await settle(manager);
        expect(loader.loads).toBe(1);
        expect(manager.isRegionReady(a) && manager.isRegionReady(b)).toBe(true);
        expect(manager.getRegionStatus(c)).toBe(Status.OutOfCapacity);
        const actual = { ...bounds };
        expect(manager.getRegionBounds(a, actual)).toBe(true);
        expect(actual).toEqual({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        manager.removeRegion(a); await settle(manager);
        expect(manager.isRegionReady(b)).toBe(true);
        manager.removeRegion(b); await settle(manager);
        expect(manager.isRegionReady(c)).toBe(true);
        expect(manager.getStats().residentTiles).toBe(1);
        expect(hit(-1.5, -0.5)).toBeUndefined();
    });

    test('resident movement area stays ready while an expanded region loads', async () => {
        const near = { minX: -1.8, minZ: -0.8, maxX: -0.2, maxZ: 0.8 };
        const far = { minX: 0.2, minZ: -0.8, maxX: 1.8, maxZ: 0.8 };
        const id = manager.addRegion(near); await settle(manager);
        expect(manager.isAreaReady(near)).toBe(true);
        let complete!: (data: Uint16Array) => void;
        jest.spyOn(loader, 'loadTile').mockImplementationOnce(() => new Promise(resolve => { complete = resolve; }));
        manager.setRegionBounds(id, { minX: -2, minZ: -1, maxX: 2, maxZ: 1 });
        expect(manager.isRegionReady(id)).toBe(false);
        expect(manager.isAreaReady(near)).toBe(true);
        await settle(manager);
        expect(manager.isRegionReady(id)).toBe(false);
        expect(manager.isAreaReady(near)).toBe(true);
        expect(manager.isAreaReady(far)).toBe(false);
        expect(manager.isAreaReady({ ...near, minX: -3 })).toBe(false);
        manager.removeRegion(id); manager.update();
        expect(manager.isAreaReady(near)).toBe(false);
        complete(samples()); await settle(manager);
        expect(manager.getStats().residentTiles).toBe(0);
    });

    test('disable releases resources, reenable restores regions; contact changes defer unloading', async () => {
        const id = manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        await settle(manager);
        director.emit(DirectorEvent.BEFORE_PHYSICS);
        manager._setEnabled(false);
        expect(manager.getStats().residentTiles).toBe(1);
        director.emit(DirectorEvent.AFTER_PHYSICS);
        expect(manager.getStats().residentTiles).toBe(0);
        expect(manager.getRegionStatus(id)).toBe(Status.Disabled);
        manager._setEnabled(true); await settle(manager);
        expect(manager.isRegionReady(id)).toBe(true);
        expect(hit(-1.5, -0.5)).toBeCloseTo(3, 3);
    });

    test('translated landscape covers world rectangles; unsupported transforms remove collision', async () => {
        node.setPosition(10, 7, 20);
        const id = manager.addRegion({ minX: 8, minZ: 19, maxX: 10, maxZ: 21 });
        await settle(manager);
        expect(manager.isRegionReady(id)).toBe(true);
        expect(hit(8.5, 19.5)).toBeCloseTo(10, 3);
        node.setScale(2, 1, 1); manager.update();
        expect(manager.getRegionStatus(id)).toBe(Status.Error);
        expect(manager.getStats().residentTiles).toBe(0);
    });

    test('3000m height range retains source quantization instead of overflowing signed16', async () => {
        loader.layout = { ...layout, heightScale: 3000, heightBias: 0 };
        const id = manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        await settle(manager);
        expect(manager.isRegionReady(id)).toBe(true);
        const ray = new geometry.Ray(-1.5, 4000, -0.5, 0, -1, 0);
        expect(PhysicsSystem.instance.raycastClosest(ray, -1, 5000)).toBe(true);
        expect(PhysicsSystem.instance.raycastClosestResult.hitPoint.y).toBeCloseTo(32771 * 3000 / 65535, 2);
    });

    test('a dynamic sphere lands on the static heightfield', async () => {
        jest.spyOn(loader, 'loadTile').mockResolvedValue(new Uint16Array(9).fill(32778));
        manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 }); await settle(manager);
        const bodyNode = new Node(); scene.addChild(bodyNode); bodyNode.setPosition(-1, 13, 0);
        bodyNode.addComponent(physics.RigidBody);
        const sphere = bodyNode.addComponent(physics.SphereCollider);
        sphere.radius = 0.25;
        let terrain!: physics.TerrainCollider;
        manager.forEachCollider(collider => { terrain = collider; });
        const contacts: physics.TerrainCollider[] = [];
        sphere.on('onCollisionEnter', event => { contacts.push(event.otherCollider as physics.TerrainCollider); });
        const system = PhysicsSystem.instance;
        for (let i = 0; i < 180; ++i) {
            system.syncSceneToPhysics(); system.step(1 / 60);
            system.physicsWorld.emitEvents(); system.physicsWorld.syncAfterEvents();
        }
        expect(bodyNode.worldPosition.y).toBeCloseTo(10.25, 1);
        expect(contacts).toContain(terrain);
        bodyNode.active = false; bodyNode._destroyImmediate();
    });

    test('Landscape ray hits preserve collider identity and collision groups', async () => {
        manager.configure({ group: 2, mask: 1 });
        manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 }); await settle(manager);
        let terrain!: physics.TerrainCollider;
        manager.forEachCollider(collider => { terrain = collider; });
        const ray = new geometry.Ray(-1.5, 100, -0.5, 0, -1, 0);
        const system = PhysicsSystem.instance;
        expect(system.raycastClosest(ray, 1, 200)).toBe(false);
        expect(system.raycastClosest(ray, 2, 200)).toBe(true);
        expect(system.raycastClosestResult.collider).toBe(terrain);
        expect(terrain.getGroup()).toBe(2);
        expect(terrain.getMask()).toBe(1);
    });

    test('stale async completion cannot restore a removed region and outstanding loads remain bounded', async () => {
        let complete!: (data: Uint16Array) => void;
        jest.spyOn(loader, 'loadTile').mockImplementationOnce(() => new Promise(resolve => { complete = resolve; }));
        manager.configure({ maxConcurrentLoads: 1 });
        const id = manager.addRegion({ minX: -2, minZ: -1, maxX: 0, maxZ: 1 });
        await settle(manager);
        expect(manager.getStats().inFlight).toBe(1);
        manager.setRegionBounds(id, { minX: 0, minZ: -1, maxX: 2, maxZ: 1 });
        await settle(manager);
        expect(manager.getStats().residentTiles).toBe(0);
        complete(samples()); await settle(manager);
        expect(manager.isRegionReady(id)).toBe(true);
        expect(hit(-1.5, -0.5)).toBeUndefined();
        expect(hit(0.5, -0.5)).toBeCloseTo(3, 3);
        expect(manager.getStats().inFlight).toBe(0);
    });

    test('load failure is explicit and removing then readding the region retries it', async () => {
        jest.spyOn(loader, 'loadTile').mockRejectedValueOnce(new Error('height unavailable'));
        const bounds = { minX: -2, minZ: -1, maxX: 0, maxZ: 1 };
        const id = manager.addRegion(bounds); await settle(manager);
        expect(manager.getRegionStatus(id)).toBe(Status.Error);
        expect(manager.getRegionError(id)).toContain('height unavailable');
        manager.removeRegion(id); manager.update();
        const replacement = manager.addRegion(bounds); await settle(manager);
        expect(manager.isRegionReady(replacement)).toBe(true);
    });
});

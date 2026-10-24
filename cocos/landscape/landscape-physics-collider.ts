// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { ccclass } from 'cc.decorator';
import { JSB } from 'internal:constants';
import { cclegacy, IVec3Like, Quat, Vec3 } from '../core';
import { TerrainCollider } from '../physics/framework/components/colliders/terrain-collider';
import { MeshCollider } from '../physics/framework/components/colliders/mesh-collider';
import { Mesh } from '../3d/assets/mesh';
import { selector } from '../physics/framework/physics-selector';
import { ITerrainAsset } from '../physics/spec/i-external';
import { ITerrainShape } from '../physics/spec/i-physics-shape';
// This is a data-only holder; importing it does not load/register the PhysX backend.
import { PhysXInstance } from '../physics/physx/physx-instance';
import type { LandscapeHeightLayout } from './landscape-height-data';

declare const jsb: any;
let nextTerrainID = 1;

/** Landscape-owned immutable samples, centered on the signed16 domain. */
export class LandscapeHeightfieldData implements ITerrainAsset {
    public readonly _uuid = `landscape-heightfield-${nextTerrainID++}`;
    // ITerrainAsset calls sample spacing tileSize; it is NOT the source tile's width.
    public readonly tileSize: number;
    public readonly localOriginY: number;
    public readonly heightFieldScale: number;
    constructor (public readonly samples: Uint16Array, private readonly _layout: LandscapeHeightLayout, public readonly holes: Uint8Array) {
        this.tileSize = _layout.tileSize / (_layout.resolution - 1);
        this.heightFieldScale = _layout.heightScale / 65535;
        this.localOriginY = _layout.heightBias + 32768 * _layout.heightScale / 65535;
    }
    public getVertexCountI (): number {
        return this._layout.resolution;
    }
    public getVertexCountJ (): number {
        return this._layout.resolution;
    }
    // Samples are Z-major with X contiguous; physics stores signed16 heights.
    public getSignedHeight (x: number, z: number): number {
        return this.samples[z * this._layout.resolution + x] - 32768;
    }
    public getHeight (x: number, z: number): number {
        return this.getSignedHeight(x, z) * this.heightFieldScale;
    }
}

// Shared with the native contract: zero is a valid native resource ID.
const INVALID_RESOURCE_ID = 0xFFFFFFFF;

/** Owns one streamed PhysX heightfield or mesh and its shape, matching C++.
 * Web handles are SDK objects; native handles are resource/wrapper IDs.
 * Failed creation returns INVALID_RESOURCE_ID; adoption returns success.
 * Destroy only after the wrapper has removed its physics/event registrations. */
class LandscapePhysicsResources {
    private _heightfield: any;
    private _triangleMesh: any;
    private _shape: any;
    private _wrapper: any;

    public createHeightfield (source: Uint16Array, holes: Uint8Array, resolution: number, wrapper: any): any {
        if (this._heightfield || this._triangleMesh) {
            return INVALID_RESOURCE_ID;
        }
        this._wrapper = wrapper;
        if (resolution < 2 || resolution > 4097 || source.length !== resolution * resolution || holes.length !== (resolution - 1) * (resolution - 1)) {
            return INVALID_RESOURCE_ID;
        }
        // The Web binding cannot set hole materials. The collision manager uses meshes
        // for these patches; reject unsupported input rather than filling holes.
        if (holes.some(value => value !== 0)) {
            return INVALID_RESOURCE_ID;
        }
        const PX = cclegacy._global.PhysX as any;
        const samples = new PX.PxHeightFieldSampleVector();
        const sample = new PX.PxHeightFieldSample();
        try {
            // PhysX rows run along X, columns along Z: transpose the source layout.
            for (let x = 0; x < resolution; ++x) {
                for (let z = 0; z < resolution; ++z) {
                    sample.height = source[z * resolution + x] - 32768;
                    samples.push_back(sample);
                }
            }
            this._heightfield = PhysXInstance.cooking.createHeightFieldExt(resolution, resolution, samples, PhysXInstance.physics);
        } finally {
            sample.delete();
            samples.delete();
        }
        return this._heightfield || INVALID_RESOURCE_ID;
    }

    public adoptTriangleMesh (mesh: any, wrapper: any): boolean {
        if (this._heightfield || this._triangleMesh) {
            return false;
        }
        this._wrapper = wrapper;
        if (!mesh || mesh === INVALID_RESOURCE_ID) {
            return false;
        }
        this._triangleMesh = mesh;
        return true;
    }

    public adoptShape (): boolean {
        if ((!this._heightfield && !this._triangleMesh) || this._shape || !this._wrapper?.impl) {
            return false;
        }
        // Transfer the original createShape reference without adding a reference.
        // The Landscape wrapper skips its ordinary release after this succeeds.
        this._shape = this._wrapper.impl;
        this._wrapper = null;
        return true;
    }

    public destroy (): void {
        this._shape?.release();
        this._shape = null;
        this._heightfield?.release();
        this._heightfield = null;
        this._triangleMesh?.release();
        this._triangleMesh = null;
        this._wrapper = null;
    }
}

/** Share ownership/teardown between Web heightfield and mesh adapters. */
function createWebLandscapeShapeType (Base: any): any {
    return class extends Base {
        protected readonly _resource = new LandscapePhysicsResources();
        private _shapeAdopted = false;

        initialize (collider: TerrainCollider | MeshCollider): void {
            super.initialize(collider);
            this._shapeAdopted = this._resource.adoptShape();
            if (!this._shapeAdopted) {
                throw new Error('Failed to own Landscape collision shape');
            }
        }

        onDestroy (): void {
            const shape = this._impl;
            const flags = this._flags;
            if (this._shapeAdopted) {
                // Preserve base shared-body cleanup, but defer the owned shape's
                // registry removal and release until that cleanup has finished.
                this._impl = null;
            }
            super.onDestroy();
            if (this._shapeAdopted && shape.$$) {
                // Same pointer-registry cleanup as PhysX removeReference. A static
                // adapter import would load PhysX even when another SDK is selected.
                const pointers = (cclegacy._global.PhysX as any).IMPL_PTR;
                pointers[shape.$$.ptr] = null;
                delete pointers[shape.$$.ptr];
            }
            this._resource.destroy();
            this._shapeAdopted = false;
            this.geometry?.delete();
            flags?.delete();
        }
    };
}

/** @engineInternal Reuse the unchanged JSB wrapper's event/raycast bookkeeping. */
export function createNativeLandscapeShapeType (Base: any, Resource: any): Constructor<ITerrainShape> {
    return class extends Base {
        private _resource: any;
        private _nativeInitialized = false;
        setTerrain (terrain: LandscapeHeightfieldData): void {
            if (this._resource) {
                return;
            }
            const resource = new Resource();
            this._resource = resource;
            const id = resource.createHeightfield(terrain.samples, terrain.holes, terrain.getVertexCountI(), this._impl.getObjectID());
            // PhysX resource ID 0 is valid; the Landscape owner returns UINT32_MAX on failure.
            if (id === INVALID_RESOURCE_ID) {
                throw new Error('Failed to create Landscape native heightfield');
            }
            this._impl.setTerrain(id, terrain.tileSize, terrain.tileSize, terrain.heightFieldScale);
        }
        initialize (collider: TerrainCollider): void {
            super.initialize(collider);
            this._nativeInitialized = true;
            // Retain the shape's original ownership reference so it can
            // be released after the unchanged JSB wrapper removes events.
            if (!this._resource.adoptShape()) {
                throw new Error('Failed to attach Landscape native heightfield');
            }
        }
        onDestroy (): void {
            // Failed cooking never initialized/booked the native shape.
            if (this._nativeInitialized) {
                super.onDestroy();
            }
            this._resource?.destroy();
            this._resource = null;
        }
    } as unknown as Constructor<ITerrainShape>;
}

// Subclass only the selected backend. Never replace its registered TerrainShape
// or load the other physics SDKs just because Landscape is enabled.
const shapeTypes = new WeakMap<Constructor<ITerrainShape>, Constructor<ITerrainShape>>();

// Ray-test just the top triangles, without building/caching convex pillars.
// Cannon's pillar ray path can miss exact patch edges after coordinate rotation.
// A tolerance in barycentric coordinates handles those edges without extending
// the geometry used by contact generation or changing ordinary terrain queries.
function createCannonHeightfieldType (CANNON: any): any {
    class Heightfield extends CANNON.Heightfield {}
    const type = CANNON.Shape.types.HEIGHTFIELD;
    const original = CANNON.Ray.prototype[type];
    CANNON.Ray.prototype[type] = function (shape: any, rotation: any, position: any, body: any, reportedShape: any): void {
        if (!(shape instanceof Heightfield)) {
            original.call(this, shape, rotation, position, body, reportedShape);
            return;
        }
        const from = new CANNON.Vec3(), to = new CANNON.Vec3();
        CANNON.Transform.pointToLocalFrame(position, rotation, this.from, from);
        CANNON.Transform.pointToLocalFrame(position, rotation, this.to, to);
        const dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        const spacing = shape.elementSize, data = shape.data;
        const tolerance = 1e-7;
        const minX = Math.max(0, Math.floor(Math.min(from.x, to.x) / spacing - tolerance));
        const minY = Math.max(0, Math.floor(Math.min(from.y, to.y) / spacing - tolerance));
        const maxX = Math.min(data.length - 2, Math.floor(Math.max(from.x, to.x) / spacing + tolerance));
        const maxY = Math.min(data[0].length - 2, Math.floor(Math.max(from.y, to.y) / spacing + tolerance));
        const normal = new CANNON.Vec3(), point = new CANNON.Vec3();
        for (let x = minX; x <= maxX; ++x) for (let y = minY; y <= maxY; ++y) {
            const a = data[x][y], b = data[x + 1][y], c = data[x][y + 1], d = data[x + 1][y + 1];
            for (let upper = 0; upper < 2; ++upper) {
                const sx = (upper ? d - c : b - a) / spacing;
                const sy = (upper ? d - b : c - a) / spacing;
                const denominator = dz - sx * dx - sy * dy;
                if (denominator === 0) continue;
                const t = ((upper ? d : a) + sx * (from.x - (x + upper) * spacing)
                    + sy * (from.y - (y + upper) * spacing) - from.z) / denominator;
                if (t < 0 || t > 1) continue;
                const u = (from.x + t * dx) / spacing - x, v = (from.y + t * dy) / spacing - y;
                if (u < -tolerance || v < -tolerance || u > 1 + tolerance || v > 1 + tolerance
                    || (upper ? u + v < 1 - tolerance : u + v > 1 + tolerance)) continue;
                normal.set(-sx, -sy, 1); normal.normalize(); rotation.vmult(normal, normal);
                point.set(this.from.x + t * (this.to.x - this.from.x), this.from.y + t * (this.to.y - this.from.y),
                    this.from.z + t * (this.to.z - this.from.z));
                this.reportIntersection(normal, point, reportedShape, body);
                if (this.result._shouldStop) return;
            }
        }
    };
    return Heightfield;
}

function createLandscapeShape (): ITerrainShape {
    const Base = selector.wrapper.TerrainShape as any;
    if (!Base) {
        throw new Error(`Landscape collision unavailable: ${selector.id}`);
    }
    let Type = shapeTypes.get(Base);
    if (!Type) {
        if (selector.id === 'bullet') {
            // Bullet already matches Landscape's triangles and owns its samples.
            Type = Base;
        } else if (selector.id === 'cannon.js') {
            const Heightfield = createCannonHeightfieldType(cclegacy._global.CANNON);
            Type = class extends Base {
                // Samples are immutable; TerrainShape.onLoad need not rebuild them.
                setTerrain (): void {}
                protected onComponentSet (): void {
                    const terrain = this.collider.terrain as LandscapeHeightfieldData;
                    const n = terrain.getVertexCountI();
                    for (let i = 0; i < n; ++i) {
                        this.data[i] = new Array<number>(n);
                        for (let j = 0; j < n; ++j) {
                            this.data[i][j] = terrain.getHeight(j, i);
                        }
                    }
                    this.options.elementSize = terrain.tileSize;
                    this._shape = new Heightfield(this.data, this.options);
                }
                protected _setCenter (v: IVec3Like): void {
                    // Local XYZ -> world ZXY preserves B-C without rotating the
                    // node (Cannon's AABB and ray paths compose rotations differently).
                    Quat.set(this._orient, -0.5, -0.5, -0.5, 0.5);
                    Vec3.copy(this._offset, v);
                }
            } as unknown as Constructor<ITerrainShape>;
        } else if (selector.id === 'physx' && JSB && typeof jsb !== 'undefined' && globalThis['jsb.physics']) {
            if (!jsb.LandscapePhysicsResources) {
                throw new Error('Landscape native physics binding is missing; rebuild native code');
            }
            Type = createNativeLandscapeShapeType(Base, jsb.LandscapePhysicsResources);
        } else if (selector.id === 'physx') {
            Type = class extends createWebLandscapeShapeType(Base) {
                setTerrain (terrain: LandscapeHeightfieldData): void {
                    if (this._impl) {
                        return;
                    }
                    const heightfield = this._resource.createHeightfield(terrain.samples, terrain.holes, terrain.getVertexCountI(), this);
                    if (heightfield === INVALID_RESOURCE_ID) {
                        throw new Error('Failed to create Landscape heightfield');
                    }
                    const PX = cclegacy._global.PhysX as any;
                    const flags = new PX.PxMeshGeometryFlags(0);
                    const geometry = new PX.PxHeightFieldGeometry(heightfield, flags, terrain.heightFieldScale, terrain.tileSize, terrain.tileSize);
                    try {
                        this._impl = PhysXInstance.physics.createShape(geometry, this.getSharedMaterial(this.collider.sharedMaterial), true, this._flags);
                    } finally {
                        geometry.delete();
                        flags.delete();
                    }
                    if (!this._impl) {
                        throw new Error('Failed to create Landscape collision shape');
                    }
                }
            } as unknown as Constructor<ITerrainShape>;
        } else {
            throw new Error(`Landscape collision unavailable: ${selector.id}`);
        }
        shapeTypes.set(Base, Type!);
    }
    return new Type!();
}

/** Internal tile component. Reuses Collider lifecycle/events without changing
 * the global shape factory or TerrainCollider's behavior for other terrain. */
@ccclass('cc.LandscapePhysicsCollider')
export class LandscapePhysicsCollider extends TerrainCollider {
    protected onLoad (): void {
        if (!selector.runInEditor) {
            return;
        }
        this.sharedMaterial = this._material;
        this._shape = createLandscapeShape();
        try {
            this._shape.initialize(this);
            this._shape.onLoad!();
        } catch (error) {
            const shape = this._shape;
            // Node activation can continue after onLoad throws. Do not enable a failed shape.
            this._shape = null;
            shape.onDestroy!();
            throw error;
        }
    }
}

/** Immutable streamed hole meshes. Backend changes stay local to Landscape. */
@ccclass('cc.LandscapePhysicsMeshCollider')
export class LandscapePhysicsMeshCollider extends MeshCollider {
    protected onLoad (): void {
        if (!selector.runInEditor) return;
        this.sharedMaterial = this._material;
        const Base = selector.wrapper.TrimeshShape as any;
        if (!Base) throw new Error(`Landscape hole collision unavailable: ${selector.id}`);
        let Type = Base;
        if (selector.id === 'cannon.js') {
            Type = class extends Base {
                setMesh (mesh: Mesh): void {
                    // initialize already builds normals, edges and the octree. The inherited
                    // onLoad calls setMesh again, but this tile's mesh never changes.
                    if (this._shape) return;
                    super.setMesh(mesh);
                    const tree = this._shape.tree;
                    // Cannon's query visits children even when their parent misses the
                    // query bounds. Prune those branches only for this Landscape mesh.
                    tree.aabbQuery = (bounds: any, result: number[]): number[] => {
                        const stack = [tree];
                        while (stack.length) {
                            const node = stack.pop()!;
                            if (!node.aabb.overlaps(bounds)) continue;
                            for (const triangle of node.data) result.push(triangle);
                            for (const child of node.children) stack.push(child);
                        }
                        return result;
                    };
                }
            };
        } else if (selector.id === 'physx' && JSB && typeof jsb !== 'undefined' && globalThis['jsb.physics']) {
            Type = class extends Base {
                private _resource: any;
                private _nativeInitialized = false;
                setMesh (mesh: Mesh): void {
                    if (this._resource) {
                        return;
                    }
                    super.setMesh(mesh);
                    this._resource = new jsb.LandscapePhysicsResources();
                    const cache = globalThis['jsb.physics'].CACHE.trimesh;
                    const adopted = this._resource.adoptTriangleMesh(cache[mesh._uuid], this._impl.getObjectID());
                    if (!adopted) {
                        throw new Error('Failed to own Landscape native hole mesh');
                    }
                    delete cache[mesh._uuid];
                }
                initialize (collider: MeshCollider): void {
                    super.initialize(collider);
                    this._nativeInitialized = true;
                    if (!this._resource.adoptShape()) {
                        throw new Error('Failed to attach Landscape native hole mesh');
                    }
                }
                onDestroy (): void {
                    if (this._nativeInitialized) {
                        super.onDestroy();
                    }
                    this._resource?.destroy();
                    this._resource = null;
                }
            };
        } else if (selector.id === 'physx') {
            Type = class extends createWebLandscapeShapeType(Base) {
                setMesh (mesh: Mesh): void {
                    if (this._impl) {
                        return;
                    }
                    super.setMesh(mesh);
                    const cache = (cclegacy._global.PhysX as any).MESH_STATIC;
                    const adopted = this._resource.adoptTriangleMesh(cache[mesh._uuid], this);
                    if (!adopted) {
                        throw new Error('Failed to own Landscape hole mesh');
                    }
                    delete cache[mesh._uuid];
                }
            };
        }
        this._shape = new Type();
        try {
            this._shape!.initialize(this);
            this._shape!.onLoad!();
        } catch (error) {
            const shape = this._shape!;
            this._shape = null;
            shape.onDestroy!();
            throw error;
        }
    }
}

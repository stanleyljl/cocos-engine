// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { ccclass } from 'cc.decorator';
import { JSB } from 'internal:constants';
import { cclegacy, IVec3Like, Quat, Vec3 } from '../core';
import { TerrainCollider } from '../physics/framework/components/colliders/terrain-collider';
import { selector } from '../physics/framework/physics-selector';
import { ITerrainAsset } from '../physics/spec/i-external';
import { ITerrainShape } from '../physics/spec/i-physics-shape';
// This is a data-only holder; importing it does not load/register the PhysX backend.
import { PhysXInstance } from '../physics/physx/physx-instance';
import type { LandscapeHeightLayout } from './landscape-height-data';

declare const jsb: any;
let nextTerrainID = 1;

/** Landscape-owned immutable samples, centered on the signed16 domain. */
export class LandscapePhysicsHeightfield implements ITerrainAsset {
    public readonly _uuid = `landscape-heightfield-${nextTerrainID++}`;
    public readonly tileSize: number;
    public readonly heightFieldScale: number;
    constructor (public readonly samples: Uint16Array, private readonly _layout: LandscapeHeightLayout) {
        this.tileSize = _layout.tileSize / (_layout.resolution - 1);
        this.heightFieldScale = _layout.heightScale / 65535;
    }
    public getVertexCountI (): number { return this._layout.resolution; }
    public getVertexCountJ (): number { return this._layout.resolution; }
    public getHeight (i: number, j: number): number {
        return (this.samples[j * this._layout.resolution + i] - 32768) * this.heightFieldScale;
    }
}

/** @engineInternal Reuse the unchanged JSB wrapper's event/raycast bookkeeping. */
export function createNativeLandscapeShapeType (Base: any, Resource: any): Constructor<ITerrainShape> {
    return class extends Base {
        private _resource: any;
        private _nativeInitialized = false;
        setTerrain (terrain: LandscapePhysicsHeightfield): void {
            if (this._resource) return;
            const resource = new Resource();
            this._resource = resource;
            const id = resource.create(terrain.samples, terrain.getVertexCountI(), this._impl.getObjectID());
            if (!id) throw new Error('Failed to create Landscape native heightfield');
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
            if (this._nativeInitialized) super.onDestroy();
            this._resource?.destroy();
            this._resource = null;
        }
    } as unknown as Constructor<ITerrainShape>;
}

// Subclass only the selected backend. Never replace its registered TerrainShape
// or load the other physics SDKs just because Landscape is enabled.
const shapeTypes = new WeakMap<Constructor<ITerrainShape>, Constructor<ITerrainShape>>();
function createLandscapeShape (): ITerrainShape {
    const Base = selector.wrapper.TerrainShape as any;
    if (!Base) throw new Error(`Landscape collision unavailable: ${selector.id}`);
    let Type = shapeTypes.get(Base);
    if (!Type) {
        if (selector.id === 'bullet') {
            // Bullet already matches Landscape's triangles and owns its samples.
            Type = Base;
        } else if (selector.id === 'cannon.js') {
            Type = class extends Base {
                // Samples are immutable; TerrainShape.onLoad need not rebuild them.
                setTerrain (): void {}
                protected onComponentSet (): void {
                    const terrain = this.collider.terrain as LandscapePhysicsHeightfield;
                    const n = terrain.getVertexCountI();
                    for (let i = 0; i < n; ++i) {
                        this.data[i] = new Array<number>(n);
                        for (let j = 0; j < n; ++j) this.data[i][j] = terrain.getHeight(j, i);
                    }
                    this.options.elementSize = terrain.tileSize;
                    this._shape = new cclegacy._global.CANNON.Heightfield(this.data, this.options);
                }
                protected _setCenter (v: IVec3Like): void {
                    // Local XYZ -> world ZXY preserves B-C without rotating the
                    // node (Cannon's AABB and ray paths compose rotations differently).
                    Quat.set(this._orient, -0.5, -0.5, -0.5, 0.5);
                    Vec3.copy(this._offset, v);
                }
            } as unknown as Constructor<ITerrainShape>;
        } else if (selector.id === 'physx' && JSB && typeof jsb !== 'undefined' && jsb.physics) {
            if (!jsb.LandscapeHeightfield) throw new Error('Landscape native physics binding is missing; rebuild native code');
            Type = createNativeLandscapeShapeType(Base, jsb.LandscapeHeightfield);
        } else if (selector.id === 'physx') {
            Type = class extends Base {
                private _ownedHeightField: any;
                setTerrain (terrain: LandscapePhysicsHeightfield): void {
                    if (this._impl) return;
                    const PX = cclegacy._global.PhysX as any;
                    const samples = new PX.PxHeightFieldSampleVector();
                    const sample = new PX.PxHeightFieldSample();
                    const n = terrain.getVertexCountI();
                    try {
                        for (let i = 0; i < n; ++i) for (let j = 0; j < n; ++j) {
                            sample.height = terrain.samples[j * n + i] - 32768;
                            samples.push_back(sample);
                        }
                        this._ownedHeightField = PhysXInstance.cooking.createHeightFieldExt(n, n, samples, PhysXInstance.physics);
                    } finally { sample.delete(); samples.delete(); }
                    if (!this._ownedHeightField) throw new Error('Failed to create Landscape heightfield');
                    const flags = new PX.PxMeshGeometryFlags(0);
                    const geometry = new PX.PxHeightFieldGeometry(this._ownedHeightField, flags,
                        terrain.heightFieldScale, terrain.tileSize, terrain.tileSize);
                    try {
                        this._impl = PhysXInstance.physics.createShape(geometry, this.getSharedMaterial(this.collider.sharedMaterial),
                            true, this._flags);
                    } finally { geometry.delete(); flags.delete(); }
                    if (!this._impl) throw new Error('Failed to create Landscape collision shape');
                }
                onDestroy (): void {
                    const flags = this._flags;
                    super.onDestroy();
                    flags?.delete();
                    this._ownedHeightField?.release();
                    this._ownedHeightField = null;
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
        if (!selector.runInEditor) return;
        this.sharedMaterial = this._material;
        this._shape = createLandscapeShape();
        this._shape.initialize(this);
        this._shape.onLoad!();
    }
}

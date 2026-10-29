// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { cclegacy } from '../core';
// This data-only holder does not load/register the PhysX backend.
import { PhysXInstance } from '../physics/physx/physx-instance';

// Shared with the native contract: zero is a valid native resource ID.
export const INVALID_RESOURCE_ID = 0xFFFFFFFF;

/** Owns one streamed PhysX heightfield or mesh and its shape, matching C++.
 * Web handles are SDK objects; native handles are resource/wrapper IDs.
 * Failed creation returns INVALID_RESOURCE_ID; adoption returns success.
 * Destroy only after the wrapper has removed its physics/event registrations. */
export class LandscapePhysicsResources {
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

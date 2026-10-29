/*
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 https://www.cocos.com/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

import { ccclass, help, menu, executeInEditMode, disallowMultiple, serializable, editable, type, displayOrder, rangeMin, range, slide, tooltip, group, visible, disallowAnimation, displayName } from 'cc.decorator';
import { EDITOR_NOT_IN_PREVIEW, JSB } from 'internal:constants';
import { Component } from '../scene-graph/component';
import { Asset, RenderTexture, Texture2D } from '../asset/assets';
import downloader from '../asset/asset-manager/downloader';
import { director, DirectorEvent } from '../game/director';
import { Enum, isValid, System, SystemPriority, Vec3 } from '../core';
import { Node } from '../scene-graph/node';
import { LandscapePhysics } from './landscape-physics';
import { LandscapeInstance, LandscapeInstanceInterface } from './LandscapeInstance';
import { API } from '../gfx';
import { warn } from '../core/platform/debug';

/** Terrain holes share streamed splat data in both modes. */
export enum LandscapeHoleMode {
    Fast = 0,
    HighQuality = 1,
}
Enum(LandscapeHoleMode);

/** Synchronous CPU query status. Only Hit makes the output valid. */
export enum LandscapeQueryStatus {
    Hit = 0,
    Miss = 1,
    NotReady = 2,
    Error = 3,
    OutOfCapacity = 4,
}

/** Reusable world-space result. Surface type is the dominant painted material ID.
 * Detail normals and decals are not part of this query.
 */
export class LandscapeSurfaceResult {
    public readonly position = new Vec3();
    public readonly normal = new Vec3(0, 1, 0);
    public surfaceType = -1;
    public surfaceWeight = 0;
    public get height (): number { return this.position.y; }
}

interface QuerySource {
    node: Node;
    radius: number;
    status: LandscapeQueryStatus;
}

/** One runtime instance of an already loaded decal layer. */
export interface LandscapeDecal {
    /** Minimum X corner in landscape-local coordinates. */
    x: number;
    /** Minimum Z corner in landscape-local coordinates. */
    z: number;
    /** Square side length, greater than zero and at most 64; must fit inside the terrain. */
    size: number;
    /** Index in the asset's preloaded DecalLayer array. */
    layer: number;
}

const ShadowCastingMode = Enum({ OFF: 0, ON: 1 });
const ShadowReceivingMode = Enum({ OFF: 0, ON: 1 });

// The native binding only exists on JSB platforms; guard native access with JSB.
declare const jsb: {
    LandscapeInstance?: new () => LandscapeInstanceInterface;
    fileUtils: { fullPathForFilename(path: string): string };
};

/**
 * @en Landscape terrain asset.
 * @zh 地形资源。
 */
@ccclass('cc.LandscapeAsset')
export class LandscapeAsset extends Asset {
    /** Resolved local library/package path, not a source-project directory. */
    get manifestPath (): string {
        return typeof this.nativeAsset === 'string' ? this.nativeAsset : this.nativeUrl;
    }
}

// Resolve the dependency without eagerly reading terrain tiles. Each runtime
// reads the manifest and streams raw pages from the same imported package.
downloader.register('.lsmanifest', (url, options, onComplete) => {
    // WebGL2 streams from the resolved library/package URL; JSB uses local files.
    if (JSB && /^[a-z][a-z0-9+.-]*:\/\//i.test(url)) {
        onComplete(new Error('Landscape assets require a local native package'));
        return;
    }
    const path = url.replace(/[?#].*$/, '');
    // Resolve the package once so physics can derive absolute raw-tile paths
    // without searching the filesystem on every asynchronous read request.
    const resolved = JSB ? jsb.fileUtils.fullPathForFilename(path) : url;
    if (!resolved) {
        onComplete(new Error(`Landscape manifest not found: ${path}`));
        return;
    }
    onComplete(null, resolved);
});

/** Debug settings shared by Inspector, keyboard/touch controls and terrain rendering. */
@ccclass('cc.LandscapeDebugData')
export class LandscapeDebugData {
    @serializable
    @editable
    @displayOrder(0)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.wireframe.tooltip')
    public wireframe = false;

    @serializable
    @editable
    @displayOrder(1)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.lodColor.tooltip')
    public lodColor = false;

    @serializable
    @editable
    @displayOrder(2)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.showRanges.tooltip')
    public showRanges = false;

    @serializable
    @editable
    @displayOrder(3)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.showBox.tooltip')
    public showBox = false;

    @serializable
    @editable
    @displayOrder(4)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.unlit.tooltip')
    public unlit = false;

    @serializable
    @editable
    @displayOrder(5)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.heightBlendEnabled.tooltip')
    public heightBlendEnabled = true;

    @serializable
    @editable
    @displayOrder(6)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.refineFarDetail.tooltip')
    public refineFarDetail = true;

    @serializable
    @editable
    @displayOrder(7)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.decal3DEnabled.tooltip')
    public decal3DEnabled = true;

    @serializable
    @editable
    @displayOrder(8)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.triplanarEnabled.tooltip')
    public triplanarEnabled = true;

    // These switches are consumed by the project's runtime debug views.
    @serializable
    @visible(false)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.physicsWireframe.tooltip')
    public physicsWireframe = false;

    @serializable
    @visible(false)
    @tooltip('i18n:classes.cc.LandscapeDebugData.properties.showVTAtlas.tooltip')
    public showVTAtlas = false;

}

/**
 * @en Terrain component backed by the native or WebGL2 Landscape runtime.
 * @zh 使用原生或 WebGL2 Landscape 运行时的地形组件。
 */
@ccclass('cc.Landscape')
@help('i18n:cc.Landscape')
@menu('Landscape/Landscape')
@executeInEditMode
@disallowMultiple
export class Landscape extends Component {
    @serializable
    private _holeMode = LandscapeHoleMode.Fast;

    @type(LandscapeHoleMode)
    @displayName('Hole Mode')
    @tooltip('i18n:classes.cc.Landscape.properties.holeMode.tooltip')
    get holeMode (): LandscapeHoleMode { return this._holeMode; }
    set holeMode (value: LandscapeHoleMode) {
        this._holeMode = value;
        this._runtime?.setHoleMode(this._holeMode);
    }

    public static ShadowCastingMode = ShadowCastingMode;
    public static ShadowReceivingMode = ShadowReceivingMode;

    @serializable
    private _shadowCastingMode = ShadowCastingMode.ON;

    @serializable
    private _shadowReceivingMode = ShadowReceivingMode.ON;

    /** Whether terrain and its displaced decals cast realtime shadows. */
    @type(ShadowCastingMode)
    @visible(false)
    get shadowCastingMode (): number {
        return this._shadowCastingMode;
    }
    set shadowCastingMode (value: number) {
        this._shadowCastingMode = value;
        if (this._runtime) {
            this._runtime.setCastShadow(value === ShadowCastingMode.ON);
        }
    }

    @group({ id: 'DynamicShadow', name: 'i18n:ENGINE.classes.cc.MeshRenderer.groups.DynamicShadow.displayName', style: 'section' })
    @displayName('i18n:ENGINE.classes.cc.MeshRenderer.properties.shadowCastingModeForInspector.displayName')
    @tooltip('i18n:classes.cc.Landscape.properties.shadowCastingModeForInspector.tooltip')
    @disallowAnimation
    get shadowCastingModeForInspector (): boolean {
        return this._shadowCastingMode === ShadowCastingMode.ON;
    }
    set shadowCastingModeForInspector (value: boolean) {
        this.shadowCastingMode = value ? ShadowCastingMode.ON : ShadowCastingMode.OFF;
    }

    /** Whether realtime shadows from terrain and other models darken the surface. */
    @type(ShadowReceivingMode)
    @visible(false)
    get receiveShadow (): number {
        return this._shadowReceivingMode;
    }
    set receiveShadow (value: number) {
        this._shadowReceivingMode = value;
        if (this._runtime) {
            this._runtime.setReceiveShadow(value === ShadowReceivingMode.ON);
        }
    }

    @group({ id: 'DynamicShadow', name: 'i18n:ENGINE.classes.cc.MeshRenderer.groups.DynamicShadow.displayName' })
    @displayName('i18n:ENGINE.classes.cc.MeshRenderer.properties.receiveShadowForInspector.displayName')
    @tooltip('i18n:classes.cc.Landscape.properties.receiveShadowForInspector.tooltip')
    @disallowAnimation
    get receiveShadowForInspector (): boolean {
        return this._shadowReceivingMode === ShadowReceivingMode.ON;
    }
    set receiveShadowForInspector (value: boolean) {
        this.receiveShadow = value ? ShadowReceivingMode.ON : ShadowReceivingMode.OFF;
    }

    /** Native binding on JSB; parallel TypeScript runtime on WebGL2. */
    private _runtime: LandscapeInstanceInterface | null = null;
    private _enableGeneration = 0;
    private _physics: LandscapePhysics | null = null;

    /** Explicit world-space collision regions, independent of rendering LOD. */
    public get physics (): LandscapePhysics {
        if (!this._physics) {
            this._physics = new LandscapePhysics(this);
            this._physics._setEnabled(this.enabledInHierarchy && !!this._runtime?.isInitialized());
        }
        return this._physics;
    }

    private readonly _querySources = new Map<number, QuerySource>();
    private readonly _queryOutput = new Float32Array(8);
    private _nextQuerySource = 1;

    @serializable
    private _queryCacheCapacity = 64;

    /** Independent CPU tile budget, set before enabling terrain. At 129x129,
     * 64 tiles hold about 7.1 MiB of height/RGB normals/splat data, excluding
     * worker decode memory. Tiles are released with their last source. No GPU allocation.
     */
    @editable
    @range([1, 4096, 1])
    @tooltip('i18n:classes.cc.Landscape.properties.queryCacheCapacity.tooltip')
    get queryCacheCapacity (): number { return this._queryCacheCapacity; }
    set queryCacheCapacity (value: number) {
        if (!Number.isInteger(value) || value < 1 || value > 4096) {
            throw new RangeError('Invalid query cache capacity');
        }
        if (this._runtime?.isInitialized()) {
            throw new Error('Set queryCacheCapacity before enabling Landscape');
        }
        this._queryCacheCapacity = value;
    }

    /** Register a node to preload/pin a square XZ region around its world position.
     * Follows the node automatically. Inactive nodes stop pinning; destroyed
     * nodes are removed automatically. Multiple sources share overlapping tiles.
     * Over-budget regions report OutOfCapacity instead of evicting another source.
     * Available on native and WebGL2; independent of camera LOD and VT readiness.
     */
    public addQuerySource (node: Node, preloadRadius = 64): number {
        if (!isValid(node, true)) {
            throw new Error('Query source requires a valid node');
        }
        this._validateQueryRadius(preloadRadius);
        const id = this._nextQuerySource++;
        this._querySources.set(id, { node, radius: preloadRadius, status: LandscapeQueryStatus.NotReady });
        return id;
    }

    public setQuerySourceRadius (id: number, preloadRadius: number): boolean {
        this._validateQueryRadius(preloadRadius);
        const source = this._querySources.get(id);
        if (!source) {
            return false;
        }
        source.radius = preloadRadius;
        source.status = LandscapeQueryStatus.NotReady;
        return true;
    }

    public removeQuerySource (id: number): void {
        this._querySources.delete(id);
        this._runtime?.removeQuerySource(id);
    }

    /** Hit means the entire requested region is ready; Error means invalid
     * input/placement or a failed source decode. Miss means no terrain overlap.
     * OutOfCapacity means the region cannot fit alongside other sources' pinned tiles.
     * Sources retry automatically as they move, shrink, or other sources release tiles.
     */
    public getQuerySourceStatus (id: number): LandscapeQueryStatus {
        const source = this._querySources.get(id);
        return source ? this._syncQuerySource(id, source) : LandscapeQueryStatus.Miss;
    }

    public isQuerySourceReady (id: number): boolean {
        return this.getQuerySourceStatus(id) === LandscapeQueryStatus.Hit;
    }

    /** Sample the highest source resolution at world XZ (Y is ignored).
     * Reads resident CPU memory only; never starts disk I/O or GPU work.
     * On non-Hit, output remains unchanged. Normals are decoded/interpolated
     * source RGB normals. Height uses the collision grid's B-C triangle split.
     * Like terrain selection, supports translation-only landscape placement.
     */
    public sampleSurface (worldPosition: Readonly<Vec3>, output: LandscapeSurfaceResult): LandscapeQueryStatus {
        if (!this._runtime || !this.enabledInHierarchy) {
            return LandscapeQueryStatus.NotReady;
        }
        const status = this._runtime.sampleSurface(worldPosition.x, worldPosition.z, this._queryOutput) as LandscapeQueryStatus;
        if (status === LandscapeQueryStatus.Hit) {
            const data = this._queryOutput;
            output.position.set(data[0], data[1], data[2]);
            output.normal.set(data[3], data[4], data[5]);
            output.surfaceType = data[6];
            output.surfaceWeight = data[7];
        }
        return status;
    }

    private _validateQueryRadius (radius: number): void {
        if (radius < 0) {
            throw new RangeError('Query radius must be nonnegative');
        }
    }

    private _syncQuerySource (id: number, source: QuerySource): LandscapeQueryStatus {
        if (!isValid(source.node, true)) {
            this.removeQuerySource(id);
            return LandscapeQueryStatus.Miss;
        }
        if (!this._runtime || !this.enabledInHierarchy || !source.node.activeInHierarchy) {
            this._runtime?.removeQuerySource(id);
            source.status = LandscapeQueryStatus.NotReady;
        } else {
            const position = source.node.worldPosition;
            source.status = this._runtime.setQuerySource(id, position.x, position.z, source.radius);
        }
        return source.status;
    }

    /** Initial capacity-admitted view resources have finished loading and composition; stays true during later streaming. */
    public get isReady (): boolean {
        return this._runtime ? this._runtime.isReady() : false;
    }

    /** Add a runtime decal after terrain initialization.
     * Returns a nonzero ID local to this Landscape, or 0 if unavailable, out of bounds,
     * the layer is not loaded, or capacity is full. Malformed parameters throw RangeError.
     * Authored and runtime decals share a limit of 512 instances. New decals compose
     * above existing ones. VT refresh uses the normal page budget.
     * Decals do not change collision or sampleSurface results.
     * Runtime instances are discarded on disable or asset replacement, not serialized.
     */
    public addDecal (decal: Readonly<LandscapeDecal>): number {
        if (typeof decal.x !== 'number' || typeof decal.z !== 'number' || typeof decal.size !== 'number'
            || decal.x < 0 || decal.z < 0 || decal.size <= 0 || decal.size > 64
            || !Number.isInteger(decal.layer) || decal.layer < 0 || decal.layer > 0xFFFFFFFF) {
            throw new RangeError('Invalid Landscape decal region or layer');
        }
        if (!this.enabledInHierarchy || !this._runtime) {
            return 0;
        }
        return this._runtime.addDecal(decal.x, decal.z, decal.size, decal.layer);
    }

    /** Remove an instance returned by addDecal. Returns false for stale/unknown IDs.
     * Authored asset instances are unaffected. VT pixels refresh over subsequent frames.
     */
    public removeDecal (id: number): boolean {
        if (!Number.isInteger(id) || id <= 0 || id > 0xFFFFFFFF || !this._runtime) {
            return false;
        }
        return this._runtime.removeDecal(id);
    }

    @serializable
    @type(LandscapeAsset)
    private _landscapeAsset: LandscapeAsset | null = null;

    @serializable
    @type(Texture2D)
    private _globalColorMap: Texture2D | null = null;

    @serializable
    private _lodQualityScale = 1.0;

    @serializable
    private _globalColorStrength = 0.1;

    @serializable
    @type(LandscapeDebugData)
    private _debugData = new LandscapeDebugData();

    // One bit per debug switch; -1 forces the initial runtime submission.
    private _appliedDebugFlags = -1;

    /** Settings are synchronized during tick; nested Inspector edits work too. */
    @editable
    @type(LandscapeDebugData)
    @displayOrder(1000)
    @tooltip('i18n:classes.cc.Landscape.properties.debugData.tooltip')
    get debugData (): LandscapeDebugData {
        return this._debugData;
    }
    set debugData (value: LandscapeDebugData) {
        this._debugData = value;
        this._syncDebugData();
    }

    /** Imported terrain resource; raw height/splat pages are loaded on demand. */
    @editable
    @type(LandscapeAsset)
    @displayOrder(0)
    @tooltip('i18n:classes.cc.Landscape.properties.landscapeAsset.tooltip')
    get landscapeAsset (): LandscapeAsset | null {
        return this._landscapeAsset;
    }
    set landscapeAsset (value: LandscapeAsset | null) {
        if (this._landscapeAsset === value) {
            return;
        }
        const enabled = this.enabledInHierarchy;
        if (enabled) {
            this.onDisable();
        }
        this._landscapeAsset = value;
        if (enabled) {
            this.onEnable();
        }
    }

    /**
     * @en LOD quality multiplier, applied only when loading terrain. Larger values retain finer geometry farther away.
     * @zh LOD 精度倍率，默认 1，最小值为 1。值越大，网格越精细；仅可在地形加载前设置。
     */
    @editable
    @displayOrder(9)
    @rangeMin(1.0)
    @tooltip('i18n:classes.cc.Landscape.properties.lodQualityScale.tooltip')
    get lodQualityScale (): number {
        return this._lodQualityScale;
    }
    set lodQualityScale (value: number) {
        if (this._runtime?.isInitialized()) {
            return;
        }
        if (value < 1.0) {
            return;
        }
        this._lodQualityScale = value;
    }

    /** Terrain-wide modulation data: RGB byte 128 is neutral; Alpha masks influence. Use a linear UNORM texture. */
    @editable
    @type(Texture2D)
    @displayOrder(1)
    @tooltip('i18n:classes.cc.Landscape.properties.globalColorMap.tooltip')
    get globalColorMap (): Texture2D | null {
        return this._globalColorMap;
    }
    set globalColorMap (value: Texture2D | null) {
        if (this._globalColorMap === value) {
            return;
        }
        this._globalColorMap = value;
        if (this._runtime) {
            this._runtime.setGlobalColorMap(value);
        }
    }

    /** Global modulation strength, multiplied by map Alpha and per-material influence. */
    @editable
    @displayOrder(10)
    @range([0, 1, 0.01])
    @slide
    @tooltip('i18n:classes.cc.Landscape.properties.globalColorStrength.tooltip')
    get globalColorStrength (): number {
        return this._globalColorStrength;
    }
    set globalColorStrength (value: number) {
        const strength = Math.min(1, Math.max(0, value));
        if (this._globalColorStrength === strength) {
            return;
        }
        this._globalColorStrength = strength;
        if (this._runtime) {
            this._runtime.setGlobalColorStrength(strength);
        }
    }

    private _syncDebugData (): void {
        if (!this._runtime) {
            return;
        }
        const data = this._debugData;
        const flags = (Number(data.wireframe) << 0)
            | (Number(data.lodColor) << 1)
            | (Number(data.showRanges) << 2)
            | (Number(data.showBox) << 3)
            | (Number(data.unlit) << 4)
            | (Number(data.showVTAtlas) << 5)
            | (Number(data.heightBlendEnabled) << 6)
            | (Number(data.refineFarDetail) << 7)
            | (Number(data.decal3DEnabled) << 8)
            | (Number(data.triplanarEnabled) << 9)
            | (Number(data.physicsWireframe) << 10);
        if (flags === this._appliedDebugFlags) {
            return;
        }
        this._runtime.setDebugData(data);
        this._appliedDebugFlags = flags;
    }

    public onLoad (): void {
        if (JSB && typeof jsb !== 'undefined' && jsb.LandscapeInstance) {
            this._runtime = new jsb.LandscapeInstance();
        } else if (!JSB) {
            if (director.root!.device.gfxAPI !== API.WEBGL2) {
                warn('[Landscape] Requires WebGL2; rendering, queries and physics were not started');
                return;
            }
            this._runtime = new LandscapeInstance();
        }
    }

    public onEnable (): void {
        const generation = ++this._enableGeneration;
        if (!this._runtime) {
            return;
        }
        this._runtime.setAssetPath(this._landscapeAsset?.manifestPath || '');
        this._runtime.setHoleMode(this._holeMode);
        this._appliedDebugFlags = -1;
        this._syncDebugData();
        this._runtime.setGlobalColorMap(this._globalColorMap);
        this._runtime.setGlobalColorStrength(this._globalColorStrength);
        this._runtime.setCastShadow(this._shadowCastingMode === ShadowCastingMode.ON);
        this._runtime.setReceiveShadow(this._shadowReceivingMode === ShadowReceivingMode.ON);
        const result = this._runtime.onEnable(this.node, this._lodQualityScale, this._queryCacheCapacity);
        if (typeof result === 'boolean') {
            if (result) {
                this._startUpdates();
            }
        } else {
            void result.then((initialized) => {
                if (initialized && generation === this._enableGeneration && this.enabledInHierarchy) {
                    this._startUpdates();
                    // Async initialization can finish after the Scene view stops ticking.
                    if (EDITOR_NOT_IN_PREVIEW) {
                        globalThis.cce.Engine.repaintInEditMode();
                    }
                }
            });
        }
    }

    private _startUpdates (): void {
        this._physics?._setEnabled(true);
        LandscapeSystem.getInstance().add(this);
        // Debug geometry is submitted for every draw, including while paused.
        director.on(DirectorEvent.BEFORE_DRAW, this._beforeDraw, this);
    }

    public onDisable (): void {
        ++this._enableGeneration;
        this._physics?._setEnabled(false);
        LandscapeSystem.remove(this);
        director.off(DirectorEvent.BEFORE_DRAW, this._beforeDraw, this);
        if (this._runtime) {
            this._runtime.onDisable();
        }
    }

    /** @engineInternal LandscapeSystem owns update scheduling after physics. */
    public override get internalUpdate (): undefined {
        return undefined;
    }

    /** Updates query streaming and the LOD view after physics and camera controllers. */
    public update (): void {
        if (!this._runtime) {
            return;
        }
        this._syncDebugData();
        for (const [id, source] of this._querySources) {
            this._syncQuerySource(id, source);
        }
        if (!this._runtime.isInitialized()) {
            return;
        }
        // Snapshot the main-camera LOD position. Pass collection traverses
        // each current frustum independently.
        this._runtime.update();
    }

    private _beforeDraw (): void {
        // Immediate-mode debug boxes must be re-submitted every frame.
        if (this._debugData.showBox) {
            this._runtime?.drawDebugBounds();
        }
    }

    public onDestroy (): void {
        ++this._enableGeneration;
        LandscapeSystem.remove(this);
        director.off(DirectorEvent.BEFORE_DRAW, this._beforeDraw, this);
        this._physics?._destroy();
        if (this._runtime) {
            this._runtime.onDisable();
        }
        this._runtime = null;
        this._querySources.clear();
    }

    /**
     * @en Returns the VT physical-atlas texture (debug), or null. Assign it
     * to a 2D Sprite to inspect the composed pages.
     * @zh 返回 VT 物理图集纹理（调试用），无则 null。可赋给 2D Sprite 查看合成结果。
     */
    public getVTAtlas (): Texture2D | RenderTexture | null {
        return this._runtime ? this._runtime.getVTAtlas() : null;
    }
}

/** Updates enabled landscapes after the built-in physics systems and their camera callbacks. */
class LandscapeSystem extends System {
    private static _instance: LandscapeSystem;

    public static getInstance (): LandscapeSystem {
        if (!LandscapeSystem._instance) {
            const system = new LandscapeSystem();
            LandscapeSystem._instance = system;
            // Larger priorities run first; both built-in physics systems use LOW (0).
            director.registerSystem('LANDSCAPE', system, SystemPriority.LOW - 1);
        }
        return LandscapeSystem._instance;
    }

    private readonly _landscapes = new Set<Landscape>();

    public add (landscape: Landscape): void {
        this._landscapes.add(landscape);
    }

    public static remove (landscape: Landscape): void {
        // An unsupported device or failed initialization never starts this system.
        LandscapeSystem._instance?._landscapes.delete(landscape);
    }

    public postUpdate (_dt: number): void {
        for (const landscape of this._landscapes) {
            landscape.update();
        }
    }
}

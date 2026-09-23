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
import { JSB } from 'internal:constants';
import { Component } from '../scene-graph/component';
import { Asset, Texture2D } from '../asset/assets';
import downloader from '../asset/asset-manager/downloader';
import { director, DirectorEvent } from '../game/director';
import { Enum, isValid, Vec3 } from '../core';
import { Node } from '../scene-graph/node';

/** Synchronous CPU query status. Only Hit makes the output valid. */
export enum LandscapeQueryStatus {
    Hit = 0,
    Miss = 1,
    NotReady = 2,
    Error = 3,
}

/** Reusable world-space result. Surface type is the dominant painted material ID.
 * Visual cliff overrides, detail normals and decals are not part of this query.
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

const ShadowCastingMode = Enum({ OFF: 0, ON: 1 });
const ShadowReceivingMode = Enum({ OFF: 0, ON: 1 });

// The native binding only exists on JSB platforms; guard native access with JSB.
declare const jsb: any;

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

// Resolve the native dependency without eagerly reading terrain tiles. Native
// FileUtils reads the manifest and streams raw pages from the same package.
downloader.register('.lsmanifest', (url, options, onComplete) => {
    // Editor/browser previews may resolve an HTTP library URL, but do not run
    // the native renderer. Native runtime supports packaged local assets only.
    if (JSB && /^[a-z][a-z0-9+.-]*:\/\//i.test(url)) {
        onComplete(new Error('Landscape assets require a local native package'));
        return;
    }
    onComplete(null, url.replace(/[?#].*$/, ''));
});

/** Debug settings shared by Inspector, keyboard/touch controls and native rendering. */
@ccclass('cc.LandscapeDebugData')
export class LandscapeDebugData {
    @serializable
    @editable
    @displayOrder(0)
    public wireframe = false;

    @serializable
    @editable
    @displayOrder(1)
    public lodColor = false;

    @serializable
    @editable
    @displayOrder(2)
    @tooltip('显示 LOD 距离范围和 Sector 边界。')
    public showRanges = false;

    @serializable
    @editable
    @displayOrder(3)
    public showBox = false;

    @serializable
    @editable
    @displayOrder(4)
    @tooltip('就绪后冻结主相机 LOD 位置和 VT 数据，各 Pass 仍按当前视锥剔除。')
    public freezeLod = false;

    @serializable
    @editable
    @displayOrder(5)
    public unlit = false;

    @serializable
    @editable
    @displayOrder(6)
    public showVTAtlas = false;

    @serializable
    @editable
    @displayOrder(7)
    @tooltip('悬崖三平面投影，结果缓存到 RVT。冻结时的切换在解冻后生效。')
    public cliffEnabled = true;

    @serializable
    @editable
    @displayOrder(8)
    @tooltip('开启高度混合；关闭使用线性权重混合。切换后重新生成 VT；冻结期间的修改在解冻后生效。')
    public heightBlendEnabled = true;

    @serializable
    @editable
    @displayOrder(9)
    @tooltip('将地形法线烘焙到 VT；关闭时使用随几何 LOD 变化的法线。冻结期间的切换在解冻后生效。')
    public bakeNormalEnabled = true;

    @serializable
    @editable
    @displayOrder(10)
    @tooltip('显示近景立体贴花网格；关闭后保留平面 RVT 贴花。冻结时仍可即时切换。')
    public decal3DEnabled = true;

}

const DEBUG_PROPERTIES = [
    'wireframe',
    'lodColor',
    'showRanges',
    'showBox',
    'freezeLod',
    'unlit',
    'showVTAtlas',
    'cliffEnabled',
    'heightBlendEnabled',
    'bakeNormalEnabled',
    'decal3DEnabled',
] as const;

/**
 * @en Terrain component backed by a native `cc::landscape::Landscape` object.
 * @zh 由原生 `cc::landscape::Landscape` 对象支撑的地形组件。
 */
@ccclass('cc.Landscape')
@help('i18n:cc.Landscape')
@menu('Landscape/Landscape')
@executeInEditMode
@disallowMultiple
export class Landscape extends Component {
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
        if (this._native) this._native.setCastShadow(value === ShadowCastingMode.ON);
    }

    @group({ id: 'DynamicShadow', name: 'i18n:ENGINE.classes.cc.MeshRenderer.groups.DynamicShadow.displayName', style: 'section' })
    @displayName('i18n:ENGINE.classes.cc.MeshRenderer.properties.shadowCastingModeForInspector.displayName')
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
        if (this._native) this._native.setReceiveShadow(value === ShadowReceivingMode.ON);
    }

    @group({ id: 'DynamicShadow', name: 'i18n:ENGINE.classes.cc.MeshRenderer.groups.DynamicShadow.displayName' })
    @displayName('i18n:ENGINE.classes.cc.MeshRenderer.properties.receiveShadowForInspector.displayName')
    @disallowAnimation
    get receiveShadowForInspector (): boolean {
        return this._shadowReceivingMode === ShadowReceivingMode.ON;
    }
    set receiveShadowForInspector (value: boolean) {
        this.receiveShadow = value ? ShadowReceivingMode.ON : ShadowReceivingMode.OFF;
    }

    /** native cc::landscape::Landscape 句柄（仅 JSB 环境有效） */
    private _native: any = null;

    private readonly _querySources = new Map<number, QuerySource>();
    private readonly _queryOutput = new Float32Array(8);
    private _nextQuerySource = 1;

    @serializable
    private _queryCacheCapacity = 64;

    /** Independent CPU tile budget, set before enabling terrain. At 129x129,
     * 64 tiles hold about 7.1 MiB of height/RGB normals/splat data, plus at most
     * two in-flight decodes and their temporary memory. No GPU allocation.
     */
    @editable
    @range([1, 4096, 1])
    get queryCacheCapacity (): number { return this._queryCacheCapacity; }
    set queryCacheCapacity (value: number) {
        if (!Number.isInteger(value) || value < 1 || value > 4096) throw new RangeError('Invalid query cache capacity');
        if (this._native?.isInitialized()) throw new Error('Set queryCacheCapacity before enabling Landscape');
        this._queryCacheCapacity = value;
    }

    /** Register a node to preload/pin a square XZ region around its world position.
     * Follows the node automatically. Inactive nodes stop pinning; destroyed
     * nodes are removed automatically. Multiple sources share overlapping tiles.
     * Over-budget regions remain NotReady instead of evicting another source.
     * Native platforms only; independent of camera LOD and VT readiness.
     */
    public addQuerySource (node: Node, preloadRadius = 64): number {
        if (!isValid(node, true)) throw new Error('Query source requires a valid node');
        this._validateQueryRadius(preloadRadius);
        const id = this._nextQuerySource++;
        this._querySources.set(id, { node, radius: preloadRadius, status: LandscapeQueryStatus.NotReady });
        return id;
    }

    public setQuerySourceRadius (id: number, preloadRadius: number): boolean {
        this._validateQueryRadius(preloadRadius);
        const source = this._querySources.get(id);
        if (!source) return false;
        source.radius = preloadRadius;
        source.status = LandscapeQueryStatus.NotReady;
        return true;
    }

    public removeQuerySource (id: number): void {
        this._querySources.delete(id);
        this._native?.removeQuerySource(id);
    }

    /** Hit means the entire requested region is ready; Error means invalid
     * input/placement or a failed source decode. Miss means no terrain overlap.
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
     * source RGB normals. Height uses bilinear interpolation of source samples.
     * Like terrain selection, supports translation-only landscape placement.
     */
    public sampleSurface (worldPosition: Readonly<Vec3>, output: LandscapeSurfaceResult): LandscapeQueryStatus {
        if (!Number.isFinite(worldPosition.x) || !Number.isFinite(worldPosition.z)) return LandscapeQueryStatus.Error;
        if (!this._native || !this.enabledInHierarchy) return LandscapeQueryStatus.NotReady;
        const status = this._native.sampleSurface(worldPosition.x, worldPosition.z, this._queryOutput) as LandscapeQueryStatus;
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
        if (!Number.isFinite(radius) || radius < 0) throw new RangeError('Query radius must be finite and nonnegative');
    }

    private _syncQuerySource (id: number, source: QuerySource): LandscapeQueryStatus {
        if (!isValid(source.node, true)) {
            this.removeQuerySource(id);
            return LandscapeQueryStatus.Miss;
        }
        if (!this._native || !this.enabledInHierarchy || !source.node.activeInHierarchy) {
            this._native?.removeQuerySource(id);
            source.status = LandscapeQueryStatus.NotReady;
        } else {
            const position = source.node.worldPosition;
            source.status = this._native.setQuerySource(id, position.x, position.z, source.radius);
        }
        return source.status;
    }

    /** Initial view has reached the configured geometry and VT precision and can render. */
    public get isReady (): boolean {
        return this._native ? this._native.isReady() : false;
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

    private _appliedDebugData: LandscapeDebugData | null = null;

    /** Settings are synchronized before drawing; nested Inspector edits work too. */
    @editable
    @type(LandscapeDebugData)
    @displayOrder(1000)
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
    get landscapeAsset (): LandscapeAsset | null {
        return this._landscapeAsset;
    }
    set landscapeAsset (value: LandscapeAsset | null) {
        if (this._landscapeAsset === value) return;
        const enabled = this.enabledInHierarchy;
        if (this._native && enabled) this.onDisable();
        this._landscapeAsset = value;
        if (this._native && enabled) this.onEnable();
    }

    /**
     * @en LOD quality multiplier, applied only when loading terrain. Larger values retain finer geometry farther away.
     * @zh LOD 精度倍率，默认 1，最小值为 1。值越大，网格越精细；仅可在地形加载前设置。
     */
    @editable
    @displayOrder(9)
    @rangeMin(1.0)
    get lodQualityScale (): number {
        return this._lodQualityScale;
    }
    set lodQualityScale (value: number) {
        if (this._native?.isInitialized()) return;
        if (!Number.isFinite(value) || value < 1.0) return;
        this._lodQualityScale = value;
    }

    /** Optional non-repeating color texture covering the whole landscape. */
    @editable
    @type(Texture2D)
    @displayOrder(1)
    @tooltip('覆盖整个地形的全局颜色图；未指定时使用原图层颜色。')
    get globalColorMap (): Texture2D | null {
        return this._globalColorMap;
    }
    set globalColorMap (value: Texture2D | null) {
        if (this._globalColorMap === value) return;
        this._globalColorMap = value;
        if (this._native) this._native.setGlobalColorMap(value);
    }

    /** Blend the global map's broad colors with the tiled materials. */
    @editable
    @displayOrder(10)
    @range([0, 1, 0.01])
    @slide
    @tooltip('全局颜色混合强度：0 使用原图层颜色，1 完全采用全局图的大范围颜色并保留材质细节。默认 0.1；未指定全局图时不生效。')
    get globalColorStrength (): number {
        return this._globalColorStrength;
    }
    set globalColorStrength (value: number) {
        if (!Number.isFinite(value)) return;
        const strength = Math.min(1, Math.max(0, value));
        if (this._globalColorStrength === strength) return;
        this._globalColorStrength = strength;
        if (this._native) {
            this._native.setGlobalColorStrength(strength);
        }
    }

    private _syncDebugData (force = false): void {
        if (!this._native) return;
        const previous = this._appliedDebugData;
        let changed = force || !previous;
        if (previous && !changed) {
            for (const key of DEBUG_PROPERTIES) {
                if (previous[key] !== this._debugData[key]) {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed) return;
        this._native.setDebugData(this._debugData);
        const snapshot = previous || new LandscapeDebugData();
        for (const key of DEBUG_PROPERTIES) snapshot[key] = this._debugData[key];
        this._appliedDebugData = snapshot;
    }

    public onLoad (): void {
        if (JSB && typeof jsb !== 'undefined' && jsb.Landscape) {
            this._native = new jsb.Landscape();
        }
    }

    public onEnable (): void {
        if (this._native) {
            this._native.setAssetPath(this._landscapeAsset?.manifestPath || '');
            this._native.setQueryCacheCapacity(this._queryCacheCapacity);
            this._syncDebugData(true);
            this._native.setGlobalColorMap(this._globalColorMap);
            this._native.setGlobalColorStrength(this._globalColorStrength);
            this._native.setCastShadow(this._shadowCastingMode === ShadowCastingMode.ON);
            this._native.setReceiveShadow(this._shadowReceivingMode === ShadowReceivingMode.ON);
            this._native.onEnable(this.node, this._lodQualityScale);
            // Wait for camera controllers in update/lateUpdate and systems to finish.
            director.on(DirectorEvent.BEFORE_DRAW, this._beforeDraw, this);
        }
    }

    public onDisable (): void {
        this._appliedDebugData = null;
        director.off(DirectorEvent.BEFORE_DRAW, this._beforeDraw, this);
        if (this._native) {
            this._native.onDisable();
        }
    }

    /**
     * @en Drives the native per-frame quadtree traversal (LOD selection + frustum culling).
     * @zh 每帧驱动原生四叉树遍历（LOD 选级 + 视锥剔除）。
     */
    private _beforeDraw (): void {
        if (!this._native) {
            return;
        }
        this._syncDebugData();
        for (const [id, source] of this._querySources) this._syncQuerySource(id, source);
        if (!this._native.isInitialized()) {
            return;
        }
        // Snapshot the main-camera LOD position. Native pass collection traverses
        // each current frustum independently, including while LOD/VT is frozen.
        this._native.update();
        // Immediate-mode debug boxes must be re-submitted every frame, even while
        // LOD is frozen.
        if (this._debugData.showBox) {
            this._native.drawDebugBounds();
        }
    }

    public onDestroy (): void {
        if (this._native) {
            this._native.onDisable();
        }
        this._native = null;
        this._querySources.clear();
    }

    /**
     * @en Returns the VT physical-atlas RenderTexture (debug), or null. Assign it
     * to a 2D Sprite to inspect the composed pages.
     * @zh 返回 VT 物理图集 RenderTexture（调试用），无则 null。可赋给 2D Sprite 查看合成结果。
     */
    public getVTAtlas (): any {
        return this._native ? this._native.getVTAtlas() : null;
    }
}

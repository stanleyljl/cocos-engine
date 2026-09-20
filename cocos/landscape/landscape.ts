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

import { ccclass, help, menu, executeInEditMode, disallowMultiple, serializable, editable, type, displayOrder, rangeMin, range, slide, tooltip } from 'cc.decorator';
import { JSB } from 'internal:constants';
import { Component } from '../scene-graph/component';
import { Asset } from '../asset/assets';
import downloader from '../asset/asset-manager/downloader';
import { director, DirectorEvent } from '../game/director';

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
    /** native cc::landscape::Landscape 句柄（仅 JSB 环境有效） */
    private _native: any = null;

    @serializable
    @type(LandscapeAsset)
    private _landscapeAsset: LandscapeAsset | null = null;

    @serializable
    private _lodQualityScale = 1.0;

    @serializable
    private _globalColorStrength = 0.1;

    @serializable
    private _wireframe = false;

    @serializable
    private _freezeLod = false;

    @serializable
    private _showBox = false;

    @serializable
    private _lodColor = false;

    @serializable
    private _showRanges = false;

    @serializable
    private _showSectors = false;

    @serializable
    private _showVTAtlas = false;

    @serializable
    private _vtMipEnabled = true;

    @serializable
    private _heightBlendEnabled = true;

    @serializable
    private _decal3DEnabled = true;

    @serializable
    private _unlit = false;

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
     * @en Whether the terrain is drawn as wireframe.
     * @zh 是否以线框模式绘制。
     */
    @editable
    @displayOrder(1)
    get wireframe (): boolean {
        return this._wireframe;
    }
    set wireframe (v: boolean) {
        this._wireframe = v;
        if (this._native) {
            this._native.setWireframe(v);
        }
    }

    /**
     * @en Freezes terrain geometry, morph and frustum selection, including render culling.
     * Move the camera freely to inspect a specific seam up close (debug).
     * @zh 冻结地形几何、morph 和视锥剔除结果；相机仍可自由移动（调试用）。
     */
    @editable
    @displayOrder(6)
    get freezeLod (): boolean {
        return this._freezeLod;
    }
    set freezeLod (v: boolean) {
        this._freezeLod = v;
        if (this._native) {
            this._native.setFreezeLod(v);
        }
    }

    /**
     * @en Draws a per-LOD-colored wireframe bounding box for every node rendered
     * this frame (debug). Works together with `freezeLod`.
     * @zh 为本帧渲染的每个节点绘制按 LOD 层级着色的线框包围盒（调试用）。可与 `freezeLod` 配合使用。
     */
    @editable
    @displayOrder(4)
    get showBox (): boolean {
        return this._showBox;
    }
    set showBox (v: boolean) {
        this._showBox = v;
    }

    /**
     * @en Tints the terrain surface by its LOD level so the level layout reads
     * at a glance (debug).
     * @zh 按 LOD 层级给地表着色，直观看分层（调试用）。
     */
    @editable
    @displayOrder(2)
    get lodColor (): boolean {
        return this._lodColor;
    }
    set lodColor (v: boolean) {
        this._lodColor = v;
        if (this._native) {
            this._native.setLodColor(v);
        }
    }

    /**
     * @en Draws each LOD level's CDLOD morph/range rings on the terrain surface,
     * using the same camera distance as selection (debug).
     * @zh 在地表上画出各级 CDLOD morph/range 距离环（与选层同一相机距离，调试用）。
     */
    @editable
    @displayOrder(3)
    get showRanges (): boolean {
        return this._showRanges;
    }
    set showRanges (v: boolean) {
        this._showRanges = v;
        if (this._native) {
            this._native.setShowRanges(v);
        }
    }

    /**
     * @en Draws a wireframe box per sector so the multi-sector world partition
     * reads at a glance (debug).
     * @zh 为每个 sector 绘制线框盒，直观看世界的多 sector 划分（调试用）。
     */
    @editable
    @displayOrder(5)
    get showSectors (): boolean {
        return this._showSectors;
    }
    set showSectors (v: boolean) {
        this._showSectors = v;
    }

    /** F7: show VT albedo without lighting, normal mapping or AO (debug). */
    @editable
    @displayOrder(7)
    get unlit (): boolean {
        return this._unlit;
    }
    set unlit (value: boolean) {
        this._unlit = value;
        if (this._native) {
            this._native.setUnlit(value);
        }
    }

    /** F8: atlas visibility, consumed by the application's debug view. */
    @editable
    @displayOrder(8)
    get showVTAtlas (): boolean {
        return this._showVTAtlas;
    }
    set showVTAtlas (value: boolean) {
        this._showVTAtlas = value;
    }

    /** F9: compare three-level VT trilinear filtering with mip0-only bilinear filtering. */
    @editable
    @displayOrder(8.5)
    @tooltip('VT mip 过滤：开启使用 mip0/mip1/mip2（256/128/64）三线性过滤；关闭仅使用 mip0 双线性过滤。冻结后仍可切换，不重新生成 VT。')
    get vtMipEnabled (): boolean {
        return this._vtMipEnabled;
    }
    set vtMipEnabled (value: boolean) {
        if (this._vtMipEnabled === value) return;
        this._vtMipEnabled = value;
        if (this._native) {
            this._native.setVTMipEnabled(value);
        }
    }

    /** F10: compare height blending with linear material weight blending. */
    @editable
    @displayOrder(8.6)
    @tooltip('开启高度混合；关闭使用线性权重混合。切换后重新生成 VT；F6 冻结期间的修改在解冻后生效。')
    get heightBlendEnabled (): boolean {
        return this._heightBlendEnabled;
    }
    set heightBlendEnabled (value: boolean) {
        if (this._heightBlendEnabled === value) return;
        this._heightBlendEnabled = value;
        if (this._native) {
            this._native.setHeightBlendEnabled(value);
        }
    }

    /** F12: compare the raised near mesh with the planar RVT decal. */
    @editable
    @displayOrder(8.8)
    @tooltip('显示近景立体贴花网格；关闭后保留平面 RVT 贴花。F6 冻结时仍可即时切换。')
    get decal3DEnabled (): boolean {
        return this._decal3DEnabled;
    }
    set decal3DEnabled (value: boolean) {
        if (this._decal3DEnabled === value) return;
        this._decal3DEnabled = value;
        if (this._native) this._native.setDecal3DEnabled(value);
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

    /** Blend the global map's broad colors with the tiled materials; overrides the manifest strength. */
    @editable
    @displayOrder(10)
    @range([0, 1, 0.01])
    @slide
    @tooltip('全局颜色混合强度：0 使用原图层颜色，1 完全采用全局图的大范围颜色并保留材质细节。默认 0.1，以原图层颜色为主；不改变 tiling、法线或粗糙度。覆盖地形清单中的 strength。')
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

    public onLoad (): void {
        if (JSB && typeof jsb !== 'undefined' && jsb.Landscape) {
            this._native = new jsb.Landscape();
        }
    }

    public onEnable (): void {
        if (this._native) {
            this._native.setAssetPath(this._landscapeAsset?.manifestPath || '');
            this._native.setFreezeLod(this._freezeLod);
            this._native.setVTMipEnabled(this._vtMipEnabled);
            this._native.setHeightBlendEnabled(this._heightBlendEnabled);
            this._native.setDecal3DEnabled(this._decal3DEnabled);
            this._native.setGlobalColorStrength(this._globalColorStrength);
            this._native.onEnable(this.node, this._lodQualityScale);
            this._native.setWireframe(this._wireframe);
            this._native.setLodColor(this._lodColor);
            this._native.setShowRanges(this._showRanges);
            this._native.setUnlit(this._unlit);
            // Wait for camera controllers in update/lateUpdate and systems to finish.
            director.on(DirectorEvent.BEFORE_DRAW, this._beforeDraw, this);
        }
    }

    public onDisable (): void {
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
        if (!this._native.isInitialized()) {
            return;
        }
        // Native captures the initial selection, then holds geometry and both
        // terrain/render frustum culling while frozen.
        this._native.update();
        // Immediate-mode debug boxes must be re-submitted every frame, even while
        // LOD is frozen (native redraws the last selected node set).
        if (this._showBox) {
            this._native.drawDebugBounds();
        }
        if (this._showSectors) {
            this._native.drawDebugSectors();
        }
    }

    public onDestroy (): void {
        if (this._native) {
            this._native.onDisable();
        }
        this._native = null;
    }

    /**
     * @en Returns the VT physical-atlas RenderTexture (debug), or null. Assign it
     * to a 2D Sprite to inspect the composed pages.
     * @zh 返回 VT 物理图集 RenderTexture（调试用），无则 null。可赋给 2D Sprite 查看合成结果。
     */
    public getDebugAtlas (): any {
        return this._native ? this._native.getDebugAtlas() : null;
    }
}

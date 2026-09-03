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

import { ccclass, help, menu, executeInEditMode, disallowMultiple, serializable, editable } from 'cc.decorator';
import { JSB } from 'internal:constants';
import { Component } from '../scene-graph/component';
import { Asset } from '../asset/assets';

// The native binding only exists on JSB platforms; guard native access with JSB.
declare const jsb: any;

/**
 * @en Landscape terrain asset.
 * @zh 地形资源。
 */
@ccclass('cc.LandscapeAsset')
export class LandscapeAsset extends Asset {
}

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
    private _dataDir = '';

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

    /**
     * @en Tile-pyramid base directory (project-relative), e.g.
     * `assets/landscape/heightmap-demo`. Native streams per-Node height tiles
     * from `<dir>/nodes/L<level>/h_<x>_<y>.png`. L0 is the finest level; larger
     * level numbers are progressively coarser.
     * @zh tile 金字塔根目录（工程相对路径），如 `assets/landscape/heightmap-demo`。
     * native 从 `<dir>/nodes/L<level>/h_<x>_<y>.png` 按 Node 流式加载高度 tile。
     * L0 为最细层级，层级数值越大越粗。
     */
    @editable
    get dataDir (): string {
        return this._dataDir;
    }
    set dataDir (v: string) {
        this._dataDir = v;
        if (this._native) {
            this._native.setDataDir(v);
        }
    }

    /**
     * @en Whether the terrain is drawn as wireframe.
     * @zh 是否以线框模式绘制。
     */
    @editable
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
     * @en Freezes the quadtree LOD selection so the current node set stays put.
     * Move the camera freely to inspect a specific seam up close (debug).
     * @zh 冻结四叉树 LOD 选级，固定当前节点集。可自由移动相机凑近检查某条接缝（调试用）。
     */
    @editable
    get freezeLod (): boolean {
        return this._freezeLod;
    }
    set freezeLod (v: boolean) {
        this._freezeLod = v;
    }

    /**
     * @en Draws a per-LOD-colored wireframe bounding box for every node rendered
     * this frame (debug). Works together with `freezeLod`.
     * @zh 为本帧渲染的每个节点绘制按 LOD 层级着色的线框包围盒（调试用）。可与 `freezeLod` 配合使用。
     */
    @editable
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
    get showSectors (): boolean {
        return this._showSectors;
    }
    set showSectors (v: boolean) {
        this._showSectors = v;
    }

    public onLoad (): void {
        if (JSB && typeof jsb !== 'undefined' && jsb.Landscape) {
            this._native = new jsb.Landscape();
        }
    }

    public onEnable (): void {
        if (this._native) {
            this._native.setDataDir(this._dataDir);
            this._native.onEnable(this.node);
            this._native.setWireframe(this._wireframe);
            this._native.setLodColor(this._lodColor);
            this._native.setShowRanges(this._showRanges);
        }
    }

    public onDisable (): void {
        if (this._native) {
            this._native.onDisable();
        }
    }

    /**
     * @en Drives the native per-frame quadtree traversal (LOD selection + frustum culling).
     * @zh 每帧驱动原生四叉树遍历（LOD 选级 + 视锥剔除）。
     */
    public update (): void {
        if (!this._native) {
            return;
        }
        if (!this._native.isInitialized() && this.node.scene) {
            this._native.setDataDir(this._dataDir);
            this._native.onEnable(this.node);
            this._native.setWireframe(this._wireframe);
            this._native.setLodColor(this._lodColor);
            this._native.setShowRanges(this._showRanges);
        }
        if (!this._native.isInitialized()) {
            return;
        }
        if (!this._freezeLod) {
            this._native.update();
        }
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

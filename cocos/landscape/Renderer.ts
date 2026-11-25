// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Material, Texture2D } from '../asset/assets';
import { RenderingSubMesh } from '../asset/assets/rendering-sub-mesh';
import { AABB, Frustum, intersect } from '../core/geometry';
import { Vec3, Vec4 } from '../core/math';
import { assert, error, log } from '../core/platform/debug';
import { director } from '../game/director';
import { CullMode, Format, FormatFeatureBit, Sampler, Texture } from '../gfx';
import { Model } from '../render-scene/scene/model';
import { Pass } from '../render-scene/core/pass';
import { RenderScene } from '../render-scene/core/render-scene';
import { Node } from '../scene-graph/node';
import { LandscapeAsset } from './Asset';
import { LandscapeData, LandscapeDebugData, LandscapeGridParams, LandscapeRegion, NodeAddress, NodeKey, QuadNode, VTPageLayout } from './Data';
import { bindLandscapeModelTextures, DecalRenderer, setLandscapeInstanceAttribute } from './DecalRenderer';
import { GridMesh } from './GridMesh';
import { MaterialLibrary } from './MaterialLibrary';
import { LandscapeResidency } from './Residency';
import { VTRenderer } from './VTRenderer';
import { MorphRange, RenderTree, RenderTreeChanges, RenderTreeNode } from './Quadtree';
import * as config from './Config';

interface GridDraw {
    model: Model;
    params: LandscapeGridParams; // Uploaded instance data, restored after material variants change.
    pageTableSlot: number; // Allocated with the draw.
}

function applyGridParams (model: Model, scratch: Float32Array, params: LandscapeGridParams): void {
    setLandscapeInstanceAttribute(model, scratch, 'a_gridInst', params.grid);
    setLandscapeInstanceAttribute(model, scratch, 'a_tileInst', params.tile);
    setLandscapeInstanceAttribute(model, scratch, 'a_mappingInst', params.mapping);
}

function createLandscapeMaterial (wireframe: boolean, heightUnorm: boolean): Material {
    const material = new Material();
    material.initialize({
        effectName: 'landscape/builtin-landscape',
        defines: { USE_INSTANCING: true, LANDSCAPE_HEIGHT_UNORM: heightUnorm, LANDSCAPE_WIREFRAME: wireframe },
        states: wireframe ? { rasterizerState: { cullMode: CullMode.NONE } } : undefined,
    });
    return material;
}

function setRuntimeTexture (material: Material, name: string, texture: Texture, sampler: Sampler): void {
    material.passes.forEach((pass, i) => {
        const handle = pass.getHandle(name);
        if (!handle) {
            return;
        }
        material.setProperty(name, texture, i);
        pass.bindSampler(Pass.getBindingFromHandle(handle), sampler);
    });
}

export class LandscapeRenderer {
    private _node!: Node;
    private _scene!: RenderScene;
    private _asset!: LandscapeAsset;
    private _data!: LandscapeData;

    private readonly _decals = new DecalRenderer();
    private readonly _residency = new LandscapeResidency();
    private readonly _materials = new MaterialLibrary();
    private _vt: VTRenderer | null = null;

    private _mesh: RenderingSubMesh | null = null;
    private _solid: Material | null = null;
    private _wire: Material | null = null;
    // Borrowed binding identity; updated by bindRuntimeTextures() and cleared on destroy().
    private _boundPageTableTexture: Texture | null = null;

    private readonly _draws = new Map<NodeKey, GridDraw>();
    private readonly _pool: Model[] = [];
    private readonly _scratch = new Float32Array(4);

    private readonly _viewPosition = new Vec3();
    private _debugData = new LandscapeDebugData();
    private _holeMode = 0;
    private _castShadow = true;
    private _receiveShadow = true;
    private _materialsDirty = false;

    private _atlasSize = 0;
    private _generation = 0;

    public async init (node: Node, scene: RenderScene, asset: LandscapeAsset): Promise<boolean> {
        const device = director.root!.device;
        const generation = this._generation;
        this._node = node;
        this._scene = scene;
        this._asset = asset;
        this._data = asset.data();
        const side = Math.min(config.VT_PAGES_PER_SIDE, Math.floor(device.capabilities.maxTextureSize / config.VT_PAGE_RES));
        const layers = Math.min(config.PAGE_POOL_LAYERS, device.capabilities.maxArrayTextureLayers);
        const roots = this._data.sectorsX * this._data.sectorsZ;
        if (side * side < roots || layers < roots) {
            error('[Landscape] Device capacity cannot hold sector roots');
            return false;
        }
        this._atlasSize = side * config.VT_PAGE_RES;
        this._mesh = GridMesh.create(device, 16);
        const filtered = FormatFeatureBit.SAMPLED_TEXTURE | FormatFeatureBit.LINEAR_FILTER;
        const heightUnorm = (device.getFormatFeatures(Format.R16_UNORM) & filtered) === filtered;
        this._solid = createLandscapeMaterial(false, heightUnorm);
        this._wire = createLandscapeMaterial(true, heightUnorm);
        if (!this._solid.passes.length || !this._wire.passes.length || !this._decals.init(node, scene, heightUnorm)
            || !this._residency.init(device, asset, layers, heightUnorm)) {
            return false;
        }
        if (!await this._materials.init(device, asset) || generation !== this._generation) {
            return false;
        }
        if (!this._decals.setAsset(asset, device, side * side)) {
            return false;
        }
        this._vt = new VTRenderer();
        if (!this._vt.init(asset, this._residency, this._materials, this._decals, this._atlasSize)) {
            return false;
        }
        this.bindRuntimeTextures();
        this.updateMaterialProperties();
        if (config.LANDSCAPE_DEBUG) {
            log(`[Landscape] Resources: sourceLayers=${layers} heightFormat=${heightUnorm ? 'R16_UNORM' : 'RG8'}`);
        }
        return true;
    }

    public destroy (): void {
        ++this._generation;
        this._materialsDirty = false;
        this.forEachModel((model) => {
            model.detachFromScene();
            director.root!.destroyModel(model);
        });
        this._draws.clear();
        this._pool.length = 0;
        this._boundPageTableTexture = null;
        this._vt?.destroy();
        this._vt = null;
        this._decals.destroy();
        this._residency.destroy();
        this._materials.destroy();
        this._mesh?.destroy();
        this._solid?.destroy();
        this._wire?.destroy();
        this._mesh = null;
        this._solid = null;
        this._wire = null;
    }

    // Render data and pass collection.
    public prepareRenderData (frame: number, tree: Readonly<RenderTree>, changes: Readonly<RenderTreeChanges>): void {
        if (this._boundPageTableTexture !== this._vt!.pageTable().texture()) {
            this.bindRuntimeTextures();
        }
        if (this._materialsDirty) {
            this._decals.setEnabled(this._debugData.decal3DEnabled);
            this._decals.setWireframe(this._debugData.wireframe);
        }
        if (changes.added.length || changes.updated.length || changes.removed.length) {
            this.syncTerrainModels(tree, changes);
        }
        if (this._materialsDirty) {
            this.refreshTerrainMaterials();
            this.updateMaterialProperties();
            this._materialsDirty = false;
        } else {
            this.updateMorphCameraProperty();
        }
        this._decals.updateView(this._viewPosition);
        // Selection contains only resource-backed nodes, including during startup.
        for (const { model } of this._draws.values()) {
            model.updateTransform(frame);
            model.updateUBOs(frame);
        }
        this._decals.preparePasses(frame);
    }

    public collectPassModels (tree: Readonly<RenderTree>, frustum: Readonly<Frustum>, shadow: boolean, models: Model[]): void {
        tree.traverse((node) => {
            const address = node.geometry.address();
            // Management nodes may cover several sectors above maxTileLevel.
            const size = this._data.sectorSize * 2 ** (address.level - this._data.maxTileLevel);
            const region = { x: address.x * size, z: address.z * size, size };
            return this.collectNodeModels(node.geometry, region, node.renderable, frustum, shadow, models);
        });
    }

    // Runtime settings.
    public setViewPosition (position: Readonly<Vec3>): void {
        this._viewPosition.set(position);
    }

    public setLodMorphRanges (ranges: readonly Readonly<MorphRange>[]): void {
        const morph = Array.from({ length: config.MAX_LOD_LEVELS }, () => new Vec4());
        for (let i = 0; i < Math.min(ranges.length, morph.length); ++i) {
            morph[i].set(ranges[i].start, ranges[i].end, 0, 0);
        }
        for (const material of this.surfaceMaterials()) {
            material.setProperty('lodMorph', morph);
        }
    }

    public setDebugData (data: LandscapeDebugData): void {
        this._debugData = { ...data };
        this._vt!.setHeightBlendEnabled(data.heightBlendEnabled);
        this._vt!.setTriplanarEnabled(data.triplanarEnabled);
        this._vt!.setRefineFarDetail(data.refineFarDetail);
        this._materialsDirty = true;
    }

    public setHoleMode (mode: number): void {
        this._holeMode = mode === 0 ? 0 : 1;
        this._materialsDirty = true;
    }

    public setCastShadow (enabled: boolean): void {
        this._castShadow = enabled;
        this.forEachModel((model) => {
            model.castShadow = enabled;
        });
        this._decals.setCastShadow(enabled);
    }

    public setReceiveShadow (enabled: boolean): void {
        if (this._receiveShadow === enabled) {
            return;
        }
        this._receiveShadow = enabled;
        this.forEachModel((model) => {
            model.receiveShadow = enabled;
        });
        this._decals.setReceiveShadow(enabled);
        this._materialsDirty = true;
    }

    public setGlobalColorMap (texture: Texture2D | null): void {
        this._vt!.setGlobalColorMap(texture);
    }

    public setGlobalColorStrength (strength: number): void {
        this._vt!.setGlobalColorStrength(strength);
    }

    public onGlobalPipelineStateChanged (): void {
        this.forEachModel((model) => model.onGlobalPipelineStateChanged());
        this._decals.onGlobalPipelineStateChanged();
    }

    // Runtime decals.
    public addDecal (x: number, z: number, size: number, layer: number): number {
        const id = this._decals.addDecal({ x, z, size, layer });
        if (id === 0) {
            return 0;
        }
        this._vt!.invalidate({ x, z, size });
        return id;
    }

    public removeDecal (id: number): boolean {
        const region = this._decals.removeDecal(id);
        if (!region) {
            return false;
        }
        this._vt!.invalidate(region);
        return true;
    }

    public residency (): LandscapeResidency {
        return this._residency;
    }

    public vtRenderer (): VTRenderer {
        return this._vt!;
    }

    public vtAtlas (): Texture2D {
        return this._vt!.atlas();
    }

    // Terrain models.
    private collectNodeModels (node: QuadNode, region: LandscapeRegion, renderable: boolean, frustum: Readonly<Frustum>, shadow: boolean, models: Model[]): boolean {
        const origin = this._node.worldPosition;
        const lift = this._debugData.decal3DEnabled ? this._asset.maxDecalHeight() : 0;
        const address = node.address();
        if (!renderable) {
            const top = node.maxY + lift;
            const bounds = new AABB(region.x + region.size * 0.5 + origin.x, (node.minY + top) * 0.5 + origin.y, region.z + region.size * 0.5 + origin.z, region.size * 0.5, (top - node.minY) * 0.5, region.size * 0.5);
            return !!intersect.aabbFrustum(bounds, frustum);
        }
        const draw = this._draws.get(address.key());
        assert(draw !== undefined, '[Landscape] Renderable node must have a draw record');
        const model = draw.model;
        assert(model != null, '[Landscape] Renderable node must have a model');
        const visible = intersect.aabbFrustum(model.worldBounds!, frustum);
        if (visible && model.enabled && (!shadow || model.castShadow)) {
            models.push(model);
        }
        if (this._debugData.decal3DEnabled) {
            let decalsVisible = !!visible;
            if (!decalsVisible) {
                const bounds = model.worldBounds!.clone();
                bounds.center.y += lift * 0.5;
                bounds.halfExtents.y += lift * 0.5;
                decalsVisible = !!intersect.aabbFrustum(bounds, frustum);
            }
            if (decalsVisible) {
                this._decals.collectPassModels(node, frustum, shadow, models);
            }
        }
        return false;
    }

    private syncTerrainModels (tree: Readonly<RenderTree>, changes: Readonly<RenderTreeChanges>): void {
        const table = this._vt!.pageTable();
        const changed: number[] = [];
        for (const key of changes.removed) {
            const draw = this._draws.get(key);
            if (draw) {
                this.recycleModel(draw);
                this._draws.delete(key);
            }
            this._decals.removeNode(key);
        }
        const updateDraw = (renderable: RenderTreeNode, draw: GridDraw): void => {
            const key = renderable.geometry.address().key();
            if (table.update(draw.pageTableSlot, renderable.material, this._vt!)) {
                changed.push(draw.pageTableSlot);
            }
            const node = renderable.geometry;
            const params = this.resolveGridParams(node, draw.pageTableSlot, renderable.edgeMask);
            draw.params = params;
            this.updateModel(draw.model, node, params);
            const region = this._data.nodeRegion(node.address());
            this._decals.updateNode({ nodeKey: key, region, minY: node.minY, maxY: node.maxY, gridParams: params });
        };
        for (const key of changes.added) {
            const renderable = tree.findRenderable(NodeAddress.fromKey(key))!;
            const draw = { model: this._pool.pop() || this.createModel(), pageTableSlot: table.allocate(), params: new LandscapeGridParams() };
            this._draws.set(key, draw);
            updateDraw(renderable, draw);
        }
        for (const key of changes.updated) {
            const renderable = tree.findRenderable(NodeAddress.fromKey(key))!;
            const draw = this._draws.get(key)!;
            updateDraw(renderable, draw);
        }
        table.upload(changed);
    }

    private resolveGridParams (node: QuadNode, pageTableSlot: number, edgeMask: number): LandscapeGridParams {
        const result = new LandscapeGridParams();
        const region = this._data.nodeRegion(node.address());
        result.grid.set(region.x, region.z, region.size, node.level);
        result.mapping.set(pageTableSlot, edgeMask, 0, 0);
        const tile = this._residency.resolve(node.address());
        if (tile.layer < 0) {
            throw new Error('Landscape unprepared exact height source');
        }
        result.tile = this._residency.shaderParams(tile);
        return result;
    }

    private updateModel (model: Model, node: QuadNode, gridParams: LandscapeGridParams): void {
        applyGridParams(model, this._scratch, gridParams);
        model.enabled = true;
        this.updateModelBounds(model, node);
    }

    private updateModelBounds (model: Model, node: QuadNode): void {
        const region = this._data.nodeRegion(node.address());
        model.createBoundingShape(new Vec3(region.x, node.minY, region.z), new Vec3(region.x + region.size, node.maxY, region.z + region.size));
        model.updateWorldBound();
    }

    private createModel (): Model {
        const model = director.root!.createModel(Model);
        model.node = this._node;
        model.transform = this._node;
        model.castShadow = this._castShadow;
        model.receiveShadow = this._receiveShadow;
        model.initSubModel(0, this._mesh!, this._debugData.wireframe ? this._wire! : this._solid!);
        bindLandscapeModelTextures(model);
        model.enabled = false;
        model.attachToScene(this._scene);
        return model;
    }

    private recycleModel (draw: GridDraw): void {
        draw.model.enabled = false;
        this._pool.push(draw.model);
        this._vt!.pageTable().release(draw.pageTableSlot);
    }

    private forEachModel (visit: (model: Model) => void): void {
        for (const { model } of this._draws.values()) {
            visit(model);
        }
        this._pool.forEach(visit);
    }

    // Materials and bindings.
    private surfaceMaterials (): Material[] {
        return [this._solid!, this._wire!, ...this._decals.materials()];
    }

    private bindRuntimeTextures (): void {
        const vt = this._vt!;
        const pageTableTexture = vt.pageTable().texture();
        for (const material of this.surfaceMaterials()) {
            setRuntimeTexture(material, 'holeSplatmap', this._residency.splatArray(), this._residency.splatSampler());
            setRuntimeTexture(material, 'heightmap', this._residency.heightArray(), this._residency.heightSampler());
            setRuntimeTexture(material, 'vtAlbedo', vt.albedo(), vt.sampler());
            setRuntimeTexture(material, 'vtNormalRoughness', vt.normalRoughness(), vt.sampler());
            setRuntimeTexture(material, 'vtPageMap', pageTableTexture, this._residency.splatSampler());
        }
        for (const material of this._decals.materials()) {
            setRuntimeTexture(material, 'decalHeightMap', this._materials.decalHeight(), this._residency.heightSampler());
        }
        this._boundPageTableTexture = pageTableTexture;
    }

    private updateMaterialProperties (): void {
        const terrain = new Vec4(this._data.heightScale, this._data.heightBias, this._debugData.lodColor ? 1 : 0, this._debugData.showRanges ? 1 : 0);
        const layout = new VTPageLayout(this._data.sectorSize);
        for (const material of this.surfaceMaterials()) {
            material.setProperty('holeParams', new Vec4(this._holeMode === 0 ? 0 : 1, 0, 0, 0));
            material.setProperty('terrainParams', terrain);
            material.setProperty('sectorParams', new Vec4(this._data.sectorSize, layout.pageSize(0), 0, 0));
            material.setProperty('vtLayout', new Vec4(this._atlasSize, config.VT_PAGE_RES, config.VT_PAGE_BORDER, config.VT_FILTER_MARGIN));
        }
        for (const material of this._decals.materials()) {
            material.setProperty('decalControl', new Vec4(this._debugData.decal3DEnabled ? 1 : 0, 0, 0, 0));
        }
        this.updateMorphCameraProperty();
    }

    private updateMorphCameraProperty (): void {
        const p = this._viewPosition;
        for (const material of this.surfaceMaterials()) {
            material.setProperty('morphCameraPos', new Vec4(p.x, p.y, p.z, 0));
        }
    }

    private refreshTerrainMaterials (): void {
        this.forEachModel((model) => {
            model.setSubModelMaterial(0, this._debugData.wireframe ? this._wire! : this._solid!);
            bindLandscapeModelTextures(model);
        });
        for (const draw of this._draws.values()) {
            applyGridParams(draw.model, this._scratch, draw.params);
        }
    }
}

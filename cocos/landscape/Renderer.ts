// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { Material, Texture2D } from '../asset/assets';
import { RenderingSubMesh } from '../asset/assets/rendering-sub-mesh';
import { AABB, Frustum, intersect } from '../core/geometry';
import { Vec3, Vec4 } from '../core/math';
import { error, log } from '../core/platform/debug';
import { director } from '../game/director';
import { CullMode, Sampler, Texture } from '../gfx';
import { Model } from '../render-scene/scene/model';
import { Pass } from '../render-scene/core/pass';
import { RenderScene } from '../render-scene/core/render-scene';
import { Node } from '../scene-graph/node';
import { LandscapeAsset } from './Asset';
import { LandscapeData, LandscapeDebugData, NodeAddress, NodeKey, QuadNode, VTPageAddress, VTPageLayout } from './Data';
import { bindLandscapeModelTextures, DecalRenderer, LandscapeGridParams } from './DecalRenderer';
import { GridMesh } from './GridMesh';
import { MaterialLibrary } from './MaterialLibrary';
import { Quadtree } from './Quadtree';
import { LandscapeResidency } from './Residency';
import { LandscapeSchedule } from './Schedule';
import { VTRenderer } from './VTRenderer';
import * as config from './Config';

interface GridDraw {
    model: Model;
    mapping: number;
}

function createLandscapeMaterial (wireframe: boolean): Material {
    const material = new Material();
    material.initialize({
        effectName: 'landscape/builtin-landscape',
        defines: { USE_INSTANCING: true, LANDSCAPE_DEBUG_UNLIT: false, LANDSCAPE_HEIGHT_UNORM: false, LANDSCAPE_WIREFRAME: wireframe },
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
    private _mesh: RenderingSubMesh | null = null;
    private _solid: Material | null = null;
    private _wire: Material | null = null;
    private readonly _decals = new DecalRenderer();
    private readonly _residency = new LandscapeResidency();
    private readonly _materials = new MaterialLibrary();
    private _vt: VTRenderer | null = null;
    private _schedule!: LandscapeSchedule;
    private readonly _draws = new Map<NodeKey, GridDraw>();
    private readonly _pool: Model[] = [];
    private readonly _scratch = new Float32Array(4);
    private readonly _viewPosition = new Vec3();
    private _debugData = new LandscapeDebugData();
    private _holeMode = 0;
    private _castShadow = true;
    private _receiveShadow = true;
    private _ready = false;
    private _atlasSize = 0;
    private _generation = 0;

    private readonly _quadtree: Quadtree;

    constructor (_quadtree: Quadtree) {
        this._quadtree = _quadtree;
    }

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
        this._solid = createLandscapeMaterial(false);
        this._wire = createLandscapeMaterial(true);
        if (!this._solid.passes.length || !this._wire.passes.length || !this._decals.init(node, scene)
            || !this._residency.init(device, asset, layers)) {
            return false;
        }
        if (!await this._materials.init(device, asset) || generation !== this._generation) {
            return false;
        }
        if (!this._decals.setAsset(asset, device, side * side)) {
            return false;
        }
        this._vt = new VTRenderer(device, this._atlasSize);
        if (!this._vt.init(asset, this._residency, this._materials, this._decals)) {
            return false;
        }
        // Root coverage and each root's worst-case composition footprint must
        // fit before publishing this renderer. Smaller devices retain the same rules.
        const rootKeys = new Set<NodeKey>();
        for (let z = 0; z < this._data.sectorsZ; ++z) {
            for (let x = 0; x < this._data.sectorsX; ++x) {
                rootKeys.add(new NodeAddress(this._data.maxLevel, x, z).key());
            }
        }
        const layout = new VTPageLayout(this._data.sectorSize);
        for (let z = 0; z < this._data.sectorsZ; ++z) {
            for (let x = 0; x < this._data.sectorsX; ++x) {
                const inputs: NodeAddress[] = [];
                this._vt.selectSources(new VTPageAddress(layout.rootLevel(), x, z), true).appendTo(inputs);
                if (rootKeys.size + inputs.filter((source) => !rootKeys.has(source.key())).length > layers) {
                    error('[Landscape] Device capacity cannot compose sector roots');
                    return false;
                }
            }
        }
        this._schedule = new LandscapeSchedule(asset, this._residency, this._vt, this._quadtree);
        this.bindRuntimeTextures();
        this.updateMaterialProperties();
        if (config.LANDSCAPE_DEBUG) {
            log(`[Landscape] Resources: sourceLayers=${layers} heightFormat=RG8`);
        }
        return true;
    }

    public isReady (): boolean {
        return this._ready;
    }
    public getVTAtlas (): Texture2D {
        return this._vt!.atlas();
    }

    private surfaceMaterials (): Material[] {
        return [this._solid!, this._wire!, ...this._decals.materials()];
    }

    private bindRuntimeTextures (): void {
        const vt = this._vt!;
        for (const material of this.surfaceMaterials()) {
            setRuntimeTexture(material, 'holeSplatmap', this._residency.splatArray(), this._residency.splatSampler());
            setRuntimeTexture(material, 'heightmap', this._residency.heightArray(), this._residency.heightSampler());
            setRuntimeTexture(material, 'vtAlbedo', vt.albedo(), vt.sampler());
            setRuntimeTexture(material, 'vtPageMap', vt.pageTable().texture(), this._residency.splatSampler());
            setRuntimeTexture(material, 'vtNormalRoughnessAO', vt.normalRoughnessAO(), vt.sampler());
        }
        for (const material of this._decals.materials()) {
            setRuntimeTexture(material, 'decalHeightMap', this._materials.decalHeight(), this._residency.heightSampler());
        }
    }

    public setGlobalColorMap (texture: Texture2D | null): void {
        this._vt!.setGlobalColorMap(texture);
    }
    public setGlobalColorStrength (strength: number): void {
        this._vt!.setGlobalColorStrength(strength);
    }

    public setDebugData (data: LandscapeDebugData): void {
        const previous = this._debugData;
        this._debugData = { ...data, unlit: previous.unlit };
        if (previous.wireframe !== data.wireframe) {
            this._decals.setWireframe(data.wireframe);
            this.refreshTerrainMaterials();
        }
        if (previous.decal3DEnabled !== data.decal3DEnabled) {
            this._decals.setEnabled(data.decal3DEnabled);
        }
        this._vt!.setHeightBlendEnabled(data.heightBlendEnabled);
        this._vt!.setTriplanarEnabled(data.triplanarEnabled);
        this._vt!.setRefineFarDetail(data.refineFarDetail);
        this.setUnlit(data.unlit);
        this.updateMaterialProperties();
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

    private updateMaterialProperties (): void {
        const terrain = new Vec4(this._data.heightScale, this._data.heightBias, this._debugData.lodColor ? 1 : 0, this._debugData.showRanges ? 1 : 0);
        const morph = Array.from({ length: config.MAX_LOD_LEVELS }, () => new Vec4());
        this._quadtree.lodMorphRanges().forEach((range, i) => morph[i].set(range.start, range.end, 0, 0));
        const layout = new VTPageLayout(this._data.sectorSize);
        for (const material of this.surfaceMaterials()) {
            material.setProperty('holeParams', new Vec4(this._holeMode === 0 ? 0 : 1, 0, 0, 0));
            material.setProperty('terrainParams', terrain);
            material.setProperty('sectorParams', new Vec4(this._data.sectorSize, layout.pageSize(0), 0, 0));
            material.setProperty('lodMorph', morph);
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

    private resolveGridParams (node: QuadNode, mapping: number): LandscapeGridParams {
        const result = new LandscapeGridParams();
        const region = this._data.nodeRegion(node.address());
        result.grid.set(region.x, region.z, region.size, node.level);
        result.mapping.set(mapping, this._quadtree.findRenderable(node.address())!.edgeMask, 0, 0);
        const tile = this._residency.resolve(node.address());
        if (tile.layer < 0) {
            throw new Error('Landscape unprepared exact height source');
        }
        result.tile = this._residency.shaderParams(tile);
        return result;
    }

    public preparePasses (): void {
        const frame = director.getTotalFrames();
        const previous = this._vt!.pageTable().texture();
        this._quadtree.updateTarget();
        this._schedule.prepareResources(frame);
        this._quadtree.selectRenderables();
        this._ready = this._ready || this._schedule.isSettled();
        if (EDITOR_NOT_IN_PREVIEW && this._schedule.needsUpdate()) {
            const generation = this._generation;
            // The editor ignores repaint requests during its current tick. Defer
            // until the frame ends, and stop once streaming settles or is cancelled.
            void Promise.resolve().then(() => {
                if (generation === this._generation && this._schedule.needsUpdate()) {
                    globalThis.cce.Engine.repaintInEditMode();
                }
            });
        }
        this.syncRenderables(frame, previous);
    }

    private syncRenderables (frame: number, previousMappings: Texture): void {
        if (previousMappings !== this._vt!.pageTable().texture()) {
            this.bindRuntimeTextures();
        }
        if (this._quadtree.changedNodes().size) {
            this.syncTerrainModels();
            this._quadtree.clearChangedNodes();
        }
        this._decals.updateView(this._viewPosition);
        if (!this._ready) {
            return;
        }
        for (const { model } of this._draws.values()) {
            model.updateTransform(frame);
            model.updateUBOs(frame);
        }
        this._decals.preparePasses(frame);
    }

    private updateModel (model: Model, node: QuadNode, gridParams: LandscapeGridParams): void {
        gridParams.apply(model, this._scratch);
        model.enabled = true;
        this.updateModelBounds(model, node);
    }

    private updateModelBounds (model: Model, node: QuadNode): void {
        const region = this._data.nodeRegion(node.address());
        model.createBoundingShape(new Vec3(region.x, node.minY, region.z), new Vec3(region.x + region.size, node.maxY, region.z + region.size));
        model.updateWorldBound();
    }

    private recycleModel (draw: GridDraw): void {
        draw.model.enabled = false;
        this._pool.push(draw.model);
        this._vt!.pageTable().release(draw.mapping);
    }

    private syncTerrainModels (): void {
        const table = this._vt!.pageTable();
        const changed: number[] = [];
        for (const key of this._quadtree.changedNodes()) {
            if (!this._quadtree.findRenderable(NodeAddress.fromKey(key))) {
                const draw = this._draws.get(key);
                if (draw) {
                    this.recycleModel(draw);
                    this._draws.delete(key);
                }
                this._decals.removeNode(key);
            }
        }
        for (const key of this._quadtree.changedNodes()) {
            const renderable = this._quadtree.findRenderable(NodeAddress.fromKey(key));
            if (!renderable) {
                continue;
            }
            let draw = this._draws.get(key);
            if (!draw) {
                draw = { model: this._pool.pop() || this.createModel(), mapping: table.allocate() };
                this._draws.set(key, draw);
            }
            if (table.update(draw.mapping, renderable.material, this._vt!)) {
                changed.push(draw.mapping);
            }
            const node = renderable.geometry;
            const params = this.resolveGridParams(node, draw.mapping);
            this.updateModel(draw.model, node, params);
            const region = this._data.nodeRegion(node.address());
            this._decals.updateNode({ nodeKey: key, region, minY: node.minY, maxY: node.maxY, gridParams: params });
        }
        table.upload(changed);
    }

    public collectPassModels (frustum: Readonly<Frustum>, shadow: boolean, models: Model[]): void {
        if (!this._ready) {
            return;
        }
        const origin = this._node.worldPosition;
        const lift = this._debugData.decal3DEnabled ? Math.max(0, ...this._asset.decalLayers().map((layer) => layer.heightScale)) : 0;
        this._quadtree.cull((address, renderable) => {
            if (!renderable) {
                const node = this._quadtree.find(address)!.geometry;
                const region = this._quadtree.region(address);
                const top = node.maxY + lift;
                const bounds = new AABB(region.x + region.size * 0.5 + origin.x, (node.minY + top) * 0.5 + origin.y, region.z + region.size * 0.5 + origin.z, region.size * 0.5, (top - node.minY) * 0.5, region.size * 0.5);
                return !!intersect.aabbFrustum(bounds, frustum);
            }
            const draw = this._draws.get(address.key());
            if (!draw) {
                return false;
            }
            const model = draw.model;
            const visible = intersect.aabbFrustum(model.worldBounds!, frustum);
            if (visible && model.enabled && (!shadow || model.castShadow)) {
                models.push(model);
            }
            if (this._debugData.decal3DEnabled) {
                if (!visible) {
                    const bounds = model.worldBounds!.clone();
                    bounds.center.y += lift * 0.5;
                    bounds.halfExtents.y += lift * 0.5;
                    if (!intersect.aabbFrustum(bounds, frustum)) {
                        return false;
                    }
                }
                this._decals.collectPassModels(this._quadtree.findRenderable(address)!.geometry, frustum, shadow, models);
            }
            return true;
        });
    }

    private forEachModel (visit: (model: Model) => void): void {
        for (const { model } of this._draws.values()) {
            visit(model);
        }
        this._pool.forEach(visit);
    }

    public onGlobalPipelineStateChanged (): void {
        this.forEachModel((model) => model.onGlobalPipelineStateChanged());
        this._decals.onGlobalPipelineStateChanged();
    }

    public addDecal (id: number, x: number, z: number, size: number, layer: number): boolean {
        if (!this._decals.addDecal(id, { x, z, size, layer })) {
            return false;
        }
        this._vt!.invalidate({ x, z, size });
        return true;
    }

    public removeDecal (id: number): boolean {
        const region = this._decals.removeDecal(id);
        if (!region) {
            return false;
        }
        this._vt!.invalidate(region);
        return true;
    }

    public setHoleMode (mode: number): void {
        this._holeMode = mode === 0 ? 0 : 1;
        this.updateMaterialProperties();
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
        this.refreshTerrainMaterials();
    }

    private refreshTerrainMaterials (): void {
        this.forEachModel((model) => {
            model.setSubModelMaterial(0, this._debugData.wireframe ? this._wire! : this._solid!);
            bindLandscapeModelTextures(model);
        });
        for (const [key, draw] of this._draws) {
            const node = this._quadtree.findRenderable(NodeAddress.fromKey(key))!;
            this.resolveGridParams(node.geometry, draw.mapping).apply(draw.model, this._scratch);
        }
    }

    public setViewPos (position: Readonly<Vec3>): void {
        if (Vec3.strictEquals(position, this._viewPosition)) {
            return;
        }
        this._viewPosition.set(position);
        this.updateMorphCameraProperty();
    }

    private setUnlit (enabled: boolean): void {
        if (this._debugData.unlit === enabled) {
            return;
        }
        const compile = (unlit: boolean): boolean => {
            let success = true;
            for (const material of this.surfaceMaterials()) {
                for (const pass of material.passes) {
                    pass.defines.LANDSCAPE_DEBUG_UNLIT = unlit;
                    success = pass.tryCompile() && success;
                }
            }
            return success;
        };
        if (compile(enabled)) {
            this._debugData.unlit = enabled;
        } else {
            compile(this._debugData.unlit);
            error('[Landscape] Failed to switch unlit shader variant');
        }
        this.refreshTerrainMaterials();
        this._decals.refreshMaterials();
    }

    public destroy (): void {
        ++this._generation;
        this._ready = false;
        this.forEachModel((model) => {
            model.detachFromScene();
            director.root!.destroyModel(model);
        });
        this._draws.clear();
        this._pool.length = 0;
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
}

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Material } from '../asset/assets';
import { RenderingSubMesh } from '../asset/assets/rendering-sub-mesh';
import { Frustum, intersect } from '../core/geometry';
import { Vec3, Vec4 } from '../core/math';
import { director } from '../game/director';
import { BufferTextureCopy, CullMode, Device, Format, Texture, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { Model } from '../render-scene/scene/model';
import { Pass } from '../render-scene/core/pass';
import { RenderScene } from '../render-scene/core/render-scene';
import { Node } from '../scene-graph/node';
import { InstancedBuffer } from '../rendering/instanced-buffer';
import { PipelineStateManager } from '../rendering/pipeline-state-manager';
import { Decal, LandscapeAsset } from './Asset';
import { LandscapeRegion, NodeKey, QuadNode } from './Data';
import { GridMesh } from './GridMesh';
import { MaterialLibrary } from './MaterialLibrary';
import * as config from './Config';

export function bindLandscapeModelTextures (model: Model): void {
    // Instancing requires these standard bindings, including after a material change replaces the descriptor set.
    model.updateLightingmap(null, new Vec4());
    model.updateReflectionProbeCubemap(null);
    model.updateReflectionProbePlanarMap(null);
    model.updateReflectionProbeBlendCubemap(null);
}

export function setLandscapeInstanceAttribute (model: Model, scratch: Float32Array, name: string, value: Readonly<Vec4>): void {
    Vec4.toArray(scratch, value);
    for (const subModel of model.subModels) {
        const { attributes, views } = subModel.instancedAttributeBlock;
        const index = attributes.findIndex((attribute) => attribute.name === name);
        if (index >= 0) {
            views[index].set(scratch);
        }
    }
}

export class LandscapeGridParams {
    public grid = new Vec4();
    public tile = new Vec4();
    public mapping = new Vec4();

    public apply (model: Model, scratch: Float32Array): void {
        setLandscapeInstanceAttribute(model, scratch, 'a_gridInst', this.grid);
        setLandscapeInstanceAttribute(model, scratch, 'a_tileInst', this.tile);
        setLandscapeInstanceAttribute(model, scratch, 'a_mappingInst', this.mapping);
    }
}

export interface TerrainNode {
    nodeKey: NodeKey;
    region: LandscapeRegion;
    minY: number;
    maxY: number;
    gridParams: LandscapeGridParams;
}

interface Draw {
    model: Model;
    decal: number;
    grid: Vec4;
}

interface Instance {
    decal: Decal;
    id: number;
    center: Vec3;
    fade: number;
    active: boolean;
    firstX: number;
    firstZ: number;
    columns: number;
    grids: Vec4[];
}

interface NodeDraws {
    terrain: TerrainNode;
    draws: Draw[];
}

export class DecalRenderer {
    private _node!: Node;
    private _scene!: RenderScene;
    private _asset!: LandscapeAsset;
    private _solid: Material | null = null;
    private _wire: Material | null = null;
    private _mesh: RenderingSubMesh | null = null;
    private _pageIndexTexture: Texture | null = null;
    private _pageIndexData = new Uint8Array();
    private readonly _instances: Instance[] = [];
    private readonly _nodes = new Map<NodeKey, NodeDraws>();
    private readonly _pool: Model[] = [];
    private readonly _scratch = new Float32Array(4);
    private readonly _viewPosition = new Vec3();
    private _enabled = true;
    private _wireframe = false;
    private _castShadow = true;
    private _receiveShadow = true;
    private _regionsDirty = true;

    public init (node: Node, scene: RenderScene): boolean {
        this._node = node;
        this._scene = scene;
        const createMaterial = (wireframe: boolean): Material => {
            const material = new Material();
            material.initialize({
                effectName: 'landscape/builtin-landscape-decal',
                defines: { USE_INSTANCING: true, LANDSCAPE_DEBUG_UNLIT: false, LANDSCAPE_HEIGHT_UNORM: false, LANDSCAPE_WIREFRAME: wireframe },
                states: wireframe ? { rasterizerState: { cullMode: CullMode.NONE } } : undefined,
            });
            return material;
        };
        this._solid = createMaterial(false);
        this._wire = createMaterial(true);
        this._mesh = GridMesh.create(director.root!.device, 16);
        if (!this._solid.passes.length || !this._wire.passes.length) {
            return false;
        }
        this._pool.push(this.createModel());
        this.compilePipelines();
        return true;
    }

    public materials (): Material[] {
        return [this._solid!, this._wire!];
    }

    public setAsset (asset: LandscapeAsset, device: Device, pageCount: number): boolean {
        this._asset = asset;
        for (const key of this._nodes.keys()) {
            this.removeNode(key);
        }
        this._instances.length = 0;
        for (const decal of asset.decals()) {
            this._instances.push(this.initializeInstance(decal, 0));
        }
        this._regionsDirty = true;
        this._pageIndexTexture?.destroy();
        this._pageIndexTexture = device.createTexture(new TextureInfo(TextureType.TEX2D, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RGBA8, config.DECAL_INSTANCE_MAX / 2, pageCount));
        this._pageIndexData = new Uint8Array(pageCount * config.DECAL_INSTANCE_MAX * 2);
        return !!this._pageIndexTexture;
    }

    private initializeInstance (decal: Decal, id: number): Instance {
        const d = { ...decal };
        const data = this._asset.data();
        const step = data.nodeSize(0) / 16;
        const x = d.x + d.size * 0.5;
        const z = d.z + d.size * 0.5;
        const address = data.nodeAtClamped(0, { x, z });
        const bounds = this._asset.getHeightRange(0, address.x, address.z)!;
        const instance: Instance = { decal: d, id, center: new Vec3(x, (bounds.minY + bounds.maxY) * 0.5, z), fade: 0, active: false, firstX: 0, firstZ: 0, columns: 0, grids: [] };
        if (this._asset.decalLayers()[d.layer].heightScale === 0) {
            return instance;
        }
        instance.firstX = Math.floor(Math.max(d.x, 0) / step);
        instance.firstZ = Math.floor(Math.max(d.z, 0) / step);
        const endX = Math.ceil(Math.min(d.x + d.size, data.width()) / step);
        const endZ = Math.ceil(Math.min(d.z + d.size, data.depth()) / step);
        instance.columns = Math.max(0, endX - instance.firstX);
        for (let iz = instance.firstZ; iz < endZ; ++iz) {
            for (let ix = instance.firstX; ix < endX; ++ix) {
                const left = Math.max(ix * step, d.x);
                const bottom = Math.max(iz * step, d.z);
                const right = Math.min((ix + 1) * step, d.x + d.size);
                const top = Math.min((iz + 1) * step, d.z + d.size);
                instance.grids.push(new Vec4(left, bottom, right - left, top - bottom));
            }
        }
        return instance;
    }

    public addDecal (id: number, d: Decal): boolean {
        const data = this._asset.data();
        if (!id || this._instances.length >= config.DECAL_INSTANCE_MAX || !Number.isInteger(d.layer) || d.layer < 0
            || d.layer >= this._asset.decalLayers().length || d.size <= 0 || d.size > 64 || d.x < 0 || d.z < 0
            || d.x + d.size > data.width() || d.z + d.size > data.depth()) {
            return false;
        }
        this._instances.push(this.initializeInstance(d, id));
        this._regionsDirty = true;
        this.updateView(this._viewPosition);
        return true;
    }

    public removeDecal (id: number): LandscapeRegion | null {
        const index = id ? this._instances.findIndex((entry) => entry.id === id) : -1;
        if (index < 0) {
            return null;
        }
        const d = this._instances[index].decal;
        for (const entry of this._nodes.values()) {
            entry.draws = entry.draws.filter((draw) => {
                if (draw.decal === index) {
                    this.recycle(draw);
                    return false;
                }
                if (draw.decal > index) {
                    --draw.decal;
                }
                return true;
            });
        }
        this._instances.splice(index, 1);
        this._regionsDirty = true;
        return { x: d.x, z: d.z, size: d.size };
    }

    public bindComposeMaterial (material: Material, materials: MaterialLibrary): boolean {
        const bindings: [string, Texture][] = [['decalAlbedoMap', materials.decalAlbedo()], ['decalNormalMap', materials.decalNormal()], ['decalIndexMap', this._pageIndexTexture!]];
        for (const [name, texture] of bindings) {
            material.setProperty(name, texture);
            for (const pass of material.passes) {
                if (!pass.getHandle(name)) {
                    return false;
                }
                pass.bindSampler(Pass.getBindingFromHandle(pass.getHandle(name)), materials.clampSampler());
            }
        }
        const layers = Array.from({ length: config.DECAL_LIBRARY_MAX }, () => new Vec4());
        this._asset.decalLayers().forEach((layer, i) => layers[i].set(layer.modulateColor ? 1 : 0, layer.detailMaterial0 + 1, layer.detailMaterial1 + 1, 0));
        material.setProperty('decalLayerParams', layers);
        this._regionsDirty = true;
        this.updateComposeMaterial(material);
        return true;
    }

    public updateComposeMaterial (material: Material): void {
        if (!this._regionsDirty) {
            return;
        }
        const regions = Array.from({ length: config.DECAL_INSTANCE_MAX }, () => new Vec4());
        this._instances.forEach((instance, i) => {
            const d = instance.decal;
            regions[i].set(d.x, d.z, d.size, d.layer);
        });
        material.setProperty('decalRegions', regions);
        material.setProperty('decalParams', new Vec4(this._instances.length, 0, 0, 0));
        this._regionsDirty = false;
    }

    public collectPageDecals (slot: number, region: Readonly<Vec4>): number {
        const border = region.z * config.VT_PAGE_BORDER / config.VT_PAGE_INTERIOR;
        let count = 0;
        this._instances.forEach((instance, i) => {
            const d = instance.decal;
            if (d.x > region.x + region.z + border || d.z > region.y + region.z + border
                || d.x + d.size < region.x - border || d.z + d.size < region.y - border) {
                return;
            }
            const offset = (slot * config.DECAL_INSTANCE_MAX + count++) * 2;
            this._pageIndexData[offset] = i & 255;
            this._pageIndexData[offset + 1] = i >> 8;
        });
        return count;
    }

    public uploadPageDecals (): void {
        const copy = new BufferTextureCopy();
        copy.texExtent.width = config.DECAL_INSTANCE_MAX / 2;
        copy.texExtent.height = this._pageIndexTexture!.height;
        director.root!.device.copyBuffersToTexture([this._pageIndexData], this._pageIndexTexture!, [copy]);
    }

    private updateBinding (draw: Draw, node: TerrainNode, boundsChanged: boolean): void {
        node.gridParams.apply(draw.model, this._scratch);
        if (boundsChanged) {
            const lift = this._asset.decalLayers()[this._instances[draw.decal].decal.layer].heightScale;
            draw.model.createBoundingShape(new Vec3(draw.grid.x, node.minY, draw.grid.y), new Vec3(draw.grid.x + draw.grid.z, node.maxY + lift, draw.grid.y + draw.grid.w));
            draw.model.updateWorldBound();
        }
    }

    private updateInstance (draw: Draw, node: TerrainNode): void {
        this.updateBinding(draw, node, true);
        const instance = this._instances[draw.decal];
        const d = instance.decal;
        const layer = this._asset.decalLayers()[d.layer];
        setLandscapeInstanceAttribute(draw.model, this._scratch, 'a_decalRegion', new Vec4(d.x, d.z, d.size, d.layer));
        setLandscapeInstanceAttribute(draw.model, this._scratch, 'a_decalGrid', draw.grid);
        setLandscapeInstanceAttribute(draw.model, this._scratch, 'a_decalFade', new Vec4(layer.heightScale, instance.fade, 0, 0));
    }

    private appendDecal (node: NodeDraws, decal: number): void {
        const instance = this._instances[decal];
        const d = instance.decal;
        const region = node.terrain.region;
        if (region.x + region.size <= d.x || region.z + region.size <= d.z || region.x >= d.x + d.size || region.z >= d.z + d.size) {
            return;
        }
        const step = this._asset.data().nodeSize(0) / 16;
        const firstX = Math.floor(Math.max(region.x, d.x) / step);
        const firstZ = Math.floor(Math.max(region.z, d.z) / step);
        const endX = Math.ceil(Math.min(region.x + region.size, d.x + d.size) / step);
        const endZ = Math.ceil(Math.min(region.z + region.size, d.z + d.size) / step);
        for (let iz = firstZ; iz < endZ; ++iz) {
            for (let ix = firstX; ix < endX; ++ix) {
                const model = this._pool.pop() || this.createModel();
                const grid = instance.grids[(iz - instance.firstZ) * instance.columns + ix - instance.firstX];
                const draw = { model, decal, grid };
                node.draws.push(draw);
                this.updateInstance(draw, node.terrain);
                model.enabled = this._enabled;
            }
        }
        node.draws.sort((a, b) => a.decal - b.decal);
    }

    private createModel (): Model {
        const model = director.root!.createModel(Model);
        model.node = this._node;
        model.transform = this._node;
        model.castShadow = this._castShadow;
        model.receiveShadow = this._receiveShadow;
        model.initSubModel(0, this._mesh!, this._wireframe ? this._wire! : this._solid!);
        bindLandscapeModelTextures(model);
        model.enabled = false;
        model.attachToScene(this._scene);
        return model;
    }

    private compilePipelines (): void {
        if (!this._pool.length) {
            return;
        }
        const subModel = this._pool[this._pool.length - 1].subModels[0];
        const root = director.root!;
        const renderPass = root.mainWindow!.framebuffer.renderPass;
        subModel.passes.forEach((pass, i) => {
            const instances = new InstancedBuffer(pass);
            instances.merge(subModel, i);
            for (const instance of instances.instances) {
                if (!instance.shader) {
                    instances.destroy();
                    throw new Error('Landscape decal shader compilation failed');
                }
                PipelineStateManager.getOrCreatePipelineState(root.device, pass, instance.shader, renderPass, instance.ia);
            }
            instances.destroy();
        });
    }

    private recycle (draw: Draw): void {
        draw.model.enabled = false;
        this._pool.push(draw.model);
    }

    public removeNode (key: NodeKey): void {
        const entry = this._nodes.get(key);
        if (!entry) {
            return;
        }
        entry.draws.forEach((draw) => this.recycle(draw));
        this._nodes.delete(key);
    }

    public updateNode (node: TerrainNode): void {
        const entry = this._nodes.get(node.nodeKey);
        if (!entry) {
            const next = { terrain: node, draws: [] };
            this._nodes.set(node.nodeKey, next);
            this._instances.forEach((instance, i) => {
                if (instance.active) {
                    this.appendDecal(next, i);
                }
            });
        } else {
            const changed = entry.terrain.minY !== node.minY || entry.terrain.maxY !== node.maxY;
            entry.terrain = node;
            entry.draws.forEach((draw) => this.updateBinding(draw, node, changed));
        }
    }

    public updateView (position: Readonly<Vec3>): void {
        this._viewPosition.set(position);
        const local = Vec3.subtract(new Vec3(), position, this._node.worldPosition);
        const changes = new Uint8Array(this._instances.length);
        let changed = false;
        this._instances.forEach((instance, i) => {
            if (!instance.grids.length) {
                return;
            }
            const distance = Vec3.distance(local, instance.center);
            const active = distance < this._asset.decalFarDistance();
            const t = Math.max(0, Math.min(1, (distance - this._asset.decalNearDistance()) / (this._asset.decalFarDistance() - this._asset.decalNearDistance())));
            const fade = 1 - t * t * (3 - 2 * t);
            if (active === instance.active && fade === instance.fade) {
                return;
            }
            changes[i] = active && !instance.active ? 2 : 1;
            changed = true;
            instance.active = active;
            instance.fade = fade;
        });
        if (!changed) {
            return;
        }
        for (const node of this._nodes.values()) {
            node.draws = node.draws.filter((draw) => {
                const instance = this._instances[draw.decal];
                if (!instance.active) {
                    this.recycle(draw);
                    return false;
                }
                if (changes[draw.decal]) {
                    const lift = this._asset.decalLayers()[instance.decal.layer].heightScale;
                    setLandscapeInstanceAttribute(draw.model, this._scratch, 'a_decalFade', new Vec4(lift, instance.fade, 0, 0));
                }
                return true;
            });
        }
        changes.forEach((change, i) => {
            if (change === 2) {
                for (const node of this._nodes.values()) {
                    this.appendDecal(node, i);
                }
            }
        });
    }

    public refreshInstances (position: Readonly<Vec3>): void {
        this.updateView(position);
        for (const entry of this._nodes.values()) {
            entry.draws.forEach((draw) => this.updateInstance(draw, entry.terrain));
        }
    }

    public preparePasses (stamp: number): void {
        for (const entry of this._nodes.values()) {
            for (const draw of entry.draws) {
                if (draw.model.enabled) {
                    draw.model.updateTransform(stamp);
                    draw.model.updateUBOs(stamp);
                }
            }
        }
    }

    public collectPassModels (node: QuadNode, frustum: Readonly<Frustum>, shadow: boolean, models: Model[]): void {
        const entry = this._nodes.get(node.address().key());
        if (!entry) {
            return;
        }
        for (const { model } of entry.draws) {
            if (model.enabled && (!shadow || model.castShadow) && intersect.aabbFrustum(model.worldBounds!, frustum)) {
                models.push(model);
            }
        }
    }

    private forEachModel (visit: (model: Model) => void): void {
        for (const entry of this._nodes.values()) {
            entry.draws.forEach((draw) => visit(draw.model));
        }
        this._pool.forEach(visit);
    }

    public onGlobalPipelineStateChanged (): void {
        this.forEachModel((model) => model.onGlobalPipelineStateChanged());
        this.compilePipelines();
    }

    public setEnabled (enabled: boolean): void {
        this._enabled = enabled;
        for (const entry of this._nodes.values()) {
            entry.draws.forEach((draw) => {
                draw.model.enabled = enabled;
            });
        }
    }

    public setWireframe (wireframe: boolean): void {
        this._wireframe = wireframe;
        this.refreshMaterials();
    }

    public refreshMaterials (): void {
        this.forEachModel((model) => {
            model.setSubModelMaterial(0, this._wireframe ? this._wire! : this._solid!);
            bindLandscapeModelTextures(model);
        });
        this.refreshInstances(this._viewPosition);
        this.compilePipelines();
    }

    public setCastShadow (enabled: boolean): void {
        this._castShadow = enabled;
        this.forEachModel((model) => {
            model.castShadow = enabled;
        });
    }

    public setReceiveShadow (enabled: boolean): void {
        this._receiveShadow = enabled;
        this.forEachModel((model) => {
            model.receiveShadow = enabled;
        });
        this.refreshMaterials();
    }

    public destroy (): void {
        this.forEachModel((model) => {
            model.detachFromScene();
            director.root!.destroyModel(model);
        });
        this._nodes.clear();
        this._pool.length = 0;
        this._instances.length = 0;
        this._pageIndexTexture?.destroy();
        this._mesh?.destroy();
        this._solid?.destroy();
        this._wire?.destroy();
        this._pageIndexTexture = null;
        this._mesh = null;
        this._solid = null;
        this._wire = null;
    }
}

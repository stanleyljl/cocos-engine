// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { RenderTexture, Texture2D } from '../asset/assets';
import { AABB, Frustum, intersect } from '../core/geometry';
import { isValid } from '../core/data/object';
import { Color } from '../core/math';
import { error, warn } from '../core/platform/debug';
import { director } from '../game/director';
import { API } from '../gfx';
import { Camera, CameraProjection, CameraUsage } from '../render-scene/scene/camera';
import { LightType } from '../render-scene/scene/light';
import { Model } from '../render-scene/scene/model';
import { SpotLight } from '../render-scene/scene/spot-light';
import { CSMOptimizationMode, ShadowType } from '../render-scene/scene/shadows';
import { RenderScene } from '../render-scene/core/render-scene';
import { PipelineSceneData } from '../rendering/pipeline-scene-data';
import { Node } from '../scene-graph/node';
import { LandscapeAsset } from './Asset';
import { LandscapeDebugData } from './Data';
import { LandscapeQuery, LandscapeQueryStatus } from './Query';
import { LandscapeSchedule } from './Schedule';
import { LandscapeRenderer } from './Renderer';
import * as config from './Config';

interface QuerySource {
    node: Node;
    radius: number;
}

interface PassSelection {
    frustum: Readonly<Frustum>;
    shadow: boolean;
    models: Model[];
}

// Component-facing contract shared with the generated native LandscapeInstance binding.
// Native initialization is synchronous; Web resolves only after async resources
// are ready. In either case true means the complete runtime has been published.
export interface LandscapeInstanceInterface {
    onEnable(node: Node, lodQualityScale: number, queryCacheCapacity: number): boolean | Promise<boolean>;
    onDisable(): void;
    destroy(): void;
    isInitialized(): boolean;
    isReady(): boolean;
    update(): void;
    drawDebugBounds(): void;
    setAssetPath(path: string): void;
    setDebugData(data: LandscapeDebugData): void;
    setHoleMode(mode: number): void;
    setCastShadow(enabled: boolean): void;
    setReceiveShadow(enabled: boolean): void;
    setGlobalColorMap(texture: Texture2D | null): void;
    setGlobalColorStrength(strength: number): void;
    getVTAtlas(): Texture2D | RenderTexture | null;
    addDecal(x: number, z: number, size: number, layer: number): number;
    removeDecal(id: number): boolean;
    addQuerySource(node: Node, radius: number): number;
    setQuerySourceRadius(id: number, radius: number): boolean;
    removeQuerySource(id: number): void;
    getQuerySourceStatus(id: number): number;
    sampleSurface(x: number, z: number, output: Float32Array): number;
}

// Non-component runtime counterpart of cc::landscape::LandscapeInstance. The component
// selects JSB or this WebGL2 implementation; scenes only see initialized objects.
export class LandscapeInstance implements LandscapeInstanceInterface {
    private _node: Node | null = null;
    private _scene: RenderScene | null = null;
    private _asset: LandscapeAsset | null = null;
    private _schedule: LandscapeSchedule | null = null;
    private _renderer: LandscapeRenderer | null = null;
    private _query: LandscapeQuery | null = null;
    private _pendingAsset: LandscapeAsset | null = null;
    private _pendingRenderer: LandscapeRenderer | null = null;
    private _generation = 0;
    private _assetPath = '';
    private _debugData = new LandscapeDebugData();
    private _holeMode = 0;
    private _castShadow = true;
    private _receiveShadow = true;
    private _globalColorStrength = 0.1;
    private _globalColorMap: Texture2D | null = null;
    private readonly _passes: PassSelection[] = [];
    // Registrations survive disable/re-enable; destroy releases their node references.
    private readonly _querySources = new Map<number, QuerySource>();
    private _nextQuerySource = 1;

    public async onEnable (node: Node, lodQualityScale: number, queryCacheCapacity: number): Promise<boolean> {
        if (director.root!.device.gfxAPI !== API.WEBGL2) {
            warn('[Landscape] Web runtime requires WebGL2; Landscape was not started');
            return false;
        }
        if (this._renderer || lodQualityScale <= 0 || !Number.isInteger(queryCacheCapacity) || queryCacheCapacity <= 0 || queryCacheCapacity > 4096) {
            return false;
        }
        this.onDisable();
        const generation = this._generation;
        const scene = node.scene?.renderScene;
        if (!scene || !this._assetPath) {
            return false;
        }
        if (!LandscapeInstance.isTranslationOnly(node)) {
            error('[Landscape] Only translation is supported; initialization failed');
            return false;
        }
        const asset = new LandscapeAsset();
        this._pendingAsset = asset;
        try {
            if (!await asset.load(this._assetPath) || generation !== this._generation) {
                if (generation === this._generation) {
                    this.onDisable();
                }
                return false;
            }
            const renderer = new LandscapeRenderer();
            this._pendingRenderer = renderer;
            if (!await renderer.init(node, scene, asset) || generation !== this._generation) {
                if (generation === this._generation) {
                    this.onDisable();
                    error('[Landscape] WebGL2 resource initialization failed; Landscape was not started');
                }
                return false;
            }
            // The node or its ancestors may change while Web resources load.
            if (!LandscapeInstance.isTranslationOnly(node)) {
                this.onDisable();
                error('[Landscape] Only translation is supported; initialization failed');
                return false;
            }
            const schedule = new LandscapeSchedule(asset, renderer, lodQualityScale);
            const query = new LandscapeQuery(asset.data(), (x, z, complete) => asset.requestQueryTile(x, z, complete), queryCacheCapacity);
            this.applyRendererSettings(renderer);
            this._node = node;
            this._scene = scene;
            this._asset = asset;
            this._schedule = schedule;
            this._renderer = renderer;
            this._query = query;
            this._pendingAsset = null;
            this._pendingRenderer = null;
            scene.addLandscape(this);
            return true;
        } catch (cause) {
            if (generation === this._generation) {
                this.onDisable();
                error(`[Landscape] Initialization failed: ${String(cause)}`);
            }
            return false;
        }
    }

    private applyRendererSettings (renderer: LandscapeRenderer): void {
        renderer.setDebugData(this._debugData);
        renderer.setHoleMode(this._holeMode);
        renderer.setGlobalColorMap(this._globalColorMap);
        renderer.setGlobalColorStrength(this._globalColorStrength);
        renderer.setCastShadow(this._castShadow);
        renderer.setReceiveShadow(this._receiveShadow);
    }

    public onDisable (): void {
        ++this._generation;
        this._scene?.removeLandscape(this);
        this._pendingRenderer?.destroy();
        this._pendingAsset?.destroy();
        this._schedule?.destroy();
        this._schedule = null;
        this._renderer?.destroy();
        this._query?.destroy();
        this._asset?.destroy();
        this._pendingRenderer = null;
        this._pendingAsset = null;
        this._renderer = null;
        this._query = null;
        this._asset = null;
        this._scene = null;
        this._node = null;
        this._passes.length = 0;
    }

    public destroy (): void {
        this.onDisable();
        this._querySources.clear();
    }

    public isInitialized (): boolean {
        return this._renderer !== null;
    }
    public isReady (): boolean {
        return this._schedule !== null && this._schedule.isReady();
    }
    public setAssetPath (path: string): void {
        this._assetPath = path;
    }
    public getVTAtlas (): Texture2D | null {
        return this._renderer ? this._renderer.getVTAtlas() : null;
    }

    public setDebugData (data: LandscapeDebugData): void {
        this._debugData = { ...data };
        this._renderer?.setDebugData(this._debugData);
    }

    public setHoleMode (mode: number): void {
        this._holeMode = mode === 0 ? 0 : 1;
        this._renderer?.setHoleMode(this._holeMode);
    }

    public setCastShadow (enabled: boolean): void {
        this._castShadow = enabled;
        this._renderer?.setCastShadow(enabled);
    }

    public setReceiveShadow (enabled: boolean): void {
        this._receiveShadow = enabled;
        this._renderer?.setReceiveShadow(enabled);
    }

    public setGlobalColorMap (texture: Texture2D | null): void {
        this._globalColorMap = texture;
        this._renderer?.setGlobalColorMap(texture);
    }

    public setGlobalColorStrength (strength: number): void {
        this._globalColorStrength = Math.min(1, Math.max(0, strength));
        this._renderer?.setGlobalColorStrength(this._globalColorStrength);
    }

    public update (): void {
        if (!this._renderer) {
            return;
        }
        for (const id of this._querySources.keys()) {
            this.getQuerySourceStatus(id);
        }
        this._query!.update();
        const camera = this.pickMainCamera();
        if (!camera) {
            return;
        }
        camera.update();
        this._schedule!.update(camera.position, this._node!.worldPosition);
    }

    private pickMainCamera (): Camera | undefined {
        // All render cameras share LOD, VT, morph and decal fade from this camera.
        // Scene editing uses the editor's view; runtime uses the first perspective camera.
        if (EDITOR_NOT_IN_PREVIEW) {
            return this._scene!.cameras.find((camera) => camera.enabled && camera.cameraUsage === CameraUsage.SCENE_VIEW);
        }
        return this._scene!.cameras.find((camera) => camera.projectionType === CameraProjection.PERSPECTIVE);
    }

    public isVisible (camera: Camera): boolean {
        const layer = this._node!.layer;
        return (camera.visibility & layer) === layer;
    }

    // Once before all cameras, while the render command buffer is recording.
    public prepareRenderData (): void {
        this._schedule!.prepareRenderData(director.getTotalFrames());
    }

    // Per camera, after its light frusta are updated. Shared geometry stays unchanged.
    public preparePasses (camera: Camera, sceneData: PipelineSceneData): void {
        this._passes.length = 0;
        if (!this.isVisible(camera)) {
            return;
        }
        this.addPass(camera.frustum, false);
        const cascades = this.addShadowPasses(sceneData);
        this.collectPassModels(cascades);
    }

    private collectPassModels (cascades: number): void {
        for (const pass of this._passes) {
            this._schedule!.collectPassModels(pass.frustum, pass.shadow, pass.models);
        }
        if (cascades >= 2) {
            const covered = new Set<Model>();
            for (let i = 1; i <= cascades; ++i) {
                const pass = this._passes[i];
                pass.models = pass.models.filter((model) => {
                    if (covered.has(model)) {
                        return false;
                    }
                    if (i < cascades && intersect.aabbFrustumCompletelyInside(model.worldBounds!, pass.frustum)) {
                        covered.add(model);
                    }
                    return true;
                });
            }
        }
    }

    private addPass (frustum: Readonly<Frustum>, shadow: boolean): void {
        this._passes.push({ frustum, shadow, models: [] });
    }

    private addShadowPasses (sceneData: PipelineSceneData): number {
        let cascades = 0;
        const shadows = sceneData.shadows;
        if (!this._castShadow || !shadows.enabled || shadows.type !== ShadowType.ShadowMap) {
            return cascades;
        }
        const light = this._scene!.mainLight;
        const layers = sceneData.csmLayers;
        if (light && light.shadowEnabled) {
            if (light.shadowFixedArea) {
                this.addPass(layers.specialLayer.validFrustum, true);
            } else {
                const count = sceneData.csmSupported ? light.csmLevel : 1;
                if (light.csmOptimizationMode === CSMOptimizationMode.RemoveDuplicates) {
                    cascades = count;
                }
                for (let i = 0; i < count; ++i) {
                    this.addPass(layers.layers[i].validFrustum, true);
                }
            }
        }
        for (const light of sceneData.validPunctualLights) {
            if (light.type === LightType.SPOT && (light as SpotLight).shadowEnabled) {
                this.addPass((light as SpotLight).frustum, true);
            }
        }
        return cascades;
    }

    public getPassModels (frustum: Readonly<Frustum>, shadow: boolean): readonly Model[] {
        return this._passes.find((pass) => pass.frustum === frustum && pass.shadow === shadow)?.models || [];
    }

    public onGlobalPipelineStateChanged (): void {
        this._renderer?.onGlobalPipelineStateChanged();
    }

    public addDecal (x: number, z: number, size: number, layer: number): number {
        const id = this._renderer ? this._renderer.addDecal(x, z, size, layer) : 0;
        if (id === 0) {
            warn(`[Landscape] Cannot add decal: initialize terrain, use a valid preloaded layer and region, and keep at most ${config.DECAL_INSTANCE_MAX} instances.`);
        }
        return id;
    }

    public removeDecal (id: number): boolean {
        return this._renderer !== null && this._renderer.removeDecal(id);
    }

    private static isTranslationOnly (node: Node): boolean {
        const m = node.worldMatrix;
        const values = [m.m00, m.m01, m.m02, m.m03, m.m04, m.m05, m.m06, m.m07, m.m08, m.m09, m.m10, m.m11];
        return values.every((value, i) => Math.abs(value - (i === 0 || i === 5 || i === 10 ? 1 : 0)) <= 1e-5);
    }

    public addQuerySource (node: Node, radius: number): number {
        if (!isValid(node, true) || radius < 0 || this._nextQuerySource > 0xFFFFFFFF) {
            return 0;
        }
        const id = this._nextQuerySource++;
        this._querySources.set(id, { node, radius });
        return id;
    }

    public setQuerySourceRadius (id: number, radius: number): boolean {
        const source = this._querySources.get(id);
        if (!source || radius < 0) {
            return false;
        }
        source.radius = radius;
        return true;
    }

    public removeQuerySource (id: number): void {
        this._querySources.delete(id);
        this._query?.removeSource(id);
    }

    public getQuerySourceStatus (id: number): LandscapeQueryStatus {
        const source = this._querySources.get(id);
        if (!source) {
            return LandscapeQueryStatus.MISS;
        }
        if (!isValid(source.node, true)) {
            this.removeQuerySource(id);
            return LandscapeQueryStatus.MISS;
        }
        if (!this._renderer || !source.node.activeInHierarchy) {
            this._query?.removeSource(id);
            return LandscapeQueryStatus.NOT_READY;
        }
        if (!LandscapeInstance.isTranslationOnly(this._node!)) {
            this._query!.removeSource(id);
            return LandscapeQueryStatus.ERROR;
        }
        const position = source.node.worldPosition;
        const origin = this._node!.worldPosition;
        return this._query!.addSource(id, { x: position.x - origin.x, z: position.z - origin.z }, source.radius);
    }

    public sampleSurface (x: number, z: number, output: Float32Array): LandscapeQueryStatus {
        if (output.length < 8) {
            return LandscapeQueryStatus.ERROR;
        }
        if (!this._renderer) {
            return LandscapeQueryStatus.NOT_READY;
        }
        if (!LandscapeInstance.isTranslationOnly(this._node!)) {
            return LandscapeQueryStatus.ERROR;
        }
        const origin = this._node!.worldPosition;
        const result = this._query!.sample({ x: x - origin.x, z: z - origin.z });
        if (result.status === LandscapeQueryStatus.HIT) {
            output[0] = x;
            output[1] = result.position.y + origin.y;
            output[2] = z;
            output[3] = result.normal.x;
            output[4] = result.normal.y;
            output[5] = result.normal.z;
            output[6] = result.surfaceType;
            output[7] = result.surfaceWeight;
        }
        return result.status;
    }

    public drawDebugBounds (): void {
        if (!this._renderer) {
            return;
        }
        const camera = this.pickMainCamera();
        if (!camera) {
            return;
        }
        camera.initGeometryRenderer();
        const geometry = camera.geometryRenderer;
        if (!geometry) {
            return;
        }
        const colors = [[1, 0.15, 0.15], [0.15, 0.9, 0.2], [0.2, 0.45, 1], [1, 0.9, 0.1], [1, 0.15, 0.9], [0.1, 0.9, 0.95], [0.65, 0.35, 0.85], [0.95, 0.55, 0.2], [0.9, 0.9, 0.9]].map(([r, g, b]) => new Color(r * 255, g * 255, b * 255, 255));
        const origin = this._node!.worldPosition;
        this._schedule!.forEachVisibleNode(camera.frustum, origin, (node, region) => {
            const half = region.size * 0.5;
            for (let quadrant = 0; quadrant < 4; ++quadrant) {
                const x = region.x + (quadrant & 1) * half;
                const z = region.z + (quadrant >> 1) * half;
                const box = new AABB(x + half * 0.5, (node.minY + node.maxY) * 0.5, z + half * 0.5, half * 0.5, (node.maxY - node.minY) * 0.5, half * 0.5);
                geometry.addBoundingBox(box, colors[node.level], true, false, false, true, this._node!.worldMatrix);
            }
        });
    }
}

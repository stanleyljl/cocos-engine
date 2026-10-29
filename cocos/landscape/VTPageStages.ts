// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Material, Texture2D } from '../asset/assets';
import { RenderingSubMesh } from '../asset/assets/rendering-sub-mesh';
import { Vec4 } from '../core/math';
import { Attribute, Buffer, BufferInfo, BufferTextureCopy, BufferUsageBit, Color, ColorAttachment, CommandBuffer, Device, DrawInfo, Feature, Filter, Format, Framebuffer, FramebufferInfo, InputAssembler, InputAssemblerInfo, InputState, LoadOp, MemoryUsageBit, PipelineState, PipelineStateInfo, Rect, RenderPass, RenderPassInfo, Sampler, Texture, TextureBlit, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { Pass } from '../render-scene/core/pass';
import { SetIndex } from '../rendering/define';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { MaterialLibrary } from './MaterialLibrary';
import { DecalRenderer } from './DecalRenderer';
import { GridMesh } from './GridMesh';
import { LandscapeRegion, VTPageSources } from './Data';
import * as config from './Config';

const TRIPLANAR_BLEND_SHARPNESS = 4;
const TRIPLANAR_FADE_RATIO = 1 / 16;
const CLIFF_NORMAL_Y = Math.cos(config.CLIFF_ANGLE * Math.PI / 180);

export interface VTPageInstance {
    pageInfo: Vec4;
    region: Vec4;
    splatSource: Vec4;
    projectionSource: Vec4;
}

function createPipeline (device: Device, pass: Pass, renderPass: RenderPass, ia: InputAssembler): PipelineState {
    const shader = pass.getShaderVariant();
    if (!shader) {
        throw new Error('Landscape VT shader compilation failed');
    }
    return device.createPipelineState(new PipelineStateInfo(shader, pass.pipelineLayout, renderPass, new InputState(ia.attributes), pass.rasterizerState, pass.depthStencilState, pass.blendState, pass.primitive, pass.dynamicStates));
}

function recordPagePass (commands: CommandBuffer, ia: InputAssembler, renderPass: RenderPass, pass: Pass, pipeline: PipelineState, framebuffer: Framebuffer, size: number, count: number): void {
    commands.beginRenderPass(renderPass, framebuffer, new Rect(0, 0, size, size), [new Color(0, 0, 0, 1), new Color(0.5, 0.5, 1, 1)], 1, 0);
    commands.bindPipelineState(pipeline);
    commands.bindDescriptorSet(SetIndex.MATERIAL, pass.descriptorSet);
    commands.bindInputAssembler(ia);
    const draw = new DrawInfo().copy(ia.drawInfo);
    draw.instanceCount = count;
    commands.draw(draw);
    commands.endRenderPass();
}

export class VTPageComposer {
    private _albedo: Texture2D | null = null;
    private _normal: Texture2D | null = null;
    private _framebuffer: Framebuffer | null = null;
    private _renderPass: RenderPass | null = null;
    private _initialPass: RenderPass | null = null;
    private _mesh: RenderingSubMesh | null = null;
    private _instances: Buffer | null = null;
    private _ia: InputAssembler | null = null;
    private _pipelines: PipelineState[] = [];
    private _material: Material | null = null;
    private _normalSourceTable: Texture | null = null;
    private _normalSourceRows = new Float32Array();
    private _decals!: DecalRenderer;
    private _rootLevel = 0;
    private _fallbackGlobalColor!: Texture;
    private _globalColorSampler!: Sampler;
    private _globalColorMap: Texture2D | null = null;
    private _globalColorStrength = 0.1;
    private _effectiveGlobalColorStrength = 0;
    private _inverseWidth = 0;
    private _inverseDepth = 0;

    private readonly _device: Device;
    private readonly _size: number;

    constructor (_device: Device, _size: number) {
        this._device = _device;
        this._size = _size;
    }

    public target (): Texture2D {
        return this._albedo!;
    }
    public albedo (): Texture {
        return this._albedo!.getGFXTexture()!;
    }
    public normal (): Texture {
        return this._normal!.getGFXTexture()!;
    }
    public material (): Material {
        return this._material!;
    }
    public size (): number {
        return this._size;
    }
    public inputAssembler (): InputAssembler {
        return this._ia!;
    }
    public renderPass (initialize = false): RenderPass {
        return initialize ? this._initialPass! : this._renderPass!;
    }

    public init (asset: LandscapeAsset, residency: LandscapeResidency, materials: MaterialLibrary, decals: DecalRenderer): boolean {
        const device = this._device;
        const data = asset.data();
        this._decals = decals;
        this._rootLevel = data.maxLevel;
        // This runtime is WebGL2-only and needs two color attachments. Its GFX
        // backend reports MRT through Feature; maxColorRenderTargets is unset.
        if (!device.hasFeature(Feature.MULTIPLE_RENDER_TARGETS)) {
            throw new Error('Landscape VT composition requires multiple render targets');
        }
        if (this._size > device.capabilities.maxTextureSize) {
            throw new Error(`Landscape VT atlas size ${this._size} exceeds device limit ${device.capabilities.maxTextureSize}`);
        }
        const createAtlas = (): Texture2D => {
            const texture = new Texture2D();
            texture.reset({ width: this._size, height: this._size, format: Texture2D.PixelFormat.RGBA8888, mipmapLevel: config.VT_MIP_LEVELS, baseLevel: 0, maxLevel: config.VT_MIP_LEVELS - 1 });
            return texture;
        };
        this._albedo = createAtlas();
        this._normal = createAtlas();
        const colors = [new ColorAttachment(Format.RGBA8), new ColorAttachment(Format.RGBA8)];
        for (const color of colors) {
            color.loadOp = LoadOp.LOAD;
        }
        this._renderPass = device.createRenderPass(new RenderPassInfo(colors));
        // WebGL2 retains the attachment descriptions by reference. Keep the
        // initial clear separate so later page batches preserve the atlas.
        const initialColors = [new ColorAttachment(Format.RGBA8), new ColorAttachment(Format.RGBA8)];
        for (const color of initialColors) {
            color.loadOp = LoadOp.CLEAR;
        }
        this._initialPass = device.createRenderPass(new RenderPassInfo(initialColors));
        this._framebuffer = device.createFramebuffer(new FramebufferInfo(this._renderPass, [this.albedo(), this.normal()]));
        this._material = new Material();
        this._material.initialize({ effectName: 'landscape/builtin-landscape-vt-compose', defines: { LANDSCAPE_VT_COMPACT: false, LANDSCAPE_HEIGHT_UNORM: false } });
        if (this._material.passes.length !== 2) {
            throw new Error(`Landscape VT compose material requires two passes; got ${this._material.passes.length}`);
        }
        const requiredProperties = ['vtLayout', 'splatmap', 'albedoHeightMap', 'normalRoughnessAOMap', 'materialParams', 'globalColorMap', 'globalColorParams', 'heightBlendParams', 'terrainNormalMap', 'normalSourceMap', 'terrainHeightMap', 'terrainHeightParams', 'triplanarParams'];
        for (const name of requiredProperties) {
            if (!this._material.passes[0].getHandle(name)) {
                throw new Error(`Landscape VT effect is missing ${name}; rebuild project effects`);
            }
        }
        this.bindTexture('splatmap', residency.splatArray(), residency.splatSampler());
        this.bindTexture('terrainNormalMap', residency.normalArray(), residency.heightSampler());
        this.bindTexture('terrainHeightMap', residency.heightArray(), residency.heightSampler());
        this.bindTexture('albedoHeightMap', materials.albedoHeight(), materials.repeatSampler());
        this.bindTexture('normalRoughnessAOMap', materials.normalRoughnessAO(), materials.repeatSampler());
        this._fallbackGlobalColor = materials.whiteTexture();
        this._globalColorSampler = materials.clampSampler();
        this._inverseWidth = 1 / data.width();
        this._inverseDepth = 1 / data.depth();
        this.applyGlobalColorMap();
        this._material.setProperty('materialParams', [...materials.materialParams()]);
        this._material.setProperty('terrainHeightParams', new Vec4(data.heightScale, data.heightBias, data.sectorSize, 0));
        this._material.setProperty('triplanarParams', new Vec4(1, (config.VT_PAGE_BORDER + 2) / config.VT_PAGE_INTERIOR, TRIPLANAR_FADE_RATIO, TRIPLANAR_BLEND_SHARPNESS));
        this._material.setProperty('vtLayout', new Vec4(this._size, config.VT_PAGE_RES, config.VT_PAGE_BORDER, device.capabilities.screenSpaceSignY * device.capabilities.clipSpaceSignY));
        if (!decals.bindComposeMaterial(this._material, materials)) {
            throw new Error('Landscape VT compose material is missing decal texture bindings');
        }
        const count = (this._size / config.VT_PAGE_RES) ** 2;
        this._normalSourceTable = device.createTexture(new TextureInfo(TextureType.TEX2D, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RGBA32F, config.VT_NORMAL_SOURCE_COUNT, count));
        this._normalSourceRows = new Float32Array(count * config.VT_NORMAL_SOURCE_COUNT * 4);
        this.bindTexture('normalSourceMap', this._normalSourceTable, residency.splatSampler());
        for (const pass of this._material.passes) {
            pass.update();
        }
        this._mesh = GridMesh.createVTQuad(device);
        this._instances = device.createBuffer(new BufferInfo(BufferUsageBit.VERTEX | BufferUsageBit.TRANSFER_DST, MemoryUsageBit.DEVICE, count * 64, 64));
        const attributes = this._mesh.attributes.slice();
        for (const [i, name] of ['a_vtPage', 'a_vtRegion', 'a_vtSource', 'a_vtProjection'].entries()) {
            attributes.push(new Attribute(name, Format.RGBA32F, false, 1, true, i + 1));
        }
        this._ia = device.createInputAssembler(new InputAssemblerInfo(attributes, [...this._mesh.vertexBuffers, this._instances], this._mesh.indexBuffer!));
        for (const pass of this._material.passes) {
            this._pipelines.push(createPipeline(device, pass, this.renderPass(), this._ia));
        }
        return true;
    }

    private bindTexture (name: string, texture: Texture, sampler: Sampler): void {
        this.material().setProperty(name, texture);
        for (const pass of this.material().passes) {
            pass.bindSampler(Pass.getBindingFromHandle(pass.getHandle(name)), sampler);
        }
    }

    public prepareInputs (slot: number, region: LandscapeRegion, sources: VTPageSources, residency: LandscapeResidency): VTPageInstance | null {
        const splat = residency.resolve(sources.splat);
        const projection = residency.resolve(sources.splat.ancestor(this._rootLevel));
        if (splat.layer < 0 || projection.layer < 0) {
            return null;
        }
        const normals = sources.normals.map((source) => residency.resolve(source));
        if (normals.some((normal) => normal.layer < 0)) {
            return null;
        }
        for (let i = 0; i < normals.length; ++i) {
            Vec4.toArray(this._normalSourceRows, residency.shaderParams(normals[i]), (slot * config.VT_NORMAL_SOURCE_COUNT + i) * 4);
        }
        const pass = this.material().passes[0];
        const enabled = pass.getUniform(pass.getHandle('triplanarParams'), new Vec4()).x > 0;
        const cliff = enabled && residency.minimumNormalY(projection.address) < CLIFF_NORMAL_Y;
        const bounds = new Vec4(region.x, region.z, region.size, 0);
        return {
            pageInfo: new Vec4(slot, this._decals.collectPageDecals(slot, bounds), cliff ? 1 : 0, 0),
            region: bounds,
            splatSource: residency.shaderParams(splat),
            projectionSource: residency.shaderParams(projection),
        };
    }

    public uploadInputs (): void {
        this._decals.updateComposeMaterial(this.material());
        this._decals.uploadPageDecals();
        const copy = new BufferTextureCopy();
        copy.texExtent.width = config.VT_NORMAL_SOURCE_COUNT;
        copy.texExtent.height = this._normalSourceRows.length / (config.VT_NORMAL_SOURCE_COUNT * 4);
        this._device.copyBuffersToTexture([this._normalSourceRows], this._normalSourceTable!, [copy]);
        for (const pass of this.material().passes) {
            pass.update();
        }
    }

    public record (commands: CommandBuffer, instances: readonly VTPageInstance[], initialize: boolean): void {
        const bytes = new Float32Array(instances.length * 16);
        instances.forEach((instance, i) => {
            Vec4.toArray(bytes, instance.pageInfo, i * 16);
            Vec4.toArray(bytes, instance.region, i * 16 + 4);
            Vec4.toArray(bytes, instance.splatSource, i * 16 + 8);
            Vec4.toArray(bytes, instance.projectionSource, i * 16 + 12);
        });
        commands.updateBuffer(this._instances!, bytes);
        const variant = instances[0].pageInfo.z;
        recordPagePass(commands, this.inputAssembler(), this.renderPass(initialize), this.material().passes[variant], this._pipelines[variant], this._framebuffer!, this._size, instances.length);
    }

    public setGlobalColorMap (texture: Texture2D | null): void {
        this._globalColorMap = texture;
        this.applyGlobalColorMap();
    }

    private applyGlobalColorMap (): void {
        const texture = this._globalColorMap?.getGFXTexture();
        this._effectiveGlobalColorStrength = texture ? this._globalColorStrength : 0;
        this.bindTexture('globalColorMap', texture || this._fallbackGlobalColor, this._globalColorSampler);
        this.material().setProperty('globalColorParams', new Vec4(this._inverseWidth, this._inverseDepth, this._effectiveGlobalColorStrength, 0));
    }

    public setGlobalColorStrength (strength: number): boolean {
        const clamped = Math.min(1, Math.max(0, strength));
        if (clamped === this._globalColorStrength) {
            return false;
        }
        this._globalColorStrength = clamped;
        const previous = this._effectiveGlobalColorStrength;
        this.applyGlobalColorMap();
        return previous !== this._effectiveGlobalColorStrength;
    }

    public setHeightBlendEnabled (enabled: boolean): boolean {
        return this.setEnabled('heightBlendParams', enabled);
    }
    public setTriplanarEnabled (enabled: boolean): boolean {
        return this.setEnabled('triplanarParams', enabled);
    }

    private setEnabled (name: string, enabled: boolean): boolean {
        const pass = this.material().passes[0];
        const params = pass.getUniform(pass.getHandle(name), new Vec4());
        const strength = enabled ? 1 : 0;
        if (params.x === strength) {
            return false;
        }
        params.x = strength;
        this.material().setProperty(name, params);
        return true;
    }

    public destroy (): void {
        for (const pipeline of this._pipelines) {
            pipeline.destroy();
        }
        this._pipelines = [];
        this._ia?.destroy();
        this._instances?.destroy();
        this._mesh?.destroy();
        this._framebuffer?.destroy();
        this._renderPass?.destroy();
        this._initialPass?.destroy();
        this._normalSourceTable?.destroy();
        this._material?.destroy();
        this._albedo?.destroy();
        this._normal?.destroy();
        this._ia = null;
        this._instances = null;
        this._mesh = null;
        this._framebuffer = null;
        this._renderPass = null;
        this._initialPass = null;
        this._normalSourceTable = null;
        this._material = null;
        this._albedo = null;
        this._normal = null;
    }
}

interface MipPass {
    albedo: Texture;
    normal: Texture;
    framebuffer: Framebuffer;
    material: Material;
    pipeline: PipelineState;
}

export class VTPageMipGenerator {
    private readonly _mips: MipPass[] = [];

    private readonly _device: Device;

    constructor (_device: Device) {
        this._device = _device;
    }

    public init (composer: VTPageComposer): boolean {
        let sourceAlbedo = composer.albedo();
        let sourceNormal = composer.normal();
        for (let level = 1; level < config.VT_MIP_LEVELS; ++level) {
            const size = composer.size() / 2 ** level;
            const info = new TextureInfo(TextureType.TEX2D, TextureUsageBit.COLOR_ATTACHMENT | TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_SRC, Format.RGBA8, size, size);
            const albedo = this._device.createTexture(info);
            const normal = this._device.createTexture(info);
            const framebuffer = this._device.createFramebuffer(new FramebufferInfo(composer.renderPass(), [albedo, normal]));
            const material = new Material();
            try {
                material.initialize({ effectName: 'landscape/builtin-landscape-vt-mip', defines: { LANDSCAPE_VT_COMPACT: false } });
                if (!material.passes.length) {
                    throw new Error('Landscape VT mip material has no passes');
                }
                material.setProperty('sourceAlbedo', sourceAlbedo);
                material.setProperty('sourceNormal', sourceNormal);
                // Each source is a separate texture with its data at mip zero.
                material.setProperty('mipLayout', new Vec4(size * 2, config.VT_PAGE_RES / 2 ** (level - 1), 0, this._device.capabilities.screenSpaceSignY * this._device.capabilities.clipSpaceSignY));
                const pass = material.passes[0];
                pass.update();
                const pipeline = createPipeline(this._device, pass, composer.renderPass(), composer.inputAssembler());
                this._mips.push({ albedo, normal, framebuffer, material, pipeline });
            } catch (cause) {
                framebuffer.destroy();
                material.destroy();
                normal.destroy();
                albedo.destroy();
                throw cause;
            }
            sourceAlbedo = albedo;
            sourceNormal = normal;
        }
        return true;
    }

    public record (commands: CommandBuffer, composer: VTPageComposer, instances: readonly VTPageInstance[], initialize: boolean): void {
        const slots = composer.size() / config.VT_PAGE_RES;
        for (let i = 0; i < this._mips.length; ++i) {
            const level = i + 1;
            const mip = this._mips[i];
            const size = composer.size() / 2 ** level;
            recordPagePass(commands, composer.inputAssembler(), composer.renderPass(initialize), mip.material.passes[0], mip.pipeline, mip.framebuffer, size, instances.length);
            const pageSize = config.VT_PAGE_RES / 2 ** level;
            const regions = instances.map((instance) => {
                const copy = new TextureBlit();
                copy.srcOffset.x = (instance.pageInfo.x % slots) * pageSize;
                copy.srcOffset.y = Math.floor(instance.pageInfo.x / slots) * pageSize;
                copy.dstOffset.copy(copy.srcOffset);
                copy.srcExtent.width = pageSize;
                copy.srcExtent.height = pageSize;
                copy.dstExtent.copy(copy.srcExtent);
                copy.dstSubres.mipLevel = level;
                return copy;
            });
            // Independent read/write objects avoid WebGL framebuffer feedback.
            commands.blitTexture(mip.albedo, composer.albedo(), regions, Filter.POINT);
            commands.blitTexture(mip.normal, composer.normal(), regions, Filter.POINT);
        }
    }

    public destroy (): void {
        for (const mip of this._mips) {
            mip.pipeline.destroy();
            mip.framebuffer.destroy();
            mip.material.destroy();
            mip.albedo.destroy();
            mip.normal.destroy();
        }
        this._mips.length = 0;
    }
}

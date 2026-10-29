// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Address, BufferTextureCopy, Device, Filter, Format, Sampler, SamplerInfo, Texture, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { Vec4 } from '../core/math';
import { error } from '../core/platform/debug';
import { LandscapeAsset } from './Asset';
import * as config from './Config';

function srgbToLinear (v: number): number {
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
}
function linearToSrgb (v: number): number {
    return v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055;
}
function toByte (v: number): number {
    return Math.round(Math.min(1, Math.max(0, v)) * 255);
}

type MipKind = 'albedo' | 'normal' | 'decal';

function buildMip (source: Uint8Array, size: number, kind: MipKind): Uint8Array {
    const next = Math.max(1, Math.floor(size / 2));
    const result = new Uint8Array(next * next * 4);
    for (let y = 0; y < next; ++y) {
        for (let x = 0; x < next; ++x) {
            const x0 = Math.floor(x * size / next);
            const x1 = Math.floor((x + 1) * size / next);
            const y0 = Math.floor(y * size / next);
            const y1 = Math.floor((y + 1) * size / next);
            const sum = [0, 0, 0, 0];
            let nz = 0;
            for (let sy = y0; sy < y1; ++sy) {
                for (let sx = x0; sx < x1; ++sx) {
                    const p = (sy * size + sx) * 4;
                    for (let c = 0; c < 4; ++c) {
                        const v = source[p + c] / 255;
                        sum[c] += kind === 'albedo' && c < 3 ? srgbToLinear(v) : kind === 'normal' && c < 2 ? v * 2 - 1 : v;
                    }
                    if (kind === 'normal') {
                        const nx = source[p] / 127.5 - 1;
                        const ny = source[p + 1] / 127.5 - 1;
                        nz += Math.sqrt(Math.max(0, 1 - nx * nx - ny * ny));
                    }
                }
            }
            const count = (x1 - x0) * (y1 - y0);
            const p = (y * next + x) * 4;
            const length = Math.hypot(sum[0], sum[1], nz);
            for (let c = 0; c < 4; ++c) {
                if (kind === 'normal' && c < 2) {
                    result[p + c] = length > 1e-6 ? toByte(sum[c] / length * 0.5 + 0.5) : 128;
                } else {
                    result[p + c] = toByte(kind === 'albedo' && c < 3 ? linearToSrgb(sum[c] / count) : sum[c] / count);
                }
            }
        }
    }
    return result;
}

export class MaterialLibrary {
    private _albedoHeight: Texture | null = null;
    private _normalRoughnessAO: Texture | null = null;
    private _decalAlbedo: Texture | null = null;
    private _decalNormal: Texture | null = null;
    private _decalHeight: Texture | null = null;
    private _whiteTexture: Texture | null = null;
    private _repeatSampler!: Sampler;
    private _clampSampler!: Sampler;
    private _materialParams: Vec4[] = [];
    private _generation = 0;

    public valid (): boolean {
        return !!this._albedoHeight && !!this._normalRoughnessAO;
    }
    public albedoHeight (): Texture {
        return this._albedoHeight!;
    }
    public normalRoughnessAO (): Texture {
        return this._normalRoughnessAO!;
    }
    public decalAlbedo (): Texture {
        return this._decalAlbedo!;
    }
    public decalNormal (): Texture {
        return this._decalNormal!;
    }
    public decalHeight (): Texture {
        return this._decalHeight!;
    }
    public whiteTexture (): Texture {
        return this._whiteTexture!;
    }
    public repeatSampler (): Sampler {
        return this._repeatSampler;
    }
    public clampSampler (): Sampler {
        return this._clampSampler;
    }
    public materialParams (): readonly Vec4[] {
        return this._materialParams;
    }

    public async init (device: Device, asset: LandscapeAsset): Promise<boolean> {
        const generation = this._generation;
        const create = (size: number, layers: number, mips = true): Texture => {
            if (size > device.capabilities.maxTextureSize || layers > device.capabilities.maxArrayTextureLayers) {
                throw new Error('Material array exceeds device capacity');
            }
            const info = new TextureInfo(TextureType.TEX2D_ARRAY, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RGBA8, size, size);
            info.layerCount = layers;
            info.levelCount = mips ? Math.floor(Math.log2(size)) + 1 : 1;
            return device.createTexture(info);
        };
        try {
            const size = asset.materialResolution();
            const layers = asset.materialLayers();
            this._albedoHeight = create(size, layers.length);
            this._normalRoughnessAO = create(size, layers.length);
            this._materialParams = Array.from({ length: config.MATERIAL_LIBRARY_MAX }, () => new Vec4());
            for (const layer of layers) {
                const [color, normal] = await Promise.all([
                    LandscapeAsset.loadColor(layer.albedoHeight, size, 4),
                    LandscapeAsset.loadColor(layer.normalRoughnessAO, size, 4),
                ]);
                if (generation !== this._generation) {
                    return false;
                }
                this.uploadMipChain(device, this._albedoHeight, size, layer.id, color, 'albedo');
                this.uploadMipChain(device, this._normalRoughnessAO, size, layer.id, normal, 'normal');
                this._materialParams[layer.id].set(layer.pixelsPerMeter / size, layer.globalColorInfluence, 0, 0);
            }
            const decals = asset.decalLayers();
            const decalSize = decals.length ? asset.decalResolution() : 1;
            const count = Math.max(1, decals.length);
            this._decalAlbedo = create(decalSize, count);
            this._decalNormal = create(decalSize, count);
            this._decalHeight = create(decalSize, count, false);
            for (let i = 0; i < count; ++i) {
                let color = new Uint8Array([0, 0, 0, 0]);
                let normal = new Uint8Array([128, 128, 255, 255]);
                let height = new Uint8Array([0, 0, 0, 255]);
                if (decals.length) {
                    [color, normal, height] = await Promise.all([
                        LandscapeAsset.loadColor(decals[i].albedoAlpha, decalSize, 4),
                        LandscapeAsset.loadColor(decals[i].normalRoughnessAO, decalSize, 4),
                        LandscapeAsset.loadColor(decals[i].height, decalSize, 4),
                    ]);
                }
                if (generation !== this._generation) {
                    return false;
                }
                for (let p = 0; p < color.length; p += 4) {
                    const alpha = color[p + 3] / 255;
                    color[p] = toByte(srgbToLinear(color[p] / 255) * alpha);
                    color[p + 1] = toByte(srgbToLinear(color[p + 1] / 255) * alpha);
                    color[p + 2] = toByte(srgbToLinear(color[p + 2] / 255) * alpha);
                }
                this.uploadMipChain(device, this._decalAlbedo, decalSize, i, color, 'decal');
                this.uploadMipChain(device, this._decalNormal, decalSize, i, normal, 'normal');
                this.uploadMip(device, this._decalHeight, i, 0, decalSize, height);
            }
            this._whiteTexture = device.createTexture(new TextureInfo(TextureType.TEX2D, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RGBA8, 1, 1));
            this.uploadMip(device, this._whiteTexture, 0, 0, 1, new Uint8Array([255, 255, 255, 255]));
            const sampler = new SamplerInfo();
            sampler.minFilter = Filter.LINEAR;
            sampler.magFilter = Filter.LINEAR;
            sampler.mipFilter = Filter.LINEAR;
            sampler.addressU = Address.WRAP;
            sampler.addressV = Address.WRAP;
            sampler.addressW = Address.WRAP;
            this._repeatSampler = device.getSampler(sampler);
            sampler.addressU = Address.CLAMP;
            sampler.addressV = Address.CLAMP;
            sampler.addressW = Address.CLAMP;
            this._clampSampler = device.getSampler(sampler);
            return true;
        } catch (cause) {
            if (generation === this._generation) {
                error(`[Landscape] Material library: ${String(cause)}`);
                this.destroy();
            }
            return false;
        }
    }

    public destroy (): void {
        ++this._generation;
        this._albedoHeight?.destroy();
        this._normalRoughnessAO?.destroy();
        this._decalAlbedo?.destroy();
        this._decalNormal?.destroy();
        this._decalHeight?.destroy();
        this._whiteTexture?.destroy();
        this._albedoHeight = null;
        this._normalRoughnessAO = null;
        this._decalAlbedo = null;
        this._decalNormal = null;
        this._decalHeight = null;
        this._whiteTexture = null;
        this._materialParams = [];
    }

    private uploadMip (device: Device, texture: Texture, layer: number, mip: number, size: number, pixels: Uint8Array): void {
        const copy = new BufferTextureCopy();
        copy.texExtent.width = size;
        copy.texExtent.height = size;
        copy.texSubres.mipLevel = mip;
        copy.texSubres.baseArrayLayer = layer;
        device.copyBuffersToTexture([pixels], texture, [copy]);
    }

    private uploadMipChain (device: Device, texture: Texture, size: number, layer: number, pixels: Uint8Array, kind: MipKind): void {
        for (let mip = 0; mip < texture.levelCount; ++mip) {
            this.uploadMip(device, texture, layer, mip, size, pixels);
            if (mip + 1 < texture.levelCount) {
                pixels = buildMip(pixels, size, kind);
                size = Math.max(1, Math.floor(size / 2));
            }
        }
    }
}

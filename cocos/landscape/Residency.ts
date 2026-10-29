// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Address, BufferTextureCopy, Device, Filter, Format, FormatFeatureBit, Sampler, SamplerInfo, Texture, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { Vec4 } from '../core/math';
import { LandscapeAsset, TileData } from './Asset';
import { LandscapeData, NodeAddress, NodeKey } from './Data';
import * as config from './Config';

interface Slot {
    layer: number;
    ready: boolean;
    minimumNormalY: number;
}

export interface Tile {
    address: NodeAddress;
    layer: number;
}

export class LandscapeResidency {
    private _device!: Device;
    private _asset!: LandscapeAsset;
    private _data!: LandscapeData;
    private _capacity = 0;
    private _heightArray: Texture | null = null;
    private _splatArray: Texture | null = null;
    private _normalArray: Texture | null = null;
    private _heightSampler!: Sampler;
    private _splatSampler!: Sampler;
    private _slots = new Map<NodeKey, Slot>();
    private _working: readonly NodeAddress[] = [];
    private readonly _inFlight = new Set<NodeKey>();

    public init (device: Device, asset: LandscapeAsset, layerCount: number): boolean {
        const data = asset.data();
        if (layerCount < data.sectorsX * data.sectorsZ || layerCount === 0 || layerCount > device.capabilities.maxArrayTextureLayers) {
            return false;
        }
        const filtered = FormatFeatureBit.SAMPLED_TEXTURE | FormatFeatureBit.LINEAR_FILTER;
        if ((device.getFormatFeatures(Format.RG8) & filtered) !== filtered
            || !(device.getFormatFeatures(Format.R16UI) & FormatFeatureBit.SAMPLED_TEXTURE)) {
            return false;
        }
        this._device = device;
        this._asset = asset;
        this._data = data;
        this._capacity = layerCount;
        const info = new TextureInfo(TextureType.TEX2D_ARRAY, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RG8, config.SOURCE_TILE_RESOLUTION, config.SOURCE_TILE_RESOLUTION);
        info.layerCount = layerCount;
        this._heightArray = device.createTexture(info);
        info.format = Format.R16UI;
        this._splatArray = device.createTexture(info);
        info.format = Format.RG8;
        info.width = config.SOURCE_NORMAL_RESOLUTION;
        info.height = config.SOURCE_NORMAL_RESOLUTION;
        this._normalArray = device.createTexture(info);
        if (!this.valid()) {
            this.destroy();
            return false;
        }
        const sampler = new SamplerInfo();
        sampler.minFilter = Filter.LINEAR;
        sampler.magFilter = Filter.LINEAR;
        sampler.mipFilter = Filter.NONE;
        sampler.addressU = Address.CLAMP;
        sampler.addressV = Address.CLAMP;
        sampler.addressW = Address.CLAMP;
        this._heightSampler = device.getSampler(sampler);
        sampler.minFilter = Filter.POINT;
        sampler.magFilter = Filter.POINT;
        this._splatSampler = device.getSampler(sampler);
        return true;
    }

    public valid (): boolean {
        return !!this._heightArray && !!this._splatArray && !!this._normalArray;
    }
    public heightArray (): Texture {
        return this._heightArray!;
    }
    public splatArray (): Texture {
        return this._splatArray!;
    }
    public normalArray (): Texture {
        return this._normalArray!;
    }
    public heightSampler (): Sampler {
        return this._heightSampler;
    }
    public splatSampler (): Sampler {
        return this._splatSampler;
    }
    public layerCount (): number {
        return this._capacity;
    }

    public destroy (): void {
        this._heightArray?.destroy();
        this._splatArray?.destroy();
        this._normalArray?.destroy();
        this._heightArray = null;
        this._splatArray = null;
        this._normalArray = null;
        this._capacity = 0;
        this._slots.clear();
        this._working = [];
        this._inFlight.clear();
    }

    public canReserve (sources: readonly NodeAddress[]): boolean {
        return sources.length <= this._capacity && sources.every((source) => source.level >= this._data.minTileLevel
            && source.level <= this._data.maxLevel && source.x >= 0 && source.z >= 0
            && source.x < this._data.nodesX(source.level) && source.z < this._data.nodesZ(source.level));
    }

    public reserve (sources: readonly NodeAddress[]): boolean {
        if (!this.canReserve(sources)) {
            return false;
        }
        const next = new Map<NodeKey, Slot>();
        const used = new Uint8Array(this._capacity);
        for (const source of sources) {
            const key = source.key();
            if (next.has(key)) {
                return false;
            }
            const previous = this._slots.get(key);
            const slot = previous ? { ...previous } : { layer: this._capacity, ready: false, minimumNormalY: 1 };
            next.set(key, slot);
            if (slot.layer < this._capacity) {
                used[slot.layer] = 1;
            }
        }
        let layer = 0;
        for (const source of sources) {
            const slot = next.get(source.key())!;
            if (slot.layer < this._capacity) {
                continue;
            }
            while (used[layer]) {
                ++layer;
            }
            slot.layer = layer;
            used[layer] = 1;
        }
        this._slots = next;
        this._working = sources.slice();
        return true;
    }

    public loadReserved (): boolean {
        const MAX_IN_FLIGHT = 8;
        if (this._asset.loadingFailed()) {
            return false;
        }
        for (const source of this._working) {
            const key = source.key();
            if (this._slots.get(key)!.ready || this._inFlight.has(key)) {
                continue;
            }
            if (this._inFlight.size >= MAX_IN_FLIGHT) {
                break;
            }
            if (!this._asset.requestTile(source.level, source.x, source.z)) {
                return false;
            }
            this._inFlight.add(key);
        }
        return !this._asset.loadingFailed();
    }

    public update (budget: number): void {
        for (let count = 0; count < budget; ++count) {
            const tile = this._asset.takeReadyTile();
            if (!tile) {
                break;
            }
            this._inFlight.delete(tile.key);
            this.uploadTile(tile);
        }
    }

    public resident (source: NodeAddress): number {
        const slot = this._slots.get(source.key());
        return slot && slot.ready ? slot.layer : -1;
    }

    public resolve (node: NodeAddress): Tile {
        const address = node.ancestor(Math.max(node.level, this._data.minTileLevel));
        return { address, layer: this.resident(address) };
    }

    public shaderParams (tile: Tile): Vec4 {
        const region = this._data.nodeRegion(tile.address);
        return new Vec4(region.x, region.z, region.size, tile.layer);
    }

    public minimumNormalY (source: NodeAddress): number {
        return this._slots.get(source.key())!.minimumNormalY;
    }

    public ready (): boolean {
        for (const slot of this._slots.values()) {
            if (!slot.ready) {
                return false;
            }
        }
        return true;
    }

    private uploadTile (tile: TileData): void {
        const slot = this._slots.get(tile.key);
        if (!slot || slot.ready) {
            return;
        }
        this.uploadLayer(this.heightArray(), slot.layer, tile.height);
        this.uploadLayer(this.splatArray(), slot.layer, tile.splat);
        this.uploadLayer(this.normalArray(), slot.layer, tile.normal);
        slot.minimumNormalY = tile.minimumNormalY;
        slot.ready = true;
    }

    private uploadLayer (texture: Texture, layer: number, data: ArrayBufferView): void {
        const region = new BufferTextureCopy();
        region.texExtent.width = texture.width;
        region.texExtent.height = texture.height;
        region.texExtent.depth = 1;
        region.texSubres.baseArrayLayer = layer;
        region.texSubres.layerCount = 1;
        this._device.copyBuffersToTexture([data], texture, [region]);
    }
}

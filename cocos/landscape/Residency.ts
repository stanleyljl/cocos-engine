// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Address, BufferTextureCopy, Device, Filter, Format, FormatFeatureBit, Sampler, SamplerInfo, Texture, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { Vec4 } from '../core/math';
import { LandscapeAsset, TileData } from './Asset';
import { LandscapeData, NodeAddress, NodeKey } from './Data';
import * as config from './Config';

interface Request {
    address: NodeAddress;
    count: number;
}

interface Slot {
    layer: number;
    ready: boolean;
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
    private _heightUnorm = false;
    private _heightArray: Texture | null = null;
    private _splatArray: Texture | null = null;
    private _normalArray: Texture | null = null;
    private _heightSampler!: Sampler;
    private _splatSampler!: Sampler;
    private _slots = new Map<NodeKey, Slot>();
    private _working: readonly NodeAddress[] = [];
    private readonly _requests = new Map<NodeKey, Request>();
    private _requestsDirty = false;
    private readonly _inFlight = new Set<NodeKey>();

    public init (device: Device, asset: LandscapeAsset, layerCount: number, heightUnorm: boolean): boolean {
        const data = asset.data();
        if (layerCount < data.sectorsX * data.sectorsZ || layerCount === 0) {
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
        this._heightUnorm = heightUnorm;
        const info = new TextureInfo(TextureType.TEX2D_ARRAY, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, heightUnorm ? Format.R16_UNORM : Format.RG8, config.SOURCE_HEIGHT_RESOLUTION, config.SOURCE_HEIGHT_RESOLUTION);
        info.layerCount = layerCount;
        this._heightArray = device.createTexture(info);
        info.format = Format.R16UI;
        info.width = config.SOURCE_TILE_RESOLUTION;
        info.height = config.SOURCE_TILE_RESOLUTION;
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
        this._heightUnorm = false;
        this._slots.clear();
        this._working = [];
        this._requests.clear();
        this._requestsDirty = false;
        this._inFlight.clear();
    }

    // retainedCount excludes resources present in the supplied list.
    public canFit (sources: readonly NodeAddress[], retainedCount = 0): boolean {
        return sources.length + retainedCount <= this._capacity && sources.every((source) => source.level >= this._data.minTileLevel
            && source.level <= this._data.maxTileLevel && source.x >= 0 && source.z >= 0
            && source.x < this._data.nodesX(source.level) && source.z < this._data.nodesZ(source.level));
    }

    // Each demand contributes one reference; physical work is shared by address.
    public request (source: NodeAddress): void {
        const entry = this._requests.get(source.key());
        if (entry) {
            ++entry.count;
        } else {
            this._requests.set(source.key(), { address: source, count: 1 });
            this._requestsDirty = true;
        }
    }

    public release (source: NodeAddress): void {
        const entry = this._requests.get(source.key());
        if (!entry) {
            throw new Error('Landscape source demand must be acquired before release');
        }
        if (--entry.count === 0) {
            this._requests.delete(source.key());
            this._requestsDirty = true;
        }
    }

    public canRequest (sources: readonly NodeAddress[]): boolean {
        const unique = new Map(sources.map((source) => [source.key(), source]));
        const additional = Array.from(unique.keys()).filter((key) => !this._requests.has(key)).length;
        return this.canFit(Array.from(unique.values())) && this._requests.size + additional <= this._capacity;
    }

    private applyRequests (): boolean {
        if (!this._requestsDirty) {
            return true;
        }
        const sources = Array.from(this._requests.values(), (entry) => entry.address);
        sources.sort((a, b) => a.level - b.level || a.x - b.x || a.z - b.z);
        if (!this.canFit(sources)) {
            return false;
        }
        const next = new Map<NodeKey, Slot>();
        const used = new Uint8Array(this._capacity);
        for (const source of sources) {
            const key = source.key();
            const previous = this._slots.get(key);
            const slot = previous ? { ...previous } : { layer: this._capacity, ready: false };
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
        this._working = sources;
        this._requestsDirty = false;
        return true;
    }

    private loadRequested (): boolean {
        const MAX_IN_FLIGHT = 8;
        const heightFormat = this._heightUnorm ? Format.R16UI : Format.RG8;
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
            if (!this._asset.requestTile(source.level, source.x, source.z, heightFormat)) {
                return false;
            }
            this._inFlight.add(key);
        }
        return !this._asset.loadingFailed();
    }

    public update (budget: number): boolean {
        if (this._asset.loadingFailed() || !this.applyRequests()) {
            return false;
        }
        for (let count = 0; count < budget; ++count) {
            const tile = this._asset.takeReadyTile();
            if (!tile) {
                break;
            }
            this._inFlight.delete(tile.key);
            this.uploadTile(tile);
        }
        return this.loadRequested();
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

    public ready (): boolean {
        for (const key of this._requests.keys()) {
            if (!this._slots.get(key)?.ready) {
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

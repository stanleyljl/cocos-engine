// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { DEBUG } from 'internal:constants';
import { Vec4 } from '../core/math';
import { log } from '../core/platform/debug';
import { Pass } from '../render-scene/core/pass';
import { Address, BufferTextureCopy, CommandBuffer, CommandBufferInfo, Device, Filter, Format, Sampler, SamplerInfo, Texture, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { Texture2D } from '../asset/assets';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { MaterialLibrary } from './MaterialLibrary';
import { DecalRenderer } from './DecalRenderer';
import { VTPageComposer, VTPageInstance, VTPageMipGenerator } from './VTPageStages';
import { LandscapeData, LandscapeRegion, NodeAddress, NodeKey, VTGridMapping, VTPageAddress, VTPageLayout, VTPageSources } from './Data';
import * as config from './Config';

export class VTPageTable {
    private _texture: Texture | null = null;
    private _entries = new Uint8Array();
    private readonly _freeSlots: number[] = [];
    private _size = 0;
    private _count = 0;

    private readonly _device: Device;

    constructor (_device: Device) {
        this._device = _device;
    }

    public texture (): Texture {
        return this._texture!;
    }

    public ensureCapacity (count: number): boolean {
        let size = Math.max(256, this._size);
        while (size * size < count * VTGridMapping.CELL_COUNT) {
            size *= 2;
        }
        if (size > this._device.capabilities.maxTextureSize) {
            return false;
        }
        if (size === this._size) {
            return true;
        }
        const texture = this._device.createTexture(new TextureInfo(TextureType.TEX2D, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RG8, size, size));
        if (!texture) {
            return false;
        }
        const entries = new Uint8Array(size * size * 2);
        entries.set(this._entries);
        const copy = new BufferTextureCopy();
        copy.texExtent.width = size;
        copy.texExtent.height = size;
        this._device.copyBuffersToTexture([entries], texture, [copy]);
        this._texture?.destroy();
        this._texture = texture;
        this._entries = entries;
        this._size = size;
        return true;
    }

    public allocate (): number {
        if (this._freeSlots.length) {
            return this._freeSlots.pop()!;
        }
        if ((this._count + 1) * VTGridMapping.CELL_COUNT * 2 > this._entries.length) {
            throw new Error('Landscape page table capacity was not reserved');
        }
        return this._count++;
    }

    public release (slot: number): void {
        this._freeSlots.push(slot);
    }

    public update (slot: number, mapping: VTGridMapping, pages: VTRenderer): boolean {
        const physical = mapping.pages.map((page) => {
            const index = pages.resolve(page);
            if (index < 0) {
                throw new Error('Exact VT page must be published before drawing');
            }
            return index;
        });
        const encoded = new Uint8Array(VTGridMapping.CELL_COUNT * 2);
        for (let i = 0; i < mapping.cells.length; ++i) {
            const page = mapping.cells[i];
            encoded[i * 2] = physical[page];
            encoded[i * 2 + 1] = mapping.pages[page].level;
        }
        const offset = slot * encoded.length;
        if (encoded.every((value, i) => value === this._entries[offset + i])) {
            return false;
        }
        this._entries.set(encoded, offset);
        return true;
    }

    public upload (slots: number[]): void {
        if (!slots.length) {
            return;
        }
        slots.sort((a, b) => a - b);
        const copies: BufferTextureCopy[] = [];
        const buffers: Uint8Array[] = [];
        const cells = VTGridMapping.CELL_COUNT;
        for (let i = 0; i < slots.length; ++i) {
            const first = slots[i] * cells;
            let width = cells;
            while (i + 1 < slots.length && slots[i + 1] === slots[i] + 1
                && Math.floor(slots[i + 1] * cells / this._size) === Math.floor(first / this._size)) {
                ++i;
                width += cells;
            }
            const copy = new BufferTextureCopy();
            copy.texOffset.x = first % this._size;
            copy.texOffset.y = Math.floor(first / this._size);
            copy.texExtent.width = width;
            copy.texExtent.height = 1;
            copies.push(copy);
            buffers.push(this._entries.subarray(first * 2, (first + width) * 2));
        }
        this._device.copyBuffersToTexture(buffers, this.texture(), copies);
    }

    public destroy (): void {
        this._texture?.destroy();
        this._texture = null;
        this._entries = new Uint8Array();
        this._freeSlots.length = 0;
        this._size = 0;
        this._count = 0;
    }
}

export enum PageState {
    EMPTY,
    UNCOMPOSED,
    READY,
    OUTDATED,
}

export class VTPage {
    public address = new VTPageAddress();
    public state = PageState.EMPTY;
    public lastReserved = 0;
    public needsComposition (): boolean {
        return this.state === PageState.UNCOMPOSED || this.state === PageState.OUTDATED;
    }
    public drawable (): boolean {
        return this.state === PageState.READY || this.state === PageState.OUTDATED;
    }
}

export class VTRenderer {
    private _data!: LandscapeData;
    private _layout!: VTPageLayout;
    private _pages: VTPage[] = [];
    private _reservedSlots: number[] = [];
    private readonly _lookup = new Map<NodeKey, number>();
    private _reservation = 0;
    private readonly _pageTable: VTPageTable;
    private readonly _composer: VTPageComposer;
    private readonly _mipGenerator: VTPageMipGenerator;
    private _commands: CommandBuffer | null = null;
    private _sampler!: Sampler;
    private _needsClear = true;
    private _refineFarDetail = true;

    private readonly _device: Device;
    private readonly _atlasSize: number;

    constructor (_device: Device, _atlasSize: number) {
        this._device = _device;
        this._atlasSize = _atlasSize;
        this._pageTable = new VTPageTable(_device);
        this._composer = new VTPageComposer(_device, _atlasSize);
        this._mipGenerator = new VTPageMipGenerator(_device);
    }

    public init (asset: LandscapeAsset, residency: LandscapeResidency, materials: MaterialLibrary, decals: DecalRenderer): boolean {
        const capacity = (this._atlasSize / config.VT_PAGE_RES) ** 2;
        const data = asset.data();
        if (!residency.valid() || !materials.valid() || capacity < data.sectorsX * data.sectorsZ) {
            return false;
        }
        this._data = data;
        this._layout = new VTPageLayout(data.sectorSize);
        this._pages = Array.from({ length: capacity }, () => new VTPage());
        if (!this._pageTable.ensureCapacity(1) || !this._composer.init(asset, residency, materials, decals) || !this._mipGenerator.init(this._composer)) {
            return false;
        }
        const sampler = new SamplerInfo();
        sampler.minFilter = Filter.LINEAR;
        sampler.magFilter = Filter.LINEAR;
        sampler.mipFilter = Filter.LINEAR;
        sampler.maxAnisotropy = config.VT_MAX_ANISOTROPY;
        sampler.addressU = Address.CLAMP;
        sampler.addressV = Address.CLAMP;
        sampler.addressW = Address.CLAMP;
        this._sampler = this._device.getSampler(sampler);
        this._commands = this._device.createCommandBuffer(new CommandBufferInfo(this._device.queue));
        if (config.LANDSCAPE_DEBUG) {
            log(`[Landscape] VT atlas: ${this._atlasSize}x${this._atlasSize} RGBA8 pages=${this._pages.length}`);
        }
        return true;
    }

    public destroy (): void {
        this._commands?.destroy();
        this._commands = null;
        this._mipGenerator.destroy();
        this._composer.destroy();
        this._pageTable.destroy();
        this._pages = [];
        this._reservedSlots = [];
        this._lookup.clear();
    }

    public pageTable (): VTPageTable {
        return this._pageTable;
    }
    public reservedSlots (): readonly number[] {
        return this._reservedSlots;
    }
    public page (slot: number): VTPage {
        return this._pages[slot];
    }
    public region (address: VTPageAddress): LandscapeRegion {
        return this._layout.pageRegion(address);
    }
    public getRefineFarDetail (): boolean {
        return this._refineFarDetail;
    }
    public atlas (): Texture2D {
        return this._composer.target();
    }
    public albedo (): Texture {
        return this._composer.albedo();
    }
    public normalRoughnessAO (): Texture {
        return this._composer.normal();
    }
    public sampler (): Sampler {
        return this._sampler;
    }

    public renderPages (residency: LandscapeResidency, pages: readonly VTPageAddress[], maxPages: number): number {
        if (!maxPages) {
            return 0;
        }
        const batch: VTPageInstance[] = [];
        for (const address of pages) {
            const slot = this._lookup.get(address.key())!;
            const page = this._pages[slot];
            if (!page.needsComposition()) {
                continue;
            }
            const instance = this._composer.prepareInputs(slot, this.region(address), this.selectSources(address, this._refineFarDetail), residency);
            if (instance) {
                batch.push(instance);
                if (batch.length === maxPages) {
                    break;
                }
            }
        }
        if (!batch.length) {
            return 0;
        }
        this._composer.uploadInputs();
        const commands = this._commands!;
        commands.begin();
        for (let variant = 0; variant < 2; ++variant) {
            const group = batch.filter((entry) => entry.pageInfo.z === variant);
            if (!group.length) {
                continue;
            }
            this._composer.record(commands, group, this._needsClear);
            this._mipGenerator.record(commands, this._composer, group, this._needsClear);
            this._needsClear = false;
        }
        commands.end();
        this._device.flushCommands([commands]);
        this._device.queue.submit([commands]);
        for (const instance of batch) {
            this._pages[instance.pageInfo.x].state = PageState.READY;
        }
        return batch.length;
    }

    public needsComposition (address: VTPageAddress): boolean {
        const slot = this._lookup.get(address.key());
        return slot === undefined || this._pages[slot].state !== PageState.READY;
    }

    public canReserve (pages: readonly VTPageAddress[]): boolean {
        return pages.length <= this._pages.length && pages.every((page) => {
            const side = 2 ** (this._layout.rootLevel() - page.level);
            return page.level >= 0 && page.level <= this._layout.rootLevel() && page.x >= 0 && page.z >= 0
                && page.x < this._data.sectorsX * side && page.z < this._data.sectorsZ * side;
        });
    }

    public reserve (pages: readonly VTPageAddress[]): boolean {
        if (!this.canReserve(pages)) {
            return false;
        }
        ++this._reservation;
        this._reservedSlots = pages.map((address) => {
            const slot = this._lookup.get(address.key());
            if (slot === undefined) {
                return -1;
            }
            this._pages[slot].lastReserved = this._reservation;
            return slot;
        });
        for (let i = 0; i < pages.length; ++i) {
            if (this._reservedSlots[i] >= 0) {
                continue;
            }
            const slot = this.allocate(pages[i]);
            this._pages[slot].lastReserved = this._reservation;
            this._reservedSlots[i] = slot;
        }
        return true;
    }

    public selectSources (page: VTPageAddress, refine: boolean): VTPageSources {
        const size = this._layout.pageSize(page.level);
        const result = new VTPageSources();
        result.splat = this._data.nodeAtClamped(this.sourceLevel(size), { x: (page.x + 0.5) * size, z: (page.z + 0.5) * size });
        const target = size * Math.max(0.5, (config.SOURCE_TILE_RESOLUTION - 1) / config.VT_PAGE_INTERIOR);
        const level = refine && page.level >= config.VT_NORMAL_DETAIL_MIN_LEVEL ? this.sourceLevel(target) : result.splat.level;
        for (let row = 0; row < 2; ++row) {
            for (let col = 0; col < 2; ++col) {
                result.normals[row * 2 + col] = this._data.nodeAtClamped(level, { x: (page.x + 0.25 + col * 0.5) * size, z: (page.z + 0.25 + row * 0.5) * size });
            }
        }
        return result;
    }

    public invalidate (changed?: LandscapeRegion): void {
        for (const page of this._pages) {
            if (page.state !== PageState.READY) {
                continue;
            }
            if (changed) {
                const bounds = this.region(page.address);
                const border = bounds.size * config.VT_PAGE_BORDER / config.VT_PAGE_INTERIOR;
                if (changed.x > bounds.x + bounds.size + border || changed.z > bounds.z + bounds.size + border
                    || changed.x + changed.size < bounds.x - border || changed.z + changed.size < bounds.z - border) {
                    continue;
                }
            }
            page.state = PageState.OUTDATED;
        }
    }

    public ready (): boolean {
        return this._reservedSlots.length > 0 && this._reservedSlots.every((slot) => this._pages[slot].state === PageState.READY);
    }

    public resolve (address: VTPageAddress): number {
        const slot = this._lookup.get(address.key());
        if (slot === undefined) {
            return -1;
        }
        const page = this._pages[slot];
        return page.lastReserved === this._reservation && page.drawable() ? slot : -1;
    }

    public setGlobalColorMap (texture: Texture2D | null): void {
        this._composer.setGlobalColorMap(texture);
        this.invalidate();
    }

    public setGlobalColorStrength (strength: number): void {
        if (this._composer.setGlobalColorStrength(strength)) {
            this.invalidate();
        }
        if (DEBUG && config.LANDSCAPE_DEBUG) {
            const pass = this._composer.material().passes[0];
            const params = pass.getUniform(pass.getHandle('globalColorParams'), new Vec4());
            const texture = pass.descriptorSet.getTexture(Pass.getBindingFromHandle(pass.getHandle('globalColorMap')));
            const stale = this._reservedSlots.filter((slot) => this._pages[slot].state === PageState.OUTDATED).length;
            log(`[Landscape] Global color strength: requested=${strength.toFixed(2)} effective=${params.z.toFixed(2)} map=${texture ? texture.width : 0}x${texture ? texture.height : 0} stale=${stale}/${this._reservedSlots.length}`);
        }
    }

    public setHeightBlendEnabled (enabled: boolean): void {
        if (this._composer.setHeightBlendEnabled(enabled)) {
            this.invalidate();
        }
    }

    public setTriplanarEnabled (enabled: boolean): void {
        if (this._composer.setTriplanarEnabled(enabled)) {
            this.invalidate();
        }
    }

    public setRefineFarDetail (enabled: boolean): void {
        if (enabled === this._refineFarDetail) {
            return;
        }
        this._refineFarDetail = enabled;
        for (const page of this._pages) {
            if (page.address.level >= config.VT_NORMAL_DETAIL_MIN_LEVEL && page.state === PageState.READY) {
                page.state = PageState.OUTDATED;
            }
        }
    }

    private sourceLevel (size: number): number {
        let level = this._data.minTileLevel;
        while (level < this._data.maxLevel && this._data.nodeSize(level) < size) {
            ++level;
        }
        return level;
    }

    private allocate (address: VTPageAddress): number {
        let slot = -1;
        let oldest = Number.MAX_VALUE;
        for (let i = 0; i < this._pages.length; ++i) {
            const page = this._pages[i];
            if (page.state === PageState.EMPTY) {
                slot = i;
                break;
            }
            if (page.lastReserved !== this._reservation && page.lastReserved < oldest) {
                oldest = page.lastReserved;
                slot = i;
            }
        }
        if (slot < 0) {
            throw new Error('Landscape VT reservation exhausted');
        }
        const previous = this._pages[slot];
        if (previous.state !== PageState.EMPTY) {
            this._lookup.delete(previous.address.key());
        }
        const page = new VTPage();
        page.address = address;
        page.state = PageState.UNCOMPOSED;
        this._pages[slot] = page;
        this._lookup.set(address.key(), slot);
        return slot;
    }
}

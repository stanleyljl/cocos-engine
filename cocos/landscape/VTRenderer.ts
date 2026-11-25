// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { error, log } from '../core/platform/debug';
import { Address, BufferTextureCopy, Filter, Format, Sampler, SamplerInfo, Texture, TextureInfo, TextureType, TextureUsageBit } from '../gfx';
import { director } from '../game/director';
import { Texture2D } from '../asset/assets';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { MaterialLibrary } from './MaterialLibrary';
import { DecalRenderer } from './DecalRenderer';
import { VTPageComposer, VTPageInstance, VTPageMipGenerator } from './VTPageStages';
import { LandscapeData, LandscapeRegion, NodeAddress, NodeKey, ReadonlyVTGridMapping, VTGridMapping, VTPageAddress, VTPageLayout, VTPageSources } from './Data';
import * as config from './Config';

export interface ResourceRequirements {
    // Keyed by address because JS sets compare address objects by identity.
    sources: Map<NodeKey, NodeAddress>;
    pages: Map<NodeKey, VTPageAddress>;
}

// Scoped to one admission scan; caches immutable inputs, never capacity decisions.
export class VTCapacityQuery {
    private readonly _pageSources = new Map<NodeKey, NodeAddress[]>();

    constructor (private readonly _renderer: VTRenderer, private readonly _residency: LandscapeResidency) {}

    // Candidate lists are unique; retained resources have already passed address validation.
    public canPrepare (pages: readonly VTPageAddress[], sources: readonly NodeAddress[], retained: ResourceRequirements = { sources: new Map(), pages: new Map() }): boolean {
        let sharedSources = 0;
        for (const source of sources) {
            if (retained.sources.has(source.key())) {
                ++sharedSources;
            }
        }
        let sharedPages = 0;
        for (const page of pages) {
            if (retained.pages.has(page.key())) {
                ++sharedPages;
            }
        }
        const retainedSourceCount = retained.sources.size - sharedSources;
        const retainedPageCount = retained.pages.size - sharedPages;
        if (!this._residency.canFit(sources, retainedSourceCount) || !this._renderer.canFit(pages, retainedPageCount)) {
            return false;
        }
        const sourceCount = retainedSourceCount + sources.length;
        const containsSource = (source: NodeAddress): boolean => retained.sources.has(source.key())
            || sources.some((candidate) => candidate.key() === source.key());
        const canCompose = (page: VTPageAddress): boolean => {
            let inputs = this._pageSources.get(page.key());
            if (!inputs) {
                inputs = [];
                this._renderer.selectSources(page, true, true).appendTo(inputs);
                this._pageSources.set(page.key(), inputs);
            }
            let additional = 0;
            for (const source of inputs) {
                if (!containsSource(source)) {
                    ++additional;
                }
            }
            return sourceCount + additional <= this._residency.layerCount();
        };
        // New sources can also exhaust composition headroom for retained pages.
        for (const page of retained.pages.values()) {
            if (!canCompose(page)) {
                return false;
            }
        }
        for (const page of pages) {
            if (!retained.pages.has(page.key()) && !canCompose(page)) {
                return false;
            }
        }
        return true;
    }
}

export class VTPageTable {
    private _texture: Texture | null = null;
    private _entries = new Uint8Array();
    private readonly _freeSlots: number[] = [];
    private _size = 0;
    private _count = 0;

    public destroy (): void {
        this._texture?.destroy();
        this._texture = null;
        this._entries = new Uint8Array();
        this._freeSlots.length = 0;
        this._size = 0;
        this._count = 0;
    }

    public canFit (count: number): boolean {
        return this.requiredSize(count) !== 0;
    }

    public ensureCapacity (count: number): boolean {
        const size = this.requiredSize(count);
        if (!size) {
            return false;
        }
        if (size === this._size) {
            return true;
        }
        const device = director.root!.device;
        const texture = device.createTexture(new TextureInfo(TextureType.TEX2D, TextureUsageBit.SAMPLED | TextureUsageBit.TRANSFER_DST, Format.RG8, size, size));
        if (!texture) {
            return false;
        }
        const entries = new Uint8Array(size * size * 2);
        entries.set(this._entries);
        const copy = new BufferTextureCopy();
        copy.texExtent.width = size;
        copy.texExtent.height = size;
        device.copyBuffersToTexture([entries], texture, [copy]);
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

    public update (slot: number, mapping: ReadonlyVTGridMapping, pages: VTRenderer): boolean {
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

    public upload (slots: readonly number[]): void {
        if (!slots.length) {
            return;
        }
        const sorted = slots.slice().sort((a, b) => a - b);
        const copies: BufferTextureCopy[] = [];
        const buffers: Uint8Array[] = [];
        const cells = VTGridMapping.CELL_COUNT;
        for (let i = 0; i < sorted.length; ++i) {
            const first = sorted[i] * cells;
            let width = cells;
            while (i + 1 < sorted.length && sorted[i + 1] === sorted[i] + 1
                && Math.floor(sorted[i + 1] * cells / this._size) === Math.floor(first / this._size)) {
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
        director.root!.device.copyBuffersToTexture(buffers, this.texture(), copies);
    }

    public texture (): Texture {
        return this._texture!;
    }

    private requiredSize (count: number): number {
        let size = Math.max(256, this._size);
        while (size * size < count * VTGridMapping.CELL_COUNT) {
            size *= 2;
        }
        return size <= director.root!.device.capabilities.maxTextureSize ? size : 0;
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

interface Request {
    address: VTPageAddress;
    count: number;
}

export class VTRenderer {
    private _data!: LandscapeData;
    private _layout!: VTPageLayout;
    private _asset!: LandscapeAsset; // Borrowed; outlives this renderer.

    private _residency!: LandscapeResidency;
    private readonly _requests = new Map<NodeKey, Request>();
    private _requestsDirty = false;
    // Owns one reference per source until the next update reconciles composition demand.
    private _sourceRequests: NodeAddress[] = [];
    private _reservation = 0;
    private _pages: VTPage[] = [];
    private _reservedSlots: number[] = [];
    private readonly _lookup = new Map<NodeKey, number>();

    private readonly _pageTable = new VTPageTable();
    private readonly _composer = new VTPageComposer();
    private readonly _mipGenerator = new VTPageMipGenerator();
    private _sampler!: Sampler;
    private _needsClear = true;

    private _refineFarDetail = true;
    private _triplanarEnabled = true;

    public init (asset: LandscapeAsset, residency: LandscapeResidency, materials: MaterialLibrary, decals: DecalRenderer, atlasSize: number): boolean {
        const capacity = (atlasSize / config.VT_PAGE_RES) ** 2;
        const data = asset.data();
        if (!residency.valid() || !materials.valid() || capacity < data.sectorsX * data.sectorsZ) {
            return false;
        }
        this._residency = residency;
        this._data = data;
        this._asset = asset;
        this._layout = new VTPageLayout(data.sectorSize);
        this._pages = Array.from({ length: capacity }, () => new VTPage());
        if (!this._pageTable.ensureCapacity(1) || !this._composer.init(asset, residency, materials, decals, atlasSize) || !this._mipGenerator.init(this._composer)) {
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
        this._sampler = director.root!.device.getSampler(sampler);
        if (!this.validateRootCapacity()) {
            error('[Landscape] Device capacity cannot compose sector roots');
            this.destroy();
            return false;
        }
        if (config.LANDSCAPE_DEBUG) {
            log(`[Landscape] VT atlas: ${atlasSize}x${atlasSize} RGBA8 pages=${this._pages.length}`);
        }
        return true;
    }

    private validateRootCapacity (): boolean {
        const rootPages: VTPageAddress[] = [];
        const rootSources: NodeAddress[] = [];
        for (let z = 0; z < this._data.sectorsZ; ++z) {
            for (let x = 0; x < this._data.sectorsX; ++x) {
                rootPages.push(new VTPageAddress(this._layout.rootLevel(), x, z));
                rootSources.push(new NodeAddress(this._data.maxTileLevel, x, z));
            }
        }
        return this.capacityQuery().canPrepare(rootPages, rootSources);
    }

    public destroy (): void {
        for (const source of this._sourceRequests) {
            this._residency.release(source);
        }
        this._sourceRequests = [];
        this._requests.clear();
        this._requestsDirty = false;
        this._mipGenerator.destroy();
        this._composer.destroy();
        this._pageTable.destroy();
        this._pages = [];
        this._reservedSlots = [];
        this._lookup.clear();
    }

    // Page residency and composition.
    public capacityQuery (): VTCapacityQuery {
        return new VTCapacityQuery(this, this._residency);
    }

    // retainedCount excludes resources present in the supplied list.
    public canFit (pages: readonly VTPageAddress[], retainedCount = 0): boolean {
        return pages.length + retainedCount <= this._pages.length && pages.every((page) => {
            const side = 2 ** (this._layout.rootLevel() - page.level);
            return page.level >= 0 && page.level <= this._layout.rootLevel() && page.x >= 0 && page.z >= 0
                && page.x < this._data.sectorsX * side && page.z < this._data.sectorsZ * side;
        });
    }

    public request (address: VTPageAddress): void {
        const entry = this._requests.get(address.key());
        if (entry) {
            ++entry.count;
        } else {
            this._requests.set(address.key(), { address, count: 1 });
            this._requestsDirty = true;
        }
    }

    public release (address: VTPageAddress): void {
        const entry = this._requests.get(address.key());
        if (!entry) {
            throw new Error('Landscape VT demand must be acquired before release');
        }
        if (--entry.count === 0) {
            this._requests.delete(address.key());
            this._requestsDirty = true;
        }
    }

    public update (budget: number): boolean {
        if (!this.applyRequests()) {
            return false;
        }
        const pages: VTPageAddress[] = [];
        // Reconcile before Residency.update(), so retained inputs never lose their slots.
        for (const source of this._sourceRequests) {
            this._residency.release(source);
        }
        const sources = new Map<NodeKey, NodeAddress>();
        for (const slot of this._reservedSlots) {
            const page = this._pages[slot].address;
            if (!this.needsComposition(page)) {
                continue;
            }
            const inputs: NodeAddress[] = [];
            this.selectSources(page, this._refineFarDetail, this._triplanarEnabled).appendTo(inputs);
            if (!this._residency.canRequest(inputs)) {
                continue;
            }
            pages.push(page);
            for (const source of inputs) {
                if (!sources.has(source.key())) {
                    sources.set(source.key(), source);
                    this._residency.request(source);
                }
            }
        }
        this._sourceRequests = Array.from(sources.values());
        // Keep this batch pinned through Residency.update() and GPU submission.
        this.renderPages(this._residency, pages, budget);
        return true;
    }

    private applyRequests (): boolean {
        if (!this._requestsDirty) {
            return true;
        }
        const pages = Array.from(this._requests.values(), (entry) => entry.address);
        pages.sort((a, b) => a.level - b.level || a.x - b.x || a.z - b.z);
        if (!this.canFit(pages)) {
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
        this._requestsDirty = false;
        return true;
    }

    private renderPages (residency: LandscapeResidency, pages: readonly VTPageAddress[], maxPages: number): number {
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
            const instance = this._composer.prepareInputs(slot, this.region(address), this.selectSources(address, this._refineFarDetail, this._triplanarEnabled), residency);
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
        const commands = director.root!.device.commandBuffer;
        for (let variant = 0; variant < 2; ++variant) {
            const group = batch.filter((entry) => entry.pageInfo.z === variant);
            if (!group.length) {
                continue;
            }
            this._composer.record(commands, group, this._needsClear);
            this._mipGenerator.record(commands, this._composer, group, this._needsClear);
            this._needsClear = false;
        }
        for (const instance of batch) {
            this._pages[instance.pageInfo.x].state = PageState.READY;
        }
        return batch.length;
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
        return !this._requestsDirty && this._reservedSlots.length > 0 && this._reservedSlots.every((slot) => this._pages[slot].state === PageState.READY);
    }

    public needsComposition (address: VTPageAddress): boolean {
        const slot = this._lookup.get(address.key());
        return slot === undefined || this._pages[slot].state !== PageState.READY;
    }

    public resolve (address: VTPageAddress): number {
        const slot = this._lookup.get(address.key());
        if (slot === undefined) {
            return -1;
        }
        const page = this._pages[slot];
        return page.lastReserved === this._reservation && page.drawable() ? slot : -1;
    }

    public reservedSlots (): readonly number[] {
        return this._reservedSlots;
    }

    public page (slot: number): Readonly<VTPage> {
        return this._pages[slot];
    }

    public region (address: VTPageAddress): LandscapeRegion {
        return this._layout.pageRegion(address);
    }

    public selectSources (page: VTPageAddress, refine: boolean, triplanar: boolean): VTPageSources {
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
        result.triplanar = triplanar && this.hasCliff(this.region(page));
        return result;
    }

    // Composition settings.
    public setGlobalColorMap (texture: Texture2D | null): void {
        this._composer.setGlobalColorMap(texture);
        this.invalidate();
    }

    public setGlobalColorStrength (strength: number): void {
        if (this._composer.setGlobalColorStrength(strength)) {
            this.invalidate();
        }
    }

    public setHeightBlendEnabled (enabled: boolean): void {
        if (this._composer.setHeightBlendEnabled(enabled)) {
            this.invalidate();
        }
    }

    public setTriplanarEnabled (enabled: boolean): void {
        if (enabled === this._triplanarEnabled) {
            return;
        }
        this._triplanarEnabled = enabled;
        this.invalidate();
    }

    public getTriplanarEnabled (): boolean {
        return this._triplanarEnabled;
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

    public getRefineFarDetail (): boolean {
        return this._refineFarDetail;
    }

    // GPU resources.
    public pageTable (): VTPageTable {
        return this._pageTable;
    }

    public atlas (): Texture2D {
        return this._composer.target();
    }

    public albedo (): Texture {
        return this._composer.albedo();
    }

    public normalRoughness (): Texture {
        return this._composer.normal();
    }

    public sampler (): Sampler {
        return this._sampler;
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

    private sourceLevel (size: number): number {
        let level = this._data.minTileLevel;
        while (level < this._data.maxTileLevel && this._data.nodeSize(level) < size) {
            ++level;
        }
        return level;
    }

    private hasCliff (region: LandscapeRegion): boolean {
        // Estimate slope from each region's height span and size, including gutters.
        // This coarse classification can miss cliffs narrower than the region.
        const border = region.size * config.VT_PAGE_BORDER / config.VT_PAGE_INTERIOR;
        let level = 0;
        while (level < this._data.maxTileLevel && this._data.nodeSize(level) < region.size + 2 * border) {
            ++level;
        }
        const first = this._data.nodeAtClamped(level, { x: region.x - border, z: region.z - border });
        const last = this._data.nodeAtClamped(level, { x: region.x + region.size + border, z: region.z + region.size + border });
        const heightThreshold = this._data.nodeSize(level) * Math.sqrt(1 / (config.TRIPLANAR_START_NORMAL_Y * config.TRIPLANAR_START_NORMAL_Y) - 1);
        const ranges = this._asset.heightRanges()[level];
        for (let z = first.z; z <= last.z; ++z) {
            for (let x = first.x; x <= last.x; ++x) {
                const range = ranges[z * this._data.nodesX(level) + x];
                if (range.maxY - range.minY > heightThreshold) {
                    return true;
                }
            }
        }
        return false;
    }
}

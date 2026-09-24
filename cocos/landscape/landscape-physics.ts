// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { isValid } from '../core';
import { Node } from '../scene-graph/node';
import { director, DirectorEvent } from '../game/director';
import { TerrainCollider } from '../physics/framework/components/colliders/terrain-collider';
import { PhysicsSystem } from '../physics/framework/physics-system';
import { PhysicsMaterial } from '../physics/framework/assets/physics-material';
import { selector } from '../physics/framework/physics-selector';
import { LandscapePhysicsCollider, LandscapePhysicsHeightfield } from './landscape-physics-collider';
import type { Landscape } from './landscape';
import { LandscapeHeightLayout, LandscapeHeightLoader } from './landscape-height-data';

export interface LandscapePhysicsBounds { minX: number; minZ: number; maxX: number; maxZ: number; }

export enum LandscapePhysicsStatus {
    NotReady,
    Ready,
    Outside,
    OutOfCapacity,
    Error,
    Removed,
    Disabled,
}

export interface LandscapePhysicsOptions {
    /** Maximum unique source tiles across all regions. Default 64. */
    maxTiles: number;
    /** Maximum outstanding height loads, including obsolete requests. Default 1. */
    maxConcurrentLoads: number;
    /** Maximum new backend heightfields per update. Default 1. */
    maxCreationsPerStep: number;
    group: number;
    mask: number;
    material: PhysicsMaterial | null;
}

interface Region {
    bounds: LandscapePhysicsBounds;
    actual?: LandscapePhysicsBounds;
    keys: number[];
    status: LandscapePhysicsStatus;
}
interface Tile {
    x: number;
    z: number;
    loading: boolean;
    samples?: Uint16Array;
    node?: Node;
    error?: string;
}
const DEFAULT_OPTIONS: LandscapePhysicsOptions = {
    maxTiles: 64, maxConcurrentLoads: 1, maxCreationsPerStep: 1, group: 1, mask: -1, material: null,
};
/** CPU-only, fixed-resolution collision residency owned by a Landscape.
 * Rectangles are in world XZ, clipped to terrain and rounded outward to whole
 * source tiles. Y is obtained from the terrain. No camera or render-LOD input.
 * Region mutations take effect at a physics safe point. Birth/teleport logic
 * must wait for Ready; this manager never pauses or moves gameplay objects.
 */
export class LandscapePhysics {
    private _options = { ...DEFAULT_OPTIONS };
    private readonly _regions = new Map<number, Region>();
    private readonly _tiles = new Map<number, Tile>();
    private _nextRegion = 1;
    private _layout?: LandscapeHeightLayout;
    private _layoutPending = false;
    private _error = '';
    private _url = '';
    private _generation = 0;
    private _inFlight = 0;
    private _dirty = true;
    private _enabled = false;
    private _destroyed = false;
    private _listening = false;
    private _inStep = false;
    private _originX = NaN;
    private _originZ = NaN;
    private _transformValid = true;
    private _resetTiles = false;

    /** @engineInternal Construct through Landscape.physics. Loader injection is for tests. */
    constructor (private readonly _landscape: Landscape, private readonly _loader = new LandscapeHeightLoader()) {}

    public get options (): Readonly<LandscapePhysicsOptions> { return { ...this._options }; }

    /** Set budgets/filter/material before registering regions. Source-byte stats
     * exclude backend allocations; each backend has its own height representation. */
    public configure (options: Partial<LandscapePhysicsOptions>): void {
        if (this._regions.size || this._tiles.size) throw new Error('Configure physics before adding regions');
        const next = { ...this._options, ...options };
        for (const name of ['maxTiles', 'maxConcurrentLoads', 'maxCreationsPerStep'] as const) {
            if (!Number.isInteger(next[name]) || next[name] < 1 || next[name] > (name === 'maxTiles' ? 4096 : 64)) {
                throw new RangeError(`Invalid Landscape physics ${name}`);
            }
        }
        if (!Number.isInteger(next.group) || !Number.isInteger(next.mask)) throw new RangeError('Invalid collision filter');
        this._options = next;
    }

    public addRegion (bounds: Readonly<LandscapePhysicsBounds>): number {
        if (this._destroyed) throw new Error('Landscape physics has been destroyed');
        this._validateBounds(bounds);
        const id = this._nextRegion++;
        this._regions.set(id, { bounds: { ...bounds }, keys: [], status: LandscapePhysicsStatus.NotReady });
        this._changed();
        return id;
    }

    public setRegionBounds (id: number, bounds: Readonly<LandscapePhysicsBounds>): boolean {
        this._validateBounds(bounds);
        const region = this._regions.get(id);
        if (!region) return false;
        if (bounds.minX === region.bounds.minX && bounds.minZ === region.bounds.minZ
            && bounds.maxX === region.bounds.maxX && bounds.maxZ === region.bounds.maxZ) return true;
        region.bounds = { ...bounds };
        this._changed();
        return true;
    }

    public removeRegion (id: number): void { this._regions.delete(id); this._changed(); }
    public clearRegions (): void { this._regions.clear(); this._changed(); }

    public getRegionStatus (id: number): LandscapePhysicsStatus {
        const region = this._regions.get(id);
        if (!region) return LandscapePhysicsStatus.Removed;
        if (!this._enabled || this._destroyed) return LandscapePhysicsStatus.Disabled;
        if (this._error || !this._transformValid) return LandscapePhysicsStatus.Error;
        if (this._dirty || !this._layout) return LandscapePhysicsStatus.NotReady;
        if (region.status !== LandscapePhysicsStatus.NotReady) return region.status;
        for (const key of region.keys) if (this._tiles.get(key)?.error) return LandscapePhysicsStatus.Error;
        return region.keys.every(key => !!this._tiles.get(key)?.node)
            ? LandscapePhysicsStatus.Ready : LandscapePhysicsStatus.NotReady;
    }

    public isRegionReady (id: number): boolean { return this.getRegionStatus(id) === LandscapePhysicsStatus.Ready; }

    /** Check already submitted collision covering a world rectangle. Does not
     * request or pin data. A small movement guard can remain ready while the
     * owner's larger region is loading distant tiles. Outside terrain is false. */
    public isAreaReady (bounds: Readonly<LandscapePhysicsBounds>): boolean {
        this._validateBounds(bounds);
        const layout = this._layout;
        if (!this._enabled || this._destroyed || !layout || this._resetTiles || this._error || !this._transformValid) return false;
        const origin = this._landscape.node.worldPosition;
        if (origin.x !== this._originX || origin.z !== this._originZ) return false;
        const startX = this._originX - layout.tilesX * layout.tileSize / 2;
        const startZ = this._originZ - layout.tilesZ * layout.tileSize / 2;
        const x0 = Math.floor((bounds.minX - startX) / layout.tileSize);
        const z0 = Math.floor((bounds.minZ - startZ) / layout.tileSize);
        const x1 = Math.ceil((bounds.maxX - startX) / layout.tileSize);
        const z1 = Math.ceil((bounds.maxZ - startZ) / layout.tileSize);
        if (x0 < 0 || z0 < 0 || x1 > layout.tilesX || z1 > layout.tilesZ) return false;
        for (let z = z0; z < z1; ++z) for (let x = x0; x < x1; ++x) {
            const tile = this._tiles.get(z * layout.tilesX + x);
            if (!tile?.node || tile.error || !isValid(tile.node) || !tile.node.activeInHierarchy) return false;
        }
        return true;
    }

    /** Actual tile-aligned coverage. Output is unchanged when unavailable. */
    public getRegionBounds (id: number, output: LandscapePhysicsBounds): boolean {
        const bounds = this._regions.get(id)?.actual;
        if (!bounds || this._dirty) return false;
        Object.assign(output, bounds);
        return true;
    }

    public getRegionError (id: number): string {
        if (!this._transformValid) return 'Landscape physics supports translation only';
        if (this._error) return this._error;
        const region = this._regions.get(id);
        if (region?.status === LandscapePhysicsStatus.OutOfCapacity) return 'Physics tile capacity exceeded';
        for (const key of region?.keys || []) { const error = this._tiles.get(key)?.error; if (error) return error; }
        return '';
    }

    public getStats (): { residentTiles: number; sourceBytes: number; inFlight: number; queuedTiles: number } {
        let residentTiles = 0; let sourceBytes = 0; let queuedTiles = 0;
        for (const tile of this._tiles.values()) {
            if (tile.node) ++residentTiles;
            if (tile.samples) sourceBytes += tile.samples.byteLength;
            if (!tile.node && !tile.error) ++queuedTiles;
        }
        return { residentTiles, sourceBytes, inFlight: this._inFlight, queuedTiles };
    }

    /** Visit the live colliders submitted to the backend, for optional debug
     * visualization. No geometry copies or GPU resources are created here.
     * Do not change regions or destroy nodes inside the visitor. */
    public forEachCollider (visitor: (collider: TerrainCollider) => void): void {
        for (const tile of this._tiles.values()) {
            if (tile.node && isValid(tile.node) && tile.node.activeInHierarchy) {
                const collider = tile.node.getComponent(TerrainCollider);
                if (collider) visitor(collider);
            }
        }
    }

    /** Automatic simulation calls this before stepping. With manual simulation,
     * call it BEFORE syncSceneToPhysics/step, never from a contact callback. */
    public update (): void {
        if (this._inStep) return;
        if (!this._enabled || this._destroyed || !this._regions.size) {
            this._clearTiles();
            this._unlisten();
            return;
        }
        const node = this._landscape.node;
        const m = node.worldMatrix;
        this._transformValid = [m.m00 - 1, m.m01, m.m02, m.m04, m.m05 - 1, m.m06, m.m08, m.m09, m.m10 - 1]
            .every(v => Number.isFinite(v) && Math.abs(v) < 1e-5)
            && [m.m12, m.m13, m.m14].every(Number.isFinite);
        if (!this._transformValid) { this._clearTiles(); return; }
        if (m.m12 !== this._originX || m.m14 !== this._originZ) {
            this._originX = m.m12; this._originZ = m.m14; this._dirty = true;
        }
        this._ensureLayout();
        if (this._resetTiles) { this._clearTiles(); this._resetTiles = false; }
        if (!this._layout || this._error) return;
        if (!PhysicsSystem.instance?.physicsWorld || !selector.wrapper.TerrainShape) {
            this._error = `Terrain collision is unavailable in physics backend ${selector.id}`;
            return;
        }
        if (this._dirty) this._reconcile();
        let remaining = this._options.maxCreationsPerStep;
        for (const tile of this._tiles.values()) {
            if (tile.node || tile.error) continue;
            if (tile.samples && remaining > 0) {
                --remaining;
                try { tile.node = this._createTile(tile); } catch (error) { tile.error = String(error); }
            } else if (!tile.samples && !tile.loading && this._inFlight < this._options.maxConcurrentLoads) {
                this._loadTile(tile);
            }
        }
    }

    /** @engineInternal */
    public _setEnabled (enabled: boolean): void {
        this._enabled = enabled && !EDITOR_NOT_IN_PREVIEW;
        if (this._enabled) this._error = '';
        if (!this._enabled) {
            ++this._generation; this._layoutPending = false;
            if (!this._inStep) { this._clearTiles(); this._unlisten(); }
        }
        this._changed();
    }

    /** @engineInternal */
    public _destroy (): void { this._destroyed = true; this._regions.clear(); this._setEnabled(false); }

    private _validateBounds (b: Readonly<LandscapePhysicsBounds>): void {
        if (![b.minX, b.minZ, b.maxX, b.maxZ].every(Number.isFinite) || b.minX >= b.maxX || b.minZ >= b.maxZ) {
            throw new RangeError('Physics region must be a finite nonempty world XZ rectangle');
        }
    }

    private _changed (): void {
        this._dirty = true;
        if (this._enabled && this._regions.size && !this._destroyed) {
            if (!this._listening) {
                director.on(DirectorEvent.BEFORE_PHYSICS, this._beforePhysics, this);
                director.on(DirectorEvent.AFTER_PHYSICS, this._afterPhysics, this);
                this._listening = true;
            }
            this._ensureLayout();
        }
    }

    private _beforePhysics (): void { this.update(); this._inStep = this._listening; }
    private _afterPhysics (): void {
        this._inStep = false;
        if (!this._enabled || this._destroyed || !this._regions.size) { this._clearTiles(); this._unlisten(); }
        // Retire ranges changed from a collision callback before the next step.
        else {
            if (this._resetTiles) { this._clearTiles(); this._resetTiles = false; }
            if (this._dirty && this._layout && !this._error && this._transformValid) this._reconcile();
        }
    }
    private _unlisten (): void {
        director.off(DirectorEvent.BEFORE_PHYSICS, this._beforePhysics, this);
        director.off(DirectorEvent.AFTER_PHYSICS, this._afterPhysics, this);
        this._listening = false;
    }

    private _ensureLayout (): void {
        const url = this._landscape.landscapeAsset?.manifestPath || '';
        if (url !== this._url) {
            ++this._generation; this._url = url; this._layout = undefined;
            this._layoutPending = false; this._error = ''; this._dirty = true; this._resetTiles = true;
        }
        if (!url || this._layout || this._layoutPending || this._error) return;
        const generation = this._generation;
        this._layoutPending = true;
        void this._loader.loadLayout(url).then(layout => {
            if (generation !== this._generation || this._destroyed) return;
            this._layout = layout; this._layoutPending = false; this._dirty = true;
        }, error => {
            if (generation !== this._generation || this._destroyed) return;
            this._layoutPending = false; this._error = String(error);
        });
    }

    private _reconcile (): void {
        const layout = this._layout!;
        const startX = this._originX - layout.tilesX * layout.tileSize / 2;
        const startZ = this._originZ - layout.tilesZ * layout.tileSize / 2;
        const desired = new Set<number>();
        for (const region of this._regions.values()) {
            const b = region.bounds;
            const x0 = Math.max(0, Math.floor((b.minX - startX) / layout.tileSize));
            const z0 = Math.max(0, Math.floor((b.minZ - startZ) / layout.tileSize));
            const x1 = Math.min(layout.tilesX, Math.ceil((b.maxX - startX) / layout.tileSize));
            const z1 = Math.min(layout.tilesZ, Math.ceil((b.maxZ - startZ) / layout.tileSize));
            region.keys = []; region.actual = undefined;
            if (x0 >= x1 || z0 >= z1) { region.status = LandscapePhysicsStatus.Outside; continue; }
            if ((x1 - x0) * (z1 - z0) > this._options.maxTiles) {
                region.status = LandscapePhysicsStatus.OutOfCapacity; continue;
            }
            const keys: number[] = []; let added = 0;
            for (let z = z0; z < z1; ++z) for (let x = x0; x < x1; ++x) {
                const key = z * layout.tilesX + x; keys.push(key); if (!desired.has(key)) ++added;
            }
            if (desired.size + added > this._options.maxTiles) { region.status = LandscapePhysicsStatus.OutOfCapacity; continue; }
            region.keys = keys; region.status = LandscapePhysicsStatus.NotReady;
            region.actual = { minX: startX + x0 * layout.tileSize, minZ: startZ + z0 * layout.tileSize,
                maxX: startX + x1 * layout.tileSize, maxZ: startZ + z1 * layout.tileSize };
            for (const key of keys) desired.add(key);
        }
        for (const [key, tile] of this._tiles) if (!desired.has(key)) { this._disposeTile(tile); this._tiles.delete(key); }
        // Prepare the center (typically the actor) before distant prefetch edges.
        const centers = [...this._regions.values()].filter(region => region.keys.length).map(region => ({
            x: (region.bounds.minX + region.bounds.maxX) / 2,
            z: (region.bounds.minZ + region.bounds.maxZ) / 2,
        }));
        const pending = [...desired].filter(key => !this._tiles.has(key)).map(key => {
            const x = startX + (key % layout.tilesX + 0.5) * layout.tileSize;
            const z = startZ + (Math.floor(key / layout.tilesX) + 0.5) * layout.tileSize;
            let distance = Infinity;
            for (const center of centers) distance = Math.min(distance, (x - center.x) ** 2 + (z - center.z) ** 2);
            return { key, distance };
        }).sort((a, b) => a.distance - b.distance);
        for (const { key } of pending) {
            this._tiles.set(key, { x: key % layout.tilesX, z: Math.floor(key / layout.tilesX), loading: false });
        }
        this._dirty = false;
    }

    private _loadTile (tile: Tile): void {
        const generation = this._generation; const layout = this._layout!;
        const key = tile.z * layout.tilesX + tile.x;
        tile.loading = true; ++this._inFlight;
        void this._loader.loadTile(this._url, layout, tile.x, tile.z).then(samples => {
            if (generation !== this._generation || this._tiles.get(key) !== tile) return;
            if (samples.length !== layout.resolution ** 2) throw new Error('Invalid height sample count');
            tile.samples = samples;
        }).catch(error => {
            if (generation === this._generation && this._tiles.get(key) === tile) tile.error = String(error);
        }).then(() => { tile.loading = false; --this._inFlight; });
    }

    private _createTile (tile: Tile): Node {
        const layout = this._layout!;
        const node = new Node(`LandscapeCollision(${tile.x},${tile.z})`);
        node.active = false;
        try {
            node.parent = this._landscape.node;
            node.setPosition((tile.x - layout.tilesX / 2) * layout.tileSize,
                layout.heightBias + 32768 * layout.heightScale / 65535,
                (tile.z - layout.tilesZ / 2) * layout.tileSize);
            const collider = node.addComponent(LandscapePhysicsCollider);
            collider.terrain = new LandscapePhysicsHeightfield(tile.samples!, layout);
            collider.sharedMaterial = this._options.material;
            node.active = true;
            if (!collider.shape?.impl) throw new Error('Terrain collider creation failed');
            collider.setGroup(this._options.group); collider.setMask(this._options.mask);
            return node;
        } catch (error) { node._destroyImmediate(); throw error; }
    }

    private _disposeTile (tile: Tile): void {
        if (tile.node && isValid(tile.node)) {
            tile.node.active = false;
            // Safe-point destruction releases shape-owned buffers now, rather
            // than retaining entire old regions until JS/native GC runs.
            tile.node._destroyImmediate();
        }
        tile.node = undefined; tile.samples = undefined;
    }
    private _clearTiles (): void {
        for (const tile of this._tiles.values()) this._disposeTile(tile);
        this._tiles.clear(); this._dirty = true;
    }
}

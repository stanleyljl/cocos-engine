// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Vec3 } from '../core/math';
import { LandscapeData, LandscapePoint, NodeAddress, NodeKey, makeNodeKey } from './Data';
import type { LandscapeQueryTile, LandscapeQueryCompletion } from './Data';
import * as config from './Config';

export enum LandscapeQueryStatus {
    HIT,
    MISS,
    NOT_READY,
    ERROR,
    OUT_OF_CAPACITY,
}

export class LandscapeSurfaceResult {
    public status = LandscapeQueryStatus.NOT_READY;
    public position = new Vec3();
    public normal = new Vec3();
    public surfaceType = -1;
    public surfaceWeight = 0;
}

class TileRegion {
    public minX: number;
    public minZ: number;
    public maxX: number;
    public maxZ: number;

    constructor (minX = 0, minZ = 0, maxX = 0, maxZ = 0) {
        this.minX = minX;
        this.minZ = minZ;
        this.maxX = maxX;
        this.maxZ = maxZ;
    }

    public tileCount (): number {
        return (this.maxX - this.minX + 1) * (this.maxZ - this.minZ + 1);
    }

    public contains (tile: NodeAddress): boolean {
        return tile.x >= this.minX && tile.x <= this.maxX && tile.z >= this.minZ && tile.z <= this.maxZ;
    }

    public equals (other: TileRegion): boolean {
        return this.minX === other.minX && this.minZ === other.minZ && this.maxX === other.maxX && this.maxZ === other.maxZ;
    }

    public forEachTile (level: number, visit: (key: NodeKey) => void): void {
        for (let z = this.minZ; z <= this.maxZ; ++z) {
            for (let x = this.minX; x <= this.maxX; ++x) {
                visit(makeNodeKey(level, x, z));
            }
        }
    }
}

interface Tile {
    data: LandscapeQueryTile | null;
    references: number;
    status: LandscapeQueryStatus;
    loaded: boolean;
}

type Loader = (x: number, z: number, complete: LandscapeQueryCompletion) => void;

export class LandscapeQuery {
    private readonly _sources = new Map<number, TileRegion>();
    private readonly _tiles = new Map<NodeKey, Tile>();
    private readonly _tileSize: number;

    private readonly _data: LandscapeData;
    private readonly _loader: Loader;
    private readonly _capacity: number;

    // Landscape validates capacity in [1, 4096] before constructing the query.
    constructor (_data: LandscapeData, _loader: Loader, _capacity = 64) {
        this._data = _data;
        this._loader = _loader;
        this._capacity = _capacity;
        this._tileSize = _data.nodeSize(_data.minTileLevel);
    }

    public destroy (): void {
        this._sources.clear();
        this._tiles.clear();
    }

    public addSource (id: number, center: LandscapePoint, radius: number): LandscapeQueryStatus {
        const region = new TileRegion();
        const status = this.resolveRegion(center, radius, region);
        if (status !== LandscapeQueryStatus.HIT) {
            return status;
        }
        const previous = this._sources.get(id);
        if (previous && previous.equals(region)) {
            return this.getSourceStatus(id);
        }
        if (!this.canAdmit(region, previous)) {
            return LandscapeQueryStatus.OUT_OF_CAPACITY;
        }
        // Acquire first so a moving source retains its overlapping tiles.
        region.forEachTile(this._data.minTileLevel, (key) => {
            let entry = this._tiles.get(key);
            if (!entry) {
                entry = { data: null, references: 0, status: LandscapeQueryStatus.NOT_READY, loaded: false };
                this._tiles.set(key, entry);
                const captured = entry;
                const address = NodeAddress.fromKey(key);
                this._loader(address.x, address.z, (tile) => {
                    if (this._tiles.get(key) === captured) {
                        captured.data = tile;
                        captured.loaded = true;
                    }
                });
            }
            ++entry.references;
        });
        this.removeSource(id);
        this._sources.set(id, region);
        return this.getSourceStatus(id);
    }

    private resolveRegion (center: LandscapePoint, radius: number, region: TileRegion): LandscapeQueryStatus {
        const data = this._data;
        if (!data.valid() || radius < 0) {
            return LandscapeQueryStatus.ERROR;
        }
        if (center.x + radius < 0 || center.z + radius < 0 || center.x - radius > data.width() || center.z - radius > data.depth()) {
            return LandscapeQueryStatus.MISS;
        }
        const lo = data.nodeAtClamped(data.minTileLevel, { x: center.x - radius, z: center.z - radius });
        const hi = data.nodeAtClamped(data.minTileLevel, { x: center.x + radius, z: center.z + radius });
        region.minX = lo.x;
        region.minZ = lo.z;
        region.maxX = hi.x;
        region.maxZ = hi.z;
        return region.tileCount() <= this._capacity ? LandscapeQueryStatus.HIT : LandscapeQueryStatus.OUT_OF_CAPACITY;
    }

    private canAdmit (region: TileRegion, previous?: TileRegion): boolean {
        let required = region.tileCount();
        for (const [key, tile] of this._tiles) {
            const address = NodeAddress.fromKey(key);
            const released = previous && previous.contains(address) ? 1 : 0;
            if (tile.references > released && !region.contains(address)) {
                if (++required > this._capacity) {
                    return false;
                }
            }
        }
        return true;
    }

    public removeSource (id: number): void {
        const region = this._sources.get(id);
        if (!region) {
            return;
        }
        region.forEachTile(this._data.minTileLevel, (key) => {
            const tile = this._tiles.get(key)!;
            if (--tile.references === 0) {
                this._tiles.delete(key);
            }
        });
        this._sources.delete(id);
    }

    public getSourceStatus (id: number): LandscapeQueryStatus {
        const region = this._sources.get(id);
        if (!region) {
            return LandscapeQueryStatus.NOT_READY;
        }
        let ready = true;
        for (let z = region.minZ; z <= region.maxZ; ++z) {
            for (let x = region.minX; x <= region.maxX; ++x) {
                const status = this._tiles.get(makeNodeKey(this._data.minTileLevel, x, z))!.status;
                if (status === LandscapeQueryStatus.ERROR) {
                    return status;
                }
                ready = ready && status === LandscapeQueryStatus.HIT;
            }
        }
        return ready ? LandscapeQueryStatus.HIT : LandscapeQueryStatus.NOT_READY;
    }

    public update (): void {
        const samples = config.SOURCE_TILE_RESOLUTION ** 2;
        const normals = config.SOURCE_NORMAL_RESOLUTION ** 2;
        for (const tile of this._tiles.values()) {
            if (tile.status !== LandscapeQueryStatus.NOT_READY || !tile.loaded) {
                continue;
            }
            const data = tile.data;
            tile.status = data && data.height.length === samples * 2 && data.splat.length === samples && data.normal.length === normals * 3
                ? LandscapeQueryStatus.HIT : LandscapeQueryStatus.ERROR;
        }
    }

    public sample (local: LandscapePoint): LandscapeSurfaceResult {
        const result = new LandscapeSurfaceResult();
        if (!this._data.valid()) {
            result.status = LandscapeQueryStatus.ERROR;
            return result;
        }
        if (!this._data.contains(local)) {
            result.status = LandscapeQueryStatus.MISS;
            return result;
        }
        const address = this._data.nodeAtClamped(this._data.minTileLevel, local);
        const entry = this._tiles.get(address.key());
        if (!entry) {
            return result;
        }
        result.status = entry.status;
        if (result.status !== LandscapeQueryStatus.HIT) {
            return result;
        }
        const tile = entry.data!;
        const edge = config.SOURCE_TILE_RESOLUTION - 1;
        const gx = Math.min(edge, Math.max(0, (local.x / this._tileSize - address.x) * edge));
        const gz = Math.min(edge, Math.max(0, (local.z / this._tileSize - address.z) * edge));
        const ix = Math.min(Math.floor(gx), edge - 1);
        const iz = Math.min(Math.floor(gz), edge - 1);
        const a = iz * config.SOURCE_TILE_RESOLUTION + ix;
        if (tile.splat[a] & 0x8000) {
            result.status = LandscapeQueryStatus.MISS;
            return result;
        }
        const indices = [a, a + 1, a + config.SOURCE_TILE_RESOLUTION, a + config.SOURCE_TILE_RESOLUTION + 1];
        const n = (iz + config.SOURCE_NORMAL_BORDER) * config.SOURCE_NORMAL_RESOLUTION + ix + config.SOURCE_NORMAL_BORDER;
        const normalIndices = [n, n + 1, n + config.SOURCE_NORMAL_RESOLUTION, n + config.SOURCE_NORMAL_RESOLUTION + 1];
        const fx = gx - ix;
        const fz = gz - iz;
        const weights = [(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz];
        const triangle = fx + fz <= 1 ? [1 - fx - fz, fx, fz, 0] : [0, 1 - fz, 1 - fx, fx + fz - 1];
        let height = 0;
        let nx = 0;
        let ny = 0;
        let nz = 0;
        const material = new Float64Array(config.MATERIAL_LIBRARY_MAX);
        for (let i = 0; i < 4; ++i) {
            const h = indices[i] * 2;
            const normal = normalIndices[i] * 3;
            height += triangle[i] * (tile.height[h] * 256 + tile.height[h + 1]);
            nx += weights[i] * (tile.normal[normal] / 127.5 - 1);
            ny += weights[i] * (tile.normal[normal + 1] / 127.5 - 1);
            nz += weights[i] * (tile.normal[normal + 2] / 127.5 - 1);
            const encoded = tile.splat[indices[i]];
            const blend = ((encoded >> 10) & 31) / 31;
            material[encoded & 31] += triangle[i] * (1 - blend);
            material[(encoded >> 5) & 31] += triangle[i] * blend;
        }
        const length = Math.hypot(nx, ny, nz);
        if (!(length > 0)) {
            result.status = LandscapeQueryStatus.ERROR;
            return result;
        }
        result.position.set(local.x, this._data.heightBias + height * this._data.heightScale / 65535, local.z);
        result.normal.set(nx / length, ny / length, nz / length);
        result.surfaceType = 0;
        for (let i = 1; i < material.length; ++i) {
            if (material[i] > material[result.surfaceType]) {
                result.surfaceType = i;
            }
        }
        result.surfaceWeight = material[result.surfaceType];
        return result;
    }
}

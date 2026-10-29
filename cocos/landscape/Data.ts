// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import type { Vec3 } from '../core/math';
import * as config from './Config';

export interface LandscapePoint {
    x: number;
    z: number;
}

export interface LandscapeRegion extends LandscapePoint {
    size: number;
}

export interface HeightRange {
    minY: number;
    maxY: number;
}

// CPU query samples retain padded RGB normals. Splat is decoded uint16;
// native stores the same host-order values in a byte vector.
export interface LandscapeQueryTile {
    height: Uint8Array;
    splat: Uint16Array;
    normal: Uint8Array;
}

// Null reports load failure; generation checks discard cancelled completions.
export type LandscapeQueryCompletion = (tile: LandscapeQueryTile | null) => void;

export class LandscapeDebugData {
    public wireframe = false;
    public lodColor = false;
    public showRanges = false;
    public showBox = false;
    public unlit = false;
    public showVTAtlas = false;
    public heightBlendEnabled = true;
    public refineFarDetail = true;
    public decal3DEnabled = true;
    public triplanarEnabled = true;
    public physicsWireframe = false;
}

// Native keys occupy 64 bits. Decimal components preserve the entire address
// without JS bitwise truncation or relying on integers beyond Number's precision.
export type NodeKey = string;

export function makeNodeKey (level: number, x: number, z: number): NodeKey {
    return `${level}:${x}:${z}`;
}

export class NodeAddress {
    public level: number;
    public x: number;
    public z: number;

    constructor (level = 0, x = 0, z = 0) {
        this.level = level;
        this.x = x;
        this.z = z;
    }

    public key (): NodeKey {
        return makeNodeKey(this.level, this.x, this.z);
    }

    public static fromKey (key: NodeKey): NodeAddress {
        const [level, x, z] = key.split(':').map(Number);
        return new NodeAddress(level, x, z);
    }

    public ancestor (level: number): NodeAddress {
        const scale = 2 ** (level - this.level);
        return new NodeAddress(level, Math.floor(this.x / scale), Math.floor(this.z / scale));
    }
}

export class QuadNode {
    public level: number;
    public ix: number;
    public iz: number;
    public minY: number;
    public maxY: number;

    constructor (level = 0, ix = 0, iz = 0, minY = 0, maxY = 0) {
        this.level = level;
        this.ix = ix;
        this.iz = iz;
        this.minY = minY;
        this.maxY = maxY;
    }

    public address (): NodeAddress {
        return new NodeAddress(this.level, this.ix, this.iz);
    }
}

export class VTPageAddress {
    public level: number;
    public x: number;
    public z: number;

    constructor (level = 0, x = 0, z = 0) {
        this.level = level;
        this.x = x;
        this.z = z;
    }

    public key (): NodeKey {
        return makeNodeKey(this.level, this.x, this.z);
    }
}

export class VTGridMapping {
    public static readonly SIDE = 16;
    public static readonly CELL_COUNT = 256;
    public readonly cells = new Uint8Array(VTGridMapping.CELL_COUNT);
    public pages: VTPageAddress[] = [];
}

export class VTPageSources {
    public splat = new NodeAddress();
    public normals: NodeAddress[] = Array.from({ length: config.VT_NORMAL_SOURCE_COUNT }, () => new NodeAddress());

    public appendTo (output: NodeAddress[]): void {
        for (const source of [this.splat, ...this.normals]) {
            if (!output.some((entry) => entry.key() === source.key())) {
                output.push(source);
            }
        }
    }
}

export class LandscapeData {
    public sectorsX = 0;
    public sectorsZ = 0;
    public maxLevel = 0;
    public minTileLevel = 0;
    public sectorSize = 0;
    public heightScale = 0;
    public heightBias = 0;

    public width (): number {
        return this.sectorSize * this.sectorsX;
    }
    public depth (): number {
        return this.sectorSize * this.sectorsZ;
    }
    public minHeight (): number {
        return this.heightBias;
    }
    public maxHeight (): number {
        return this.heightBias + this.heightScale;
    }

    public valid (): boolean {
        return Number.isInteger(this.sectorsX) && Number.isInteger(this.sectorsZ)
            && this.sectorsX > 0 && this.sectorsZ > 0
            && Number.isInteger(this.maxLevel) && this.maxLevel >= 0 && this.maxLevel < config.MAX_LOD_LEVELS
            && this.sectorsX * this.sectorsZ <= config.MAX_SECTOR_COUNT
            && Number.isInteger(this.minTileLevel) && this.minTileLevel >= 0 && this.minTileLevel <= this.maxLevel
            && this.sectorSize > 0 && this.heightScale > 0;
    }

    public nodesPerSectorSide (level: number): number {
        return level >= 0 && level <= this.maxLevel && this.maxLevel < config.MAX_LOD_LEVELS ? 2 ** (this.maxLevel - level) : 0;
    }

    public nodeSize (level: number): number {
        const side = this.nodesPerSectorSide(level);
        return side !== 0 ? this.sectorSize / side : 0;
    }

    public nodesX (level: number): number {
        return this.sectorsX * this.nodesPerSectorSide(level);
    }
    public nodesZ (level: number): number {
        return this.sectorsZ * this.nodesPerSectorSide(level);
    }

    public contains (position: LandscapePoint): boolean {
        return position.x >= 0 && position.z >= 0 && position.x <= this.width() && position.z <= this.depth();
    }

    public nodeAtClamped (level: number, position: LandscapePoint): NodeAddress {
        const size = this.nodeSize(level);
        return new NodeAddress(level,
            Math.min(Math.max(Math.floor(position.x / size), 0), this.nodesX(level) - 1),
            Math.min(Math.max(Math.floor(position.z / size), 0), this.nodesZ(level) - 1));
    }

    public nodeRegion (address: NodeAddress): LandscapeRegion {
        const size = this.nodeSize(address.level);
        return { x: address.x * size, z: address.z * size, size };
    }
}

export class NodeIndexLayout {
    public static readonly INVALID_INDEX = -1;
    private readonly _levels: {
        nodesPerSide: number;
        offset: number;
    }[] = [];
    private _sectorsX = 0;
    private _sectorsZ = 0;
    private _nodesPerSector = 0;

    constructor (data?: LandscapeData) {
        if (!data || data.sectorsX <= 0 || data.sectorsZ <= 0 || data.maxLevel >= config.MAX_LOD_LEVELS
            || data.sectorsX * data.sectorsZ > config.MAX_SECTOR_COUNT) {
            return;
        }
        this._sectorsX = data.sectorsX;
        this._sectorsZ = data.sectorsZ;
        for (let level = 0; level <= data.maxLevel; ++level) {
            const side = data.nodesPerSectorSide(level);
            this._levels.push({ nodesPerSide: side, offset: this._nodesPerSector });
            this._nodesPerSector += side * side;
        }
    }

    public nodeCount (): number {
        return this._sectorsX * this._sectorsZ * this._nodesPerSector;
    }

    public sectorNodeIndex (sectorX: number, sectorZ: number, level: number, localX: number, localZ: number): number {
        if (level < 0 || level >= this._levels.length || sectorX < 0 || sectorZ < 0 || sectorX >= this._sectorsX || sectorZ >= this._sectorsZ) {
            return NodeIndexLayout.INVALID_INDEX;
        }
        const layout = this._levels[level];
        if (localX < 0 || localZ < 0 || localX >= layout.nodesPerSide || localZ >= layout.nodesPerSide) {
            return NodeIndexLayout.INVALID_INDEX;
        }
        return (sectorZ * this._sectorsX + sectorX) * this._nodesPerSector + layout.offset + localZ * layout.nodesPerSide + localX;
    }

    public globalNodeIndex (level: number, x: number, z: number): number {
        if (level < 0 || level >= this._levels.length) {
            return NodeIndexLayout.INVALID_INDEX;
        }
        const side = this._levels[level].nodesPerSide;
        return this.sectorNodeIndex(Math.floor(x / side), Math.floor(z / side), level, x % side, z % side);
    }
}

export class VTPageLayout {
    private readonly _rootLevel: number;

    private readonly _sectorSize: number;

    constructor (_sectorSize = 1) {
        this._sectorSize = _sectorSize;
        this._rootLevel = Math.min(27, Math.max(0, Math.floor(Math.log2(_sectorSize))));
    }

    public rootLevel (): number {
        return this._rootLevel;
    }

    public pageSize (level: number): number {
        return level <= this._rootLevel ? this._sectorSize / 2 ** (this._rootLevel - level) : 0;
    }

    public levelForDistance (distance: number): number {
        let level = this._rootLevel;
        let nextSize = this._sectorSize * 0.5;
        const target = distance * 0.5;
        while (level > 0 && nextSize >= target) {
            --level;
            nextSize *= 0.5;
        }
        return level;
    }

    public pageRegion (page: VTPageAddress): LandscapeRegion {
        const size = this.pageSize(page.level);
        return { x: page.x * size, z: page.z * size, size };
    }

    public buildGridMapping (region: LandscapeRegion, minY: number, maxY: number, camera: Readonly<Vec3>): VTGridMapping {
        const result = new VTGridMapping();
        const heightDistance = Math.max(minY - camera.y, camera.y - maxY, 0);
        const firstCell = { x: region.x, z: region.z, size: region.size / VTGridMapping.SIDE };
        const half = VTGridMapping.SIDE / 2;
        for (let z = 0; z < VTGridMapping.SIDE; z += half) {
            for (let x = 0; x < VTGridMapping.SIDE; x += half) {
                this.mapCells(firstCell, camera, heightDistance, x, z, half, result);
            }
        }
        return result;
    }

    public coveringPage (desiredLevel: number, min: LandscapePoint, max: LandscapePoint): VTPageAddress {
        for (let level = desiredLevel; level <= this._rootLevel; ++level) {
            const size = this.pageSize(level);
            const page = new VTPageAddress(level, Math.floor(min.x / size + 1e-7), Math.floor(min.z / size + 1e-7));
            if (max.x <= (page.x + 1) * size + size * 1e-7 && max.z <= (page.z + 1) * size + size * 1e-7) {
                return page;
            }
        }
        throw new Error('VT coverage must not cross sector boundaries');
    }

    private mapCells (firstCell: LandscapeRegion, camera: Readonly<Vec3>, heightDistance: number, left: number, bottom: number, side: number, mapping: VTGridMapping): void {
        const cellSize = firstCell.size;
        const x = firstCell.x + left * cellSize;
        const z = firstCell.z + bottom * cellSize;
        const size = side * cellSize;
        const dx = Math.max(x - camera.x, camera.x - x - size, 0);
        const dz = Math.max(z - camera.z, camera.z - z - size, 0);
        const level = this.levelForDistance(Math.sqrt(dx * dx + heightDistance * heightDistance + dz * dz));
        if (side > 1 && this.pageSize(level) < size) {
            const half = side / 2;
            for (let row = bottom; row < bottom + side; row += half) {
                for (let col = left; col < left + side; col += half) {
                    this.mapCells(firstCell, camera, heightDistance, col, row, half, mapping);
                }
            }
            return;
        }
        // Include the complete trajectory of odd vertices during morph/stitching.
        const page = this.coveringPage(level, { x: x - (left & 1) * cellSize, z: z - (bottom & 1) * cellSize }, { x: x + size, z: z + size });
        let index = mapping.pages.findIndex((entry) => entry.key() === page.key());
        if (index < 0) {
            index = mapping.pages.length;
            mapping.pages.push(page);
        }
        for (let row = bottom; row < bottom + side; ++row) {
            mapping.cells.fill(index, row * VTGridMapping.SIDE + left, row * VTGridMapping.SIDE + left + side);
        }
    }
}

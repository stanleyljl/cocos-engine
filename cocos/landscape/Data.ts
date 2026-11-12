// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import type { Vec3 } from '../core/math';
import { Vec4 } from '../core/math';
import * as config from './Config';

export interface LandscapePoint {
    x: number;
    z: number;
}

export interface LandscapeRegion extends LandscapePoint {
    size: number;
}

// Grid instance parameters shared by terrain and displaced decals.
export class LandscapeGridParams {
    public grid = new Vec4(); // landscape-local XZ origin, size, geometry LOD
    public tile = new Vec4(); // source tile XZ origin, size, texture array layer
    public mapping = new Vec4(); // Local table slot, packed 2-bit neighbor relations per edge, unused ZW.
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
    public static readonly SIDE_BITS = 4;
    public static readonly SIDE = 1 << VTGridMapping.SIDE_BITS;
    public static readonly CELL_COUNT = VTGridMapping.SIDE * VTGridMapping.SIDE;
    public readonly cells = new Uint8Array(VTGridMapping.CELL_COUNT);
    public pages: VTPageAddress[] = [];
}

export class VTPageSources {
    public splat = new NodeAddress();
    public normals: NodeAddress[] = Array.from({ length: config.VT_NORMAL_SOURCE_COUNT }, () => new NodeAddress());
    // Side projections sample the padded height data of the splat source.
    public triplanar = false;

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
    public maxTileLevel = 0; // Coarsest source tile level; also the sector root level.
    public minTileLevel = 0; // Finest source tile level; finer geometry nodes sample ancestor tiles.
    public sectorSize = 0; // Positive power of two.
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
            && Number.isInteger(this.maxTileLevel) && this.maxTileLevel >= 0 && this.maxTileLevel < config.MAX_LOD_LEVELS
            && this.sectorsX * this.sectorsZ <= config.MAX_SECTOR_COUNT
            && Number.isInteger(this.minTileLevel) && this.minTileLevel >= 0 && this.minTileLevel <= this.maxTileLevel
            && this.sectorSize > 0 && 2 ** Math.floor(Math.log2(this.sectorSize)) === this.sectorSize
            && this.heightScale > 0;
    }

    public nodesPerSectorSide (level: number): number {
        return level >= 0 && level <= this.maxTileLevel && this.maxTileLevel < config.MAX_LOD_LEVELS ? 2 ** (this.maxTileLevel - level) : 0;
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

// Immutable inputs shared by one synchronous mapping traversal; never retained.
interface GridMappingContext {
    readonly node: NodeAddress;
    readonly region: LandscapeRegion;
    readonly cellSize: number;
    // VT-relative level; negative when cells are smaller than an L0 page.
    readonly cellLevel: number;
    readonly camera: Readonly<Vec3>;
    readonly heightDistance: number;
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

    // Subdivide a geometry node by distance; integer cell coordinates determine
    // the containing page, including the complete morph/stitch trajectory.
    public buildGridMapping (data: LandscapeData, node: NodeAddress, minY: number, maxY: number, camera: Readonly<Vec3>): VTGridMapping {
        const result = new VTGridMapping();
        const region = data.nodeRegion(node);
        const context: GridMappingContext = {
            node,
            region,
            cellSize: region.size / VTGridMapping.SIDE,
            cellLevel: this._rootLevel - (data.maxTileLevel - node.level + VTGridMapping.SIDE_BITS),
            camera,
            heightDistance: Math.max(minY - camera.y, camera.y - maxY, 0),
        };
        // Keep the four initial quadrants and traversal order used for page admission.
        const half = VTGridMapping.SIDE / 2;
        for (let z = 0; z < VTGridMapping.SIDE; z += half) {
            for (let x = 0; x < VTGridMapping.SIDE; x += half) {
                this.mapGridBlock(context, x, z, VTGridMapping.SIDE_BITS - 1, result);
            }
        }
        return result;
    }

    private mapGridBlock (context: GridMappingContext, left: number, bottom: number, blockLevel: number, result: VTGridMapping): void {
        const side = 1 << blockLevel;
        const x = context.region.x + left * context.cellSize;
        const z = context.region.z + bottom * context.cellSize;
        const size = side * context.cellSize;
        const dx = Math.max(x - context.camera.x, context.camera.x - x - size, 0);
        const dz = Math.max(z - context.camera.z, context.camera.z - z - size, 0);
        const desiredLevel = this.levelForDistance(Math.sqrt(dx * dx + context.heightDistance * context.heightDistance + dz * dz));
        if (blockLevel > 0 && this.pageSize(desiredLevel) < size) {
            const half = side / 2;
            for (let row = bottom; row < bottom + side; row += half) {
                for (let column = left; column < left + side; column += half) {
                    this.mapGridBlock(context, column, row, blockLevel - 1, result);
                }
            }
            return;
        }
        // Aligned blocks contain every morph target. An odd single cell needs
        // its 2x2 parent because odd vertices move to the preceding even vertex.
        const coverageLevel = context.cellLevel + blockLevel + (blockLevel === 0 && ((left | bottom) & 1) ? 1 : 0);
        const level = Math.max(desiredLevel, coverageLevel);
        const shift = level - context.cellLevel;
        const page = new VTPageAddress(level, (context.node.x * VTGridMapping.SIDE + left) >>> shift, (context.node.z * VTGridMapping.SIDE + bottom) >>> shift);
        let index = result.pages.findIndex((entry) => entry.key() === page.key());
        if (index < 0) {
            index = result.pages.length;
            result.pages.push(page);
        }
        for (let row = bottom; row < bottom + side; ++row) {
            result.cells.fill(index, row * VTGridMapping.SIDE + left, row * VTGridMapping.SIDE + left + side);
        }
    }
}

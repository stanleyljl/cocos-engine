// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Vec3, Vec4 } from '../core/math';
import type { Frustum } from '../core/geometry';
import { log, warn } from '../core/platform/debug';
import * as config from './Config';
import { HeightRange, LandscapeData, LandscapeRegion, NodeAddress, NodeIndexLayout, NodeKey, QuadNode, VTGridMapping } from './Data';

export interface MorphRange {
    start: number;
    end: number;
}

export class QuadtreeNode {
    public geometry = new QuadNode();
    public material = new VTGridMapping();
    public children: QuadtreeNode[] | null = null;
    public loaded = false;
    public ready = false;
    public renderable = false;
    public edgeMask = 0;
}

interface Pass {
    planes: Vec4[];
    includeNearby: boolean;
}

interface HeightData {
    data(): LandscapeData;
    heightRanges(): readonly HeightRange[];
}

// Target hierarchy, published mappings and balanced render selection. Schedule
// supplies completed resources; Renderer consumes selected nodes and changes.
export class Quadtree {
    private _data!: LandscapeData;
    private _nodeIndexLayout!: NodeIndexLayout;
    private _heightRanges: readonly HeightRange[] = [];
    private _root = new QuadtreeNode();
    private _renderNodeKeys = new Set<NodeKey>();
    private _renderSelectionChanged = false;
    private _needsRenderSelection = true;
    private readonly _changedNodes = new Set<NodeKey>();
    private _lodMorphRanges: MorphRange[] = [];
    private readonly _localCameraPosition = new Vec3();
    private readonly _worldOrigin = new Vec3();
    private readonly _passes: Pass[] = [];
    private _activePassCount = 0;
    private _viewChanged = true;
    private _lastMorphRangeWarning = false;

    public init (asset: HeightData, lodQualityScale = 1): void {
        this._data = asset.data();
        this._nodeIndexLayout = new NodeIndexLayout(this._data);
        this._heightRanges = asset.heightRanges();
        this._root = new QuadtreeNode();
        this._renderNodeKeys.clear();
        this._renderSelectionChanged = false;
        this._needsRenderSelection = true;
        this._changedNodes.clear();
        this._passes.length = 0;
        this._activePassCount = 0;
        this._lastMorphRangeWarning = false;
        let rootLevel = this._data.maxLevel;
        for (let side = 1; side < Math.max(this._data.sectorsX, this._data.sectorsZ); side *= 2) {
            ++rootLevel;
        }
        this.buildRoot(this._root, new NodeAddress(rootLevel));
        this.initLodMorphRanges(lodQualityScale);
    }

    private initLodMorphRanges (quality: number): void {
        let totalWeight = 0;
        let weight = 1;
        for (let level = 0; level <= this._data.maxLevel; ++level) {
            totalWeight += weight;
            weight *= config.LOD_DISTANCE_RATIO;
        }
        const section = Math.hypot(this._data.width(), this._data.depth()) * quality / totalWeight;
        let end = 0;
        let start = 0;
        weight = 1;
        this._lodMorphRanges = [];
        for (let level = 0; level <= this._data.maxLevel; ++level) {
            end += section * weight;
            start += (end - start) * config.MORPH_START_RATIO;
            this._lodMorphRanges.push({ start, end });
            weight *= config.LOD_DISTANCE_RATIO;
        }
    }

    public beginUpdate (camera: Readonly<Vec3>, origin: Readonly<Vec3>): void {
        const position = Vec3.subtract(new Vec3(), camera, origin);
        this._viewChanged = !Vec3.strictEquals(position, this._localCameraPosition) || !Vec3.strictEquals(origin, this._worldOrigin);
        this._localCameraPosition.set(position);
        this._worldOrigin.set(origin);
        this._activePassCount = 0;
    }

    public addPass (frustum: Readonly<Frustum>, includeNearby: boolean): void {
        if (this._activePassCount === this._passes.length) {
            this._passes.push({ planes: Array.from({ length: 6 }, () => new Vec4()), includeNearby: false });
            this._viewChanged = true;
        }
        const pass = this._passes[this._activePassCount++];
        this._viewChanged = this._viewChanged || pass.includeNearby !== includeNearby;
        pass.includeNearby = includeNearby;
        for (let i = 0; i < 6; ++i) {
            const plane = frustum.planes[i];
            const saved = pass.planes[i];
            this._viewChanged = this._viewChanged || saved.x !== plane.n.x || saved.y !== plane.n.y || saved.z !== plane.n.z || saved.w !== plane.d;
            saved.set(plane.n.x, plane.n.y, plane.n.z, plane.d);
        }
    }

    public updateTarget (): void {
        this._viewChanged = this._viewChanged || this._activePassCount !== this._passes.length;
        this._passes.length = this._activePassCount;
        if (this._viewChanged) {
            this.updateTargetNode(this._root);
            this._needsRenderSelection = true;
        }
    }

    private updateTargetNode (node: QuadtreeNode): void {
        const address = node.geometry.address();
        if (!this.contains(address)) {
            return;
        }
        let refine = address.level > this._data.maxLevel;
        if (!refine && address.level > 0) {
            const range = this._lodMorphRanges[address.level - 1].end;
            for (let q = 0; q < 4; ++q) {
                if (this.distanceSquared(Quadtree.child(address, q)) <= range * range) {
                    refine = true;
                    break;
                }
            }
        }
        if (!refine) {
            node.children = null;
            return;
        }
        if (!node.children) {
            node.children = Array.from({ length: 4 }, (_, q) => {
                const child = new QuadtreeNode();
                child.geometry = this.geometry(Quadtree.child(address, q));
                return child;
            });
        }
        for (const child of node.children) {
            this.updateTargetNode(child);
        }
    }

    public requests (): NodeAddress[] {
        const missing: NodeAddress[] = [];
        const refresh: NodeAddress[] = [];
        this.forEachNode((node) => {
            const address = node.geometry.address();
            if (address.level > this._data.maxLevel) {
                return true;
            }
            if (!node.ready) {
                missing.push(address);
                return false;
            }
            if (node.renderable || !node.children) {
                refresh.push(address);
            }
            return true;
        });
        // Native keys sort by level, X, Z. String keys must not sort lexically.
        const priority = (a: NodeAddress, b: NodeAddress): number => a.level - b.level || a.x - b.x || a.z - b.z;
        missing.sort(priority);
        refresh.sort(priority);
        return missing.concat(refresh);
    }

    public publish (address: NodeAddress, material: VTGridMapping): boolean {
        const node = this.find(address);
        if (!node || address.level > this._data.maxLevel || material.pages.length === 0) {
            return false;
        }
        if (address.level < this._data.maxLevel && !this.find(address.ancestor(address.level + 1))!.ready) {
            return false;
        }
        this._needsRenderSelection = this._needsRenderSelection || !node.ready;
        node.material = material;
        node.loaded = true;
        node.ready = true;
        if (node.renderable) {
            this._changedNodes.add(address.key());
        }
        return true;
    }

    public selectRenderables (): boolean {
        this._renderSelectionChanged = false;
        if (!this._needsRenderSelection) {
            return true;
        }
        if (!this.sectorRootsReady()) {
            return false;
        }
        let current: QuadtreeNode[] = [];
        this.forEachNode((node) => {
            node.renderable = node.geometry.level === this._data.maxLevel;
            if (node.renderable) {
                current.push(node);
            }
            return true;
        });
        while (current.length) {
            const next: QuadtreeNode[] = [];
            for (const node of current) {
                if (!node.children || !node.children.every((child) => child.ready)) {
                    continue;
                }
                if ((this.computeEdgeMask(node.geometry.address()) & 0x55) !== 0) {
                    continue;
                }
                node.renderable = false;
                for (const child of node.children) {
                    child.renderable = true;
                    next.push(child);
                }
            }
            current = next;
        }
        const previous = this._renderNodeKeys;
        this._renderNodeKeys = new Set<NodeKey>();
        let morphRangeWarning = false;
        this.forEachNode((node) => {
            if (!node.renderable) {
                return true;
            }
            const address = node.geometry.address();
            const edges = this.computeEdgeMask(address);
            this._renderNodeKeys.add(address.key());
            const wasRenderable = previous.delete(address.key());
            if (!wasRenderable || node.edgeMask !== edges) {
                this._changedNodes.add(address.key());
                this._renderSelectionChanged = true;
            }
            node.edgeMask = edges;
            if (config.LANDSCAPE_DEBUG && !morphRangeWarning) {
                morphRangeWarning = this.morphRangeTooSmall(node);
            }
            return false;
        });
        for (const removed of previous) {
            this._changedNodes.add(removed);
            this._renderSelectionChanged = true;
        }
        this._needsRenderSelection = false;
        if (config.LANDSCAPE_DEBUG && morphRangeWarning !== this._lastMorphRangeWarning) {
            if (morphRangeWarning) {
                warn('[Landscape] CDLOD granularity check: visibility ranges are too small for a guaranteed transition');
            } else {
                log('[Landscape] CDLOD granularity check: OK');
            }
            this._lastMorphRangeWarning = morphRangeWarning;
        }
        return true;
    }

    public viewChanged (): boolean {
        return this._viewChanged;
    }
    public renderSelectionChanged (): boolean {
        return this._renderSelectionChanged;
    }
    public localCameraPosition (): Readonly<Vec3> {
        return this._localCameraPosition;
    }
    public lodMorphRanges (): readonly MorphRange[] {
        return this._lodMorphRanges;
    }
    public changedNodes (): ReadonlySet<NodeKey> {
        return this._changedNodes;
    }
    public clearChangedNodes (): void {
        this._changedNodes.clear();
    }

    public forEachNode (visit: (node: QuadtreeNode) => boolean): void {
        this.visitNodes(this._root, visit);
    }

    public cull (visit: (address: NodeAddress, renderable: boolean) => boolean): void {
        if (!this._renderNodeKeys.size) {
            return;
        }
        this.forEachNode((node) => visit(node.geometry.address(), node.renderable) && !node.renderable);
    }

    public forEachRenderable (visit: (node: QuadtreeNode) => void): void {
        if (!this._renderNodeKeys.size) {
            return;
        }
        this.forEachNode((node) => {
            if (node.renderable) {
                visit(node);
            }
            return !node.renderable;
        });
    }

    private visitNodes (node: QuadtreeNode, visit: (node: QuadtreeNode) => boolean): void {
        if (!this.contains(node.geometry.address()) || !visit(node) || !node.children) {
            return;
        }
        for (const child of node.children) {
            this.visitNodes(child, visit);
        }
    }

    private computeEdgeMask (address: NodeAddress): number {
        const width = this._data.sectorsX * 2 ** this._data.maxLevel;
        const depth = this._data.sectorsZ * 2 ** this._data.maxLevel;
        const size = 2 ** address.level;
        const x = address.x * size;
        const z = address.z * size;
        let edges = 0;
        const add = (neighbor: NodeAddress, edge: number): void => {
            if (neighbor.level > address.level) {
                edges |= 1 << (edge * 2);
            } else if (neighbor.level < address.level) {
                edges |= 2 << (edge * 2);
            }
        };
        if (x > 0) {
            add(this.renderableAt(x - 1, z), 0);
        }
        if (x + size < width) {
            add(this.renderableAt(x + size, z), 1);
        }
        if (z > 0) {
            add(this.renderableAt(x, z - 1), 2);
        }
        if (z + size < depth) {
            add(this.renderableAt(x, z + size), 3);
        }
        return edges;
    }

    private morphRangeTooSmall (node: QuadtreeNode): boolean {
        const address = node.geometry.address();
        if (address.level === this._data.maxLevel) {
            return false;
        }
        const area = this.region(address);
        const camera = this._localCameraPosition;
        const x = Math.max(Math.abs(area.x - camera.x), Math.abs(area.x + area.size - camera.x));
        const y = Math.max(Math.abs(node.geometry.minY - camera.y), Math.abs(node.geometry.maxY - camera.y));
        const z = Math.max(Math.abs(area.z - camera.z), Math.abs(area.z + area.size - camera.z));
        const start = this._lodMorphRanges[address.level + 1].start;
        if (x * x + y * y + z * z <= start * start) {
            return false;
        }
        const distance = this.distanceSquared(address);
        const range = this._lodMorphRanges[address.level].end;
        if (distance > range * range) {
            return false;
        }
        const bounds = this.heightRange(address);
        const center = new Vec3(area.x + area.size * 0.5 + this._worldOrigin.x, (bounds.minY + bounds.maxY) * 0.5 + this._worldOrigin.y, area.z + area.size * 0.5 + this._worldOrigin.z);
        const extent = new Vec3(area.size * 0.5, (bounds.maxY - bounds.minY) * 0.5, area.size * 0.5);
        const nearby = this._lodMorphRanges[Math.min(1, this._data.maxLevel)].end;
        return this._passes.some((pass) => (pass.includeNearby && distance <= nearby * nearby) || pass.planes.every((plane) => {
            const radius = extent.x * Math.abs(plane.x) + extent.y * Math.abs(plane.y) + extent.z * Math.abs(plane.z);
            return center.x * plane.x + center.y * plane.y + center.z * plane.z + radius >= plane.w;
        }));
    }

    private buildRoot (node: QuadtreeNode, address: NodeAddress): void {
        node.geometry = new QuadNode(address.level, address.x, address.z);
        if (!this.contains(address)) {
            return;
        }
        if (address.level === this._data.maxLevel) {
            node.geometry = this.geometry(address);
            return;
        }
        node.children = Array.from({ length: 4 }, () => new QuadtreeNode());
        let minY = Number.MAX_VALUE;
        let maxY = -Number.MAX_VALUE;
        for (let q = 0; q < 4; ++q) {
            const child = Quadtree.child(address, q);
            this.buildRoot(node.children[q], child);
            if (this.contains(child)) {
                minY = Math.min(minY, node.children[q].geometry.minY);
                maxY = Math.max(maxY, node.children[q].geometry.maxY);
            }
        }
        node.geometry.minY = minY;
        node.geometry.maxY = maxY;
    }

    private static child (address: NodeAddress, quadrant: number): NodeAddress {
        return new NodeAddress(address.level - 1, address.x * 2 + (quadrant & 1), address.z * 2 + (quadrant >> 1));
    }

    public sectorRootsReady (): boolean {
        for (let z = 0; z < this._data.sectorsZ; ++z) {
            for (let x = 0; x < this._data.sectorsX; ++x) {
                if (!this.find(new NodeAddress(this._data.maxLevel, x, z))!.ready) {
                    return false;
                }
            }
        }
        return true;
    }

    private contains (address: NodeAddress): boolean {
        const scale = 2 ** (address.level - this._data.maxLevel);
        return address.x >= 0 && address.z >= 0 && address.x * scale < this._data.sectorsX && address.z * scale < this._data.sectorsZ;
    }

    public find (address: NodeAddress): QuadtreeNode | null {
        if (!this.contains(address) || address.level > this._root.geometry.level) {
            return null;
        }
        let node = this._root;
        while (node.geometry.level > address.level) {
            if (!node.children) {
                return null;
            }
            const shift = node.geometry.level - 1 - address.level;
            const quadrant = ((address.x >> shift) & 1) | (((address.z >> shift) & 1) << 1);
            node = node.children[quadrant];
        }
        return node;
    }

    public findRenderable (address: NodeAddress): QuadtreeNode | null {
        const node = this.find(address);
        return node && node.renderable ? node : null;
    }

    public region (address: NodeAddress): LandscapeRegion {
        const size = this._data.sectorSize * 2 ** (address.level - this._data.maxLevel);
        return { x: address.x * size, z: address.z * size, size };
    }

    private heightRange (address: NodeAddress): HeightRange {
        if (address.level > this._data.maxLevel) {
            const bounds = this.find(address)!.geometry;
            return { minY: bounds.minY, maxY: bounds.maxY };
        }
        return this._heightRanges[this._nodeIndexLayout.globalNodeIndex(address.level, address.x, address.z)];
    }

    private geometry (address: NodeAddress): QuadNode {
        const bounds = { ...this.heightRange(address) };
        if (address.level < this._data.minTileLevel) {
            const source = this.heightRange(address.ancestor(this._data.minTileLevel));
            bounds.minY = Math.min(bounds.minY, source.minY);
            bounds.maxY = Math.max(bounds.maxY, source.maxY);
        }
        return new QuadNode(address.level, address.x, address.z, bounds.minY, bounds.maxY);
    }

    private renderableAt (x: number, z: number): NodeAddress {
        let node = this._root;
        while (!node.renderable) {
            const shift = node.geometry.level - 1;
            node = node.children![((x >> shift) & 1) | (((z >> shift) & 1) << 1)];
        }
        return node.geometry.address();
    }

    private distanceSquared (address: NodeAddress): number {
        const bounds = this.heightRange(address);
        const area = this.region(address);
        const camera = this._localCameraPosition;
        const x = Math.max(area.x - camera.x, camera.x - area.x - area.size, 0);
        const y = Math.max(bounds.minY - camera.y, camera.y - bounds.maxY, 0);
        const z = Math.max(area.z - camera.z, camera.z - area.z - area.size, 0);
        return x * x + y * y + z * z;
    }
}

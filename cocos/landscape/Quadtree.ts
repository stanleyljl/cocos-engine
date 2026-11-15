// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { Vec3 } from '../core/math';
import * as config from './Config';
import { HeightRange, LandscapeData, LandscapeRegion, NodeAddress, NodeKey, QuadNode, ReadonlyVTGridMapping, VTGridMapping } from './Data';

export interface MorphRange {
    start: number;
    end: number;
}

export interface QuadtreeNode {
    readonly geometry: Readonly<QuadNode>;
    readonly material: ReadonlyVTGridMapping;
    readonly children: readonly QuadtreeNode[] | null;
    readonly refined: boolean;
    readonly ready: boolean;
    readonly renderable: boolean;
    readonly edgeMask: number;
}

class MutableQuadtreeNode implements QuadtreeNode {
    public geometry = new QuadNode();
    public material = new VTGridMapping();
    public children: MutableQuadtreeNode[] | null = null;
    public refined = false; // Target subdivision; children survive until render selection.
    public ready = false;
    public renderable = false;
    public edgeMask = 0;
}

interface HeightData {
    data(): LandscapeData;
    heightRanges(): readonly (readonly HeightRange[])[];
}

// Owns the target hierarchy, published mappings and balanced render selection.
export class Quadtree {
    private _data!: LandscapeData;
    private _heightRanges: readonly (readonly HeightRange[])[] = [];
    private _lodMorphRanges: MorphRange[] = [];

    private _root = new MutableQuadtreeNode();
    private readonly _localCameraPosition = new Vec3();
    private readonly _worldOrigin = new Vec3();
    private _viewChanged = true;

    // Previous selection survives target changes until draws are synchronized.
    private _renderNodeKeys = new Set<NodeKey>();
    private _needsRenderSelection = true;
    private _renderSelectionChanged = false;
    private readonly _changedNodes = new Set<NodeKey>();

    public init (asset: HeightData, lodQualityScale = 1): void {
        this._data = asset.data();
        this._heightRanges = asset.heightRanges();
        this._root = new MutableQuadtreeNode();
        this._renderNodeKeys.clear();
        this._renderSelectionChanged = false;
        this._needsRenderSelection = true;
        this._changedNodes.clear();
        let rootLevel = this._data.maxTileLevel;
        for (let side = 1; side < Math.max(this._data.sectorsX, this._data.sectorsZ); side *= 2) {
            ++rootLevel;
        }
        this.buildRoot(this._root, new NodeAddress(rootLevel));
        this.initLodMorphRanges(lodQualityScale);
    }

    // Target and render state.
    public updateTarget (camera: Readonly<Vec3>, origin: Readonly<Vec3>): void {
        const position = Vec3.subtract(new Vec3(), camera, origin);
        this._viewChanged = !Vec3.strictEquals(position, this._localCameraPosition) || !Vec3.strictEquals(origin, this._worldOrigin);
        this._localCameraPosition.set(position);
        this._worldOrigin.set(origin);
        if (this._viewChanged || this._needsRenderSelection) {
            this.updateTargetNode(this._root);
            this._needsRenderSelection = true;
        }
    }

    public requests (): NodeAddress[] {
        const missing: NodeAddress[] = [];
        const refresh: NodeAddress[] = [];
        this.forEachNode((node) => {
            const address = node.geometry.address();
            if (address.level > this._data.maxTileLevel) {
                return true;
            }
            if (!node.ready) {
                missing.push(address);
                return false;
            }
            if (node.renderable || !node.refined) {
                refresh.push(address);
            }
            return node.refined;
        });
        // Native keys sort by level, X, Z. String keys must not sort lexically.
        const priority = (a: NodeAddress, b: NodeAddress): number => a.level - b.level || a.x - b.x || a.z - b.z;
        missing.sort(priority);
        refresh.sort(priority);
        return missing.concat(refresh);
    }

    // Publish only after the source and complete mapping are ready.
    public publish (address: NodeAddress, material: VTGridMapping): boolean {
        if (!this.isTarget(address)) {
            return false;
        }
        const node = this.findNode(address)!;
        if (address.level > this._data.maxTileLevel || material.pages.length === 0) {
            return false;
        }
        if (address.level < this._data.maxTileLevel && !this.find(address.ancestor(address.level + 1))!.ready) {
            return false;
        }
        this._needsRenderSelection = this._needsRenderSelection || !node.ready;
        node.material = material;
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
        let current: MutableQuadtreeNode[] = [];
        this.visitNodes(this._root, (node) => {
            node.renderable = node.geometry.level === this._data.maxTileLevel;
            if (node.renderable) {
                current.push(node);
            }
            return true;
        });
        while (current.length) {
            const next: MutableQuadtreeNode[] = [];
            for (const node of current) {
                if (!node.refined || !node.children!.every((child) => child.ready)) {
                    continue;
                }
                if ((this.computeEdgeMask(node.geometry.address()) & 0x55) !== 0) {
                    continue;
                }
                node.renderable = false;
                for (const child of node.children!) {
                    child.renderable = true;
                    next.push(child);
                }
            }
            current = next;
        }
        const previous = this._renderNodeKeys;
        this._renderNodeKeys = new Set<NodeKey>();
        this.visitNodes(this._root, (node) => {
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
            return false;
        });
        for (const removed of previous) {
            this._changedNodes.add(removed);
            this._renderSelectionChanged = true;
        }
        this.visitNodes(this._root, (node) => {
            if (!node.refined) {
                node.children = null;
            }
            return node.refined;
        });
        this._needsRenderSelection = false;
        return true;
    }

    // State and changes.
    public viewChanged (): boolean {
        return this._viewChanged;
    }

    public sectorRootsReady (): boolean {
        for (let z = 0; z < this._data.sectorsZ; ++z) {
            for (let x = 0; x < this._data.sectorsX; ++x) {
                if (!this.find(new NodeAddress(this._data.maxTileLevel, x, z))!.ready) {
                    return false;
                }
            }
        }
        return true;
    }

    // Retained until the next selection, independently of changedNodes.
    public renderSelectionChanged (): boolean {
        return this._renderSelectionChanged;
    }

    public changedNodes (): ReadonlySet<NodeKey> {
        return this._changedNodes;
    }

    public clearChangedNodes (): void {
        this._changedNodes.clear();
    }

    // Node queries.
    public find (address: NodeAddress): QuadtreeNode | null {
        return this.findNode(address);
    }

    public findRenderable (address: NodeAddress): QuadtreeNode | null {
        const node = this.find(address);
        return node && node.renderable ? node : null;
    }

    public isTarget (address: NodeAddress): boolean {
        if (!this.find(address)) {
            return false;
        }
        while (address.level < this._root.geometry.level) {
            address = address.ancestor(address.level + 1);
            if (!this.find(address)!.refined) {
                return false;
            }
        }
        return true;
    }

    // Includes management levels above sector roots.
    public region (address: NodeAddress): LandscapeRegion {
        const size = this._data.sectorSize * 2 ** (address.level - this._data.maxTileLevel);
        return { x: address.x * size, z: address.z * size, size };
    }

    public localCameraPosition (): Readonly<Vec3> {
        return this._localCameraPosition;
    }

    public worldOrigin (): Readonly<Vec3> {
        return this._worldOrigin;
    }

    public lodMorphRanges (): readonly Readonly<MorphRange>[] {
        return this._lodMorphRanges;
    }

    // Traversal: boolean callbacks return false to prune descendants.
    public forEachNode (visit: (node: QuadtreeNode) => boolean): void {
        this.visitNodes(this._root, visit);
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

    public cull (visit: (node: QuadtreeNode) => boolean): void {
        if (!this._renderNodeKeys.size) {
            return;
        }
        this.forEachNode((node) => visit(node) && !node.renderable);
    }

    // Hierarchy.
    private initLodMorphRanges (quality: number): void {
        let totalWeight = 0;
        let weight = 1;
        for (let level = 0; level <= this._data.maxTileLevel; ++level) {
            totalWeight += weight;
            weight *= config.LOD_DISTANCE_RATIO;
        }
        const section = Math.hypot(this._data.width(), this._data.depth()) * quality / totalWeight;
        let end = 0;
        let start = 0;
        weight = 1;
        this._lodMorphRanges = [];
        for (let level = 0; level <= this._data.maxTileLevel; ++level) {
            end += section * weight;
            start += (end - start) * config.MORPH_START_RATIO;
            this._lodMorphRanges.push({ start, end });
            weight *= config.LOD_DISTANCE_RATIO;
        }
    }

    private buildRoot (node: MutableQuadtreeNode, address: NodeAddress): void {
        node.geometry = new QuadNode(address.level, address.x, address.z);
        if (!this.contains(address)) {
            return;
        }
        if (address.level === this._data.maxTileLevel) {
            node.geometry = this.geometry(address);
            return;
        }
        node.refined = true;
        node.children = Array.from({ length: 4 }, () => new MutableQuadtreeNode());
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

    private updateTargetNode (node: MutableQuadtreeNode): void {
        const address = node.geometry.address();
        if (!this.contains(address)) {
            return;
        }
        let refine = address.level > this._data.maxTileLevel;
        if (!refine && address.level > 0) {
            const range = this._lodMorphRanges[address.level - 1].end;
            for (let q = 0; q < 4; ++q) {
                if (this.distanceSquared(Quadtree.child(address, q)) <= range * range) {
                    refine = true;
                    break;
                }
            }
        }
        node.refined = refine;
        if (!refine) {
            return;
        }
        if (!node.children) {
            node.children = Array.from({ length: 4 }, (_, q) => {
                const child = new MutableQuadtreeNode();
                child.geometry = this.geometry(Quadtree.child(address, q));
                return child;
            });
        }
        for (const child of node.children) {
            this.updateTargetNode(child);
        }
    }

    private static child (address: NodeAddress, quadrant: number): NodeAddress {
        return new NodeAddress(address.level - 1, address.x * 2 + (quadrant & 1), address.z * 2 + (quadrant >> 1));
    }

    private findNode (address: NodeAddress): MutableQuadtreeNode | null {
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

    private contains (address: NodeAddress): boolean {
        const scale = 2 ** (address.level - this._data.maxTileLevel);
        return address.x >= 0 && address.z >= 0 && address.x * scale < this._data.sectorsX && address.z * scale < this._data.sectorsZ;
    }

    // Bounds and morph edges.
    private geometry (address: NodeAddress): QuadNode {
        const bounds = { ...this.heightRange(address) };
        if (address.level < this._data.minTileLevel) {
            const source = this.heightRange(address.ancestor(this._data.minTileLevel));
            bounds.minY = Math.min(bounds.minY, source.minY);
            bounds.maxY = Math.max(bounds.maxY, source.maxY);
        }
        return new QuadNode(address.level, address.x, address.z, bounds.minY, bounds.maxY);
    }

    private heightRange (address: NodeAddress): HeightRange {
        if (address.level > this._data.maxTileLevel) {
            const bounds = this.find(address)!.geometry;
            return { minY: bounds.minY, maxY: bounds.maxY };
        }
        return this._heightRanges[address.level][address.z * this._data.nodesX(address.level) + address.x];
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

    private renderableAt (x: number, z: number): NodeAddress {
        let node = this._root;
        while (!node.renderable) {
            const shift = node.geometry.level - 1;
            node = node.children![((x >> shift) & 1) | (((z >> shift) & 1) << 1)];
        }
        return node.geometry.address();
    }

    private computeEdgeMask (address: NodeAddress): number {
        const width = this._data.sectorsX * 2 ** this._data.maxTileLevel;
        const depth = this._data.sectorsZ * 2 ** this._data.maxTileLevel;
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

    private visitNodes (node: MutableQuadtreeNode, visit: (node: MutableQuadtreeNode) => boolean): void {
        if (!this.contains(node.geometry.address()) || !visit(node) || !node.children) {
            return;
        }
        for (const child of node.children) {
            this.visitNodes(child, visit);
        }
    }
}

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { AABB, Frustum, intersect } from '../core/geometry';
import { Vec3 } from '../core/math';
import { log, warn } from '../core/platform/debug';
import type { LandscapeAsset } from './Asset';
import * as config from './Config';
import { HeightRange, LandscapeData, LandscapeRegion, NodeAddress, NodeKey, QuadNode, ReadonlyVTGridMapping, VTGridMapping, VTPageAddress } from './Data';

function pointAABBMinDistanceSquared (point: Readonly<Vec3>, region: Readonly<LandscapeRegion>, minY: number, maxY: number): number {
    const x = Math.max(region.x - point.x, point.x - region.x - region.size, 0);
    const y = Math.max(minY - point.y, point.y - maxY, 0);
    const z = Math.max(region.z - point.z, point.z - region.z - region.size, 0);
    return x * x + y * y + z * z;
}

function pointAABBMaxDistanceSquared (point: Readonly<Vec3>, region: Readonly<LandscapeRegion>, minY: number, maxY: number): number {
    const x = Math.max(Math.abs(region.x - point.x), Math.abs(region.x + region.size - point.x));
    const y = Math.max(Math.abs(minY - point.y), Math.abs(maxY - point.y));
    const z = Math.max(Math.abs(region.z - point.z), Math.abs(region.z + region.size - point.z));
    return x * x + y * y + z * z;
}

export interface MorphRange {
    start: number;
    end: number;
}

export interface TargetTreeNode {
    readonly geometry: Readonly<QuadNode>;
    readonly children: readonly TargetTreeNode[] | null;
}

class MutableTargetTreeNode implements TargetTreeNode {
    public geometry = new QuadNode();
    public children: MutableTargetTreeNode[] | null = null;
}

// Distance-based target topology. Resource readiness never changes this tree.
export class TargetTree {
    private _data!: LandscapeData;
    private _heightRanges: readonly (readonly HeightRange[])[] = [];
    private _lodMorphRanges: MorphRange[] = [];

    private _root = new MutableTargetTreeNode();
    private readonly _localCameraPosition = new Vec3();
    private readonly _worldOrigin = new Vec3();
    private _needsUpdate = true; // Force the first target update

    public init (asset: LandscapeAsset, lodQualityScale = 1): void {
        this._data = asset.data();
        this._heightRanges = asset.heightRanges();
        this._root = new MutableTargetTreeNode();
        let rootLevel = this._data.maxTileLevel;
        for (let side = 1; side < Math.max(this._data.sectorsX, this._data.sectorsZ); side *= 2) {
            ++rootLevel;
        }
        this.buildRoot(this._root, new NodeAddress(rootLevel));
        this.initLodMorphRanges(lodQualityScale);
    }

    public updateTarget (camera: Readonly<Vec3>, origin: Readonly<Vec3>): boolean {
        const position = Vec3.subtract(new Vec3(), camera, origin);
        if (!this._needsUpdate && Vec3.strictEquals(position, this._localCameraPosition) && Vec3.strictEquals(origin, this._worldOrigin)) {
            return false;
        }
        this._localCameraPosition.set(position);
        this._worldOrigin.set(origin);
        this.updateTargetNode(this._root);
        this._needsUpdate = false;
        return true;
    }

    public root (): TargetTreeNode {
        return this._root;
    }

    public data (): LandscapeData {
        return this._data;
    }

    public find (address: NodeAddress): TargetTreeNode | null {
        return this.findNode(address);
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

    // Diagnose morph constraints for nodes within their LOD distance range.
    public morphRangeTooSmall (node: Readonly<QuadNode>): boolean {
        const address = node.address();
        if (address.level === this._data.maxTileLevel) {
            return false;
        }
        const range = this._lodMorphRanges[address.level].end;
        if (this.distanceSquared(address) > range * range) {
            return false;
        }
        // Use the geometry bounds, including the source height range used during morphing.
        const area = this.region(address);
        const distanceSq = pointAABBMaxDistanceSquared(this._localCameraPosition, area, node.minY, node.maxY);
        const start = this._lodMorphRanges[address.level + 1].start;
        return distanceSq > start * start;
    }

    public distanceSquared (address: NodeAddress): number {
        const bounds = this.heightRange(address);
        const area = this.region(address);
        return pointAABBMinDistanceSquared(this._localCameraPosition, area, bounds.minY, bounds.maxY);
    }

    // Pre-order traversal of valid nodes from the root, excluding out-of-bounds quadrants.
    // Returning false skips only this node's descendants; siblings are still visited.
    public traverse (visit: (node: TargetTreeNode) => boolean): void {
        this.traverseNode(this._root, visit);
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
        if (config.LANDSCAPE_DEBUG) {
            let message = '[Landscape] LOD ranges:';
            this._lodMorphRanges.forEach((range, level) => {
                message += ` [LOD${level}: ${range.start.toFixed(1)},${range.end.toFixed(1)}]`;
            });
            log(message);
        }
    }

    private buildRoot (node: MutableTargetTreeNode, address: NodeAddress): void {
        node.geometry = new QuadNode(address.level, address.x, address.z);
        if (!this.contains(address)) {
            return;
        }
        if (address.level === this._data.maxTileLevel) {
            node.geometry = this.geometry(address);
            return;
        }
        node.children = Array.from({ length: 4 }, () => new MutableTargetTreeNode());
        let minY = Number.MAX_VALUE;
        let maxY = -Number.MAX_VALUE;
        for (let q = 0; q < 4; ++q) {
            const child = address.child(q);
            this.buildRoot(node.children[q], child);
            if (this.contains(child)) {
                minY = Math.min(minY, node.children[q].geometry.minY);
                maxY = Math.max(maxY, node.children[q].geometry.maxY);
            }
        }
        node.geometry.minY = minY;
        node.geometry.maxY = maxY;
    }

    private updateTargetNode (node: MutableTargetTreeNode): void {
        const address = node.geometry.address();
        if (!this.contains(address)) {
            return;
        }
        let refine = address.level > this._data.maxTileLevel;
        if (!refine && address.level > 0) {
            const range = this._lodMorphRanges[address.level - 1].end;
            for (let q = 0; q < 4; ++q) {
                if (this.distanceSquared(address.child(q)) <= range * range) {
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
                const child = new MutableTargetTreeNode();
                child.geometry = this.geometry(address.child(q));
                return child;
            });
        }
        for (const child of node.children) {
            this.updateTargetNode(child);
        }
    }

    private findNode (address: NodeAddress): MutableTargetTreeNode | null {
        if (!this.contains(address)) {
            return null;
        }
        if (address.level > this._root.geometry.level) {
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

    public contains (address: NodeAddress): boolean {
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

    public heightRange (address: NodeAddress): HeightRange {
        if (address.level > this._data.maxTileLevel) {
            const bounds = this.find(address)!.geometry;
            return { minY: bounds.minY, maxY: bounds.maxY };
        }
        return this._heightRanges[address.level][address.z * this._data.nodesX(address.level) + address.x];
    }

    private traverseNode (node: MutableTargetTreeNode, visit: (node: MutableTargetTreeNode) => boolean): void {
        if (!this.contains(node.geometry.address())) {
            return;
        }
        if (!visit(node)) {
            return;
        }
        if (!node.children) {
            return;
        }
        for (const child of node.children) {
            this.traverseNode(child, visit);
        }
    }
}

export interface RenderTreeNode {
    readonly geometry: Readonly<QuadNode>;
    readonly material: ReadonlyVTGridMapping;
    readonly children: readonly RenderTreeNode[] | null;
    readonly edgeMask: number;
    readonly valid: boolean;
    readonly renderable: boolean;
}

class MutableRenderNode implements RenderTreeNode {
    public geometry = new QuadNode();
    public material = new VTGridMapping();
    public children: MutableRenderNode[] | null = null;
    public edgeMask = 0;
    public valid = false; // False for out-of-bounds quadrant placeholders.
    public renderable = false;
}

export interface RenderTreeChanges {
    added: NodeKey[];
    updated: NodeKey[];
    removed: NodeKey[];
}

// Borrowed ready mapping, or null while unavailable; select copies it into the snapshot.
type MaterialMappingQuery = (address: NodeAddress) => ReadonlyVTGridMapping | null;

// Committed render snapshot; owns its geometry and mappings independently of the target.
export class RenderTree {
    private _root: MutableRenderNode | null = null;
    // Rebuilt with each snapshot; nodes are owned by _root.
    private _renderableNodes = new Map<NodeKey, MutableRenderNode>();
    private _lastMorphRangeWarning = false;

    // Missing sector coverage leaves the previous snapshot intact.
    public select (target: TargetTree, queryMaterialMapping: MaterialMappingQuery): RenderTreeChanges {
        const changes: RenderTreeChanges = { added: [], updated: [], removed: [] };
        const data = target.data();
        for (let z = 0; z < data.sectorsZ; ++z) {
            for (let x = 0; x < data.sectorsX; ++x) {
                if (!queryMaterialMapping(new NodeAddress(data.maxTileLevel, x, z))) {
                    return changes;
                }
            }
        }
        const root = new MutableRenderNode();
        let current: MutableRenderNode[] = [];
        this.buildRoots(root, target.root(), target, queryMaterialMapping, current);
        while (current.length) {
            const next: MutableRenderNode[] = [];
            for (const node of current) {
                const desired = target.find(node.geometry.address())!;
                if (!desired.children) {
                    continue;
                }
                const mappings = desired.children.map((child) => queryMaterialMapping(child.geometry.address()));
                if (mappings.some((mapping) => mapping === null)) {
                    continue;
                }
                if ((this.computeEdgeMask(root, node.geometry.address(), data) & 0x55) !== 0) {
                    continue;
                }
                node.renderable = false;
                node.material = new VTGridMapping();
                node.children = desired.children.map((entry, q) => {
                    const child = new MutableRenderNode();
                    child.geometry = new QuadNode(entry.geometry.level, entry.geometry.ix, entry.geometry.iz, entry.geometry.minY, entry.geometry.maxY);
                    this.copyMapping(child.material, mappings[q]!);
                    child.valid = true;
                    child.renderable = true;
                    next.push(child);
                    return child;
                });
            }
            current = next;
        }
        const renderableNodes = new Map<NodeKey, MutableRenderNode>();
        this.traverseNode(root, (node) => {
            if (!node.renderable) {
                return true;
            }
            const address = node.geometry.address();
            node.edgeMask = this.computeEdgeMask(root, address, data);
            const previous = this.findRenderable(address);
            if (!previous) {
                changes.added.push(address.key());
            } else if (previous.edgeMask !== node.edgeMask || !VTGridMapping.equals(previous.material, node.material)) {
                changes.updated.push(address.key());
            }
            renderableNodes.set(address.key(), node);
            return false;
        });
        for (const key of this._renderableNodes.keys()) {
            if (!renderableNodes.has(key)) {
                changes.removed.push(key);
            }
        }
        this._root = root;
        this._renderableNodes = renderableNodes;
        this.checkMorphRanges(target);
        return changes;
    }

    public empty (): boolean {
        return this._root === null;
    }

    // Borrowed node; valid until the next committed selection.
    public findRenderable (address: NodeAddress): RenderTreeNode | null {
        return this._renderableNodes.get(address.key()) ?? null;
    }

    // Visits every renderable leaf in traversal order, without visibility filtering.
    public forEachRenderableNode (visit: (node: RenderTreeNode) => void): void {
        this.traverse((node) => {
            if (node.renderable) {
                visit(node);
            }
            return true;
        });
    }

    // Pre-order traversal of valid nodes from the root, including internal management nodes.
    // Returning false skips only this node's descendants; siblings are still visited.
    public traverse (visit: (node: RenderTreeNode) => boolean): void {
        if (this._root) {
            this.traverseNode(this._root, visit);
        }
    }

    // Visits renderable leaves whose geometry bounds intersect the frustum, in traversal order.
    // Passes each leaf's geometry and terrain-local region; internal nodes are only used for pruning.
    public forEachVisibleNode (target: TargetTree, frustum: Readonly<Frustum>, origin: Readonly<Vec3>, visit: (node: QuadNode, region: LandscapeRegion) => void): void {
        this.traverse((treeNode) => {
            const node = treeNode.geometry;
            const region = target.region(node.address());
            const bounds = new AABB(region.x + region.size * 0.5 + origin.x, (node.minY + node.maxY) * 0.5 + origin.y, region.z + region.size * 0.5 + origin.z, region.size * 0.5, (node.maxY - node.minY) * 0.5, region.size * 0.5);
            if (!intersect.aabbFrustum(bounds, frustum)) {
                return false;
            }
            if (treeNode.renderable) {
                visit(node, region);
            }
            return true;
        });
    }

    // Diagnose each committed selection regardless of frustum visibility.
    private checkMorphRanges (target: TargetTree): void {
        if (!config.LANDSCAPE_DEBUG) {
            return;
        }
        let morphRangeWarning = false;
        this.forEachRenderableNode((node) => {
            if (morphRangeWarning) {
                return;
            }
            morphRangeWarning = target.morphRangeTooSmall(node.geometry);
        });
        if (morphRangeWarning !== this._lastMorphRangeWarning) {
            if (morphRangeWarning) {
                warn('[Landscape] CDLOD granularity check: visibility ranges are too small for a guaranteed transition');
            } else {
                log('[Landscape] CDLOD granularity check: OK');
            }
            this._lastMorphRangeWarning = morphRangeWarning;
        }
    }

    private buildRoots (node: MutableRenderNode, target: TargetTreeNode, tree: TargetTree, queryMaterialMapping: MaterialMappingQuery, roots: MutableRenderNode[]): void {
        node.geometry = new QuadNode(target.geometry.level, target.geometry.ix, target.geometry.iz, target.geometry.minY, target.geometry.maxY);
        const address = node.geometry.address();
        node.valid = tree.contains(address);
        if (!node.valid) {
            return;
        }
        if (address.level === tree.data().maxTileLevel) {
            this.copyMapping(node.material, queryMaterialMapping(address)!);
            node.renderable = true;
            roots.push(node);
            return;
        }
        node.children = target.children!.map((entry) => {
            const child = new MutableRenderNode();
            this.buildRoots(child, entry, tree, queryMaterialMapping, roots);
            return child;
        });
    }

    private copyMapping (output: VTGridMapping, input: ReadonlyVTGridMapping): void {
        output.cells.set(input.cells);
        output.pages = input.pages.map((page) => new VTPageAddress(page.level, page.x, page.z));
    }

    private traverseNode (node: MutableRenderNode, visit: (node: MutableRenderNode) => boolean): void {
        if (!node.valid) {
            return;
        }
        if (!visit(node)) {
            return;
        }
        if (!node.children) {
            return;
        }

        for (const child of node.children) {
            this.traverseNode(child, visit);
        }
    }

    private renderableAt (root: MutableRenderNode, x: number, z: number): NodeAddress {
        let node = root;
        while (!node.renderable) {
            const shift = node.geometry.level - 1;
            node = node.children![((x >> shift) & 1) | (((z >> shift) & 1) << 1)];
        }
        return node.geometry.address();
    }

    private computeEdgeMask (root: MutableRenderNode, address: NodeAddress, data: LandscapeData): number {
        const width = data.sectorsX * 2 ** data.maxTileLevel;
        const depth = data.sectorsZ * 2 ** data.maxTileLevel;
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
            add(this.renderableAt(root, x - 1, z), 0);
        }
        if (x + size < width) {
            add(this.renderableAt(root, x + size, z), 1);
        }
        if (z > 0) {
            add(this.renderableAt(root, x, z - 1), 2);
        }
        if (z + size < depth) {
            add(this.renderableAt(root, x, z + size), 3);
        }
        return edges;
    }

}

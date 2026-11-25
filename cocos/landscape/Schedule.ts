// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { Frustum } from '../core/geometry';
import { Vec3 } from '../core/math';
import { assert } from '../core/platform/debug';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { VTRenderer, ResourceRequirements } from './VTRenderer';
import { TargetTree, RenderTree, RenderTreeChanges, MorphRange } from './Quadtree';
import { LandscapeRegion, QuadNode, NodeAddress, NodeKey, ReadonlyVTGridMapping, VTGridMapping, VTPageAddress, VTPageLayout } from './Data';
import * as config from './Config';

interface NodeResources {
    address: NodeAddress;
    current: VTGridMapping;
    pending: VTGridMapping;
}

export class LandscapeSchedule {
    private readonly _asset: LandscapeAsset;
    private readonly _residency: LandscapeResidency;
    private readonly _vtRenderer: VTRenderer;
    private readonly _targetTree = new TargetTree();
    private readonly _renderTree = new RenderTree();

    private _ready = false;
    private _destroyed = false;
    // Retry after view/selection changes, cancellation or publication, not capacity rejection.
    private _requestsDirty = true;
    private _renderDirty = true;
    // Owns resource lifetimes; RenderTree only owns immutable draw snapshots.
    private readonly _nodes = new Map<NodeKey, NodeResources>();
    constructor (asset: LandscapeAsset, residency: LandscapeResidency, vtRenderer: VTRenderer, lodQualityScale: number) {
        this._asset = asset;
        this._residency = residency;
        this._vtRenderer = vtRenderer;
        this._targetTree.init(asset, lodQualityScale);
    }

    public destroy (): void {
        this._destroyed = true;
        for (const node of this._nodes.values()) {
            this.release(node.address, node.current);
            this.release(node.address, node.pending);
        }
        this._nodes.clear();
    }

    // Frame updates.
    public update (camera: Readonly<Vec3>, origin: Readonly<Vec3>): void {
        if (this._targetTree.updateTarget(camera, origin)) {
            this._requestsDirty = true;
            this._renderDirty = true;
        }
        if (this._requestsDirty && this.prepareRequests()) {
            this._requestsDirty = false;
        }
    }

    public prepareRenderTree (): RenderTreeChanges {
        const published = this.prepareResources();
        let changes: RenderTreeChanges = { added: [], updated: [], removed: [] };
        if (this._renderDirty || published) {
            changes = this._renderTree.select(this._targetTree, (address) => this.queryMapping(address));
        }
        const renderChanged = changes.added.length > 0 || changes.updated.length > 0 || changes.removed.length > 0;
        this._requestsDirty = this._requestsDirty || published || renderChanged;
        this._renderDirty = this._renderTree.empty();
        return changes;
    }

    // Call after Renderer has synchronized models to the prepared tree.
    public finishUpdate (): void {
        // Retire resources only after models stop referring to the previous snapshot.
        let released = false;
        for (const [key, node] of this._nodes) {
            if (!this._targetTree.find(node.address) && !this._renderTree.findRenderable(node.address)) {
                this.release(node.address, node.current);
                this.release(node.address, node.pending);
                this._nodes.delete(key);
                released = true;
            }
        }
        this._requestsDirty = this._requestsDirty || released;
        this._ready = this._ready || this.isStable();
        if (EDITOR_NOT_IN_PREVIEW && this.needsUpdate()) {
            // Editor repaint requests must wait until its current tick has ended.
            void Promise.resolve().then(() => {
                if (!this._destroyed && this.needsUpdate()) {
                    globalThis.cce.Engine.repaintInEditMode();
                }
            });
        }
    }

    public renderTree (): Readonly<RenderTree> {
        return this._renderTree;
    }

    public lodMorphRanges (): readonly Readonly<MorphRange>[] {
        return this._targetTree.lodMorphRanges();
    }

    // Latched after the first stable render state.
    public isReady (): boolean {
        return this._ready;
    }

    // Current capacity-admitted requests are ready and draw selection is stable.
    public isStable (): boolean {
        if (this._requestsDirty || this._renderDirty) {
            return false;
        }
        if (Array.from(this._nodes.values()).some((node) => node.pending.pages.length > 0)) {
            return false;
        }
        if (!this._residency.ready() || !this._vtRenderer.ready()) {
            return false;
        }
        return !this._renderTree.empty();
    }

    // Repaint while admitted work or render selection is still pending.
    public needsUpdate (): boolean {
        return !this.isStable();
    }

    public forEachVisibleNode (frustum: Readonly<Frustum>, origin: Readonly<Vec3>, visit: (node: QuadNode, region: LandscapeRegion) => void): void {
        this._renderTree.forEachVisibleNode(this._targetTree, frustum, origin, visit);
    }

    // Resource requests.
    private queryMapping (address: NodeAddress): ReadonlyVTGridMapping | null {
        const node = this._nodes.get(address.key());
        return node && node.current.pages.length ? node.current : null;
    }

    private requests (): NodeAddress[] {
        const missing: NodeAddress[] = [];
        const refresh: NodeAddress[] = [];
        this._targetTree.traverse((node) => {
            const address = node.geometry.address();
            if (address.level > this._asset.data().maxTileLevel) {
                return true;
            }
            if (!this.queryMapping(address)) {
                missing.push(address);
                return false;
            }
            if (this._renderTree.findRenderable(address) || !node.children) {
                refresh.push(address);
            }
            return node.children !== null;
        });
        const priority = (a: NodeAddress, b: NodeAddress): number => a.level - b.level || a.x - b.x || a.z - b.z;
        missing.sort(priority);
        refresh.sort(priority);
        return missing.concat(refresh);
    }

    // Returns false while waiting for the admitted source batch; capacity rejection completes the scan.
    private prepareRequests (): boolean {
        const loaded = this._residency.ready();
        for (const [key, node] of this._nodes) {
            if (!this._targetTree.find(node.address)) {
                this.release(node.address, node.pending);
            }
            if (!node.current.pages.length && !node.pending.pages.length) {
                this._nodes.delete(key);
            }
        }
        // Keep an admitted source batch pinned until it loads; target cancellation is immediate.
        if (!loaded) {
            return false;
        }
        const resources = this.collectResources();
        const rootsReady = !this._renderTree.empty();
        const data = this._asset.data();
        const layout = new VTPageLayout(data.sectorSize);
        const capacity = this._vtRenderer.capacityQuery();
        for (const address of this.requests()) {
            const existing = this._nodes.get(address.key());
            if (existing && existing.pending.pages.length) {
                continue;
            }
            const current = this.queryMapping(address);
            if (!rootsReady && (current || address.level !== data.maxTileLevel)) {
                continue;
            }
            let pending = new VTGridMapping();
            if (!current && address.level === data.maxTileLevel) {
                pending.pages = [new VTPageAddress(layout.rootLevel(), address.x, address.z)];
            } else {
                const geometry = this._targetTree.find(address)!.geometry;
                pending = layout.buildGridMapping(data, address, geometry.minY, geometry.maxY, this._targetTree.localCameraPosition());
            }
            if (current && VTGridMapping.equals(current, pending)) {
                continue;
            }
            const mappingCount = this._nodes.size + (existing ? 0 : 1);
            if (!this._vtRenderer.pageTable().canFit(mappingCount)) {
                continue;
            }
            const source = address.ancestor(Math.max(address.level, data.minTileLevel));
            if (!capacity.canPrepare(pending.pages, [source], resources)) {
                continue;
            }
            this.appendResources(resources, address, pending);
            this.retain(address, pending);
            const node = existing ?? { address, current: new VTGridMapping(), pending: new VTGridMapping() };
            node.pending = pending;
            this._nodes.set(address.key(), node);
        }
        return true;
    }

    private collectResources (): ResourceRequirements {
        const resources: ResourceRequirements = { sources: new Map(), pages: new Map() };
        for (const node of this._nodes.values()) {
            this.appendResources(resources, node.address, node.current);
            this.appendResources(resources, node.address, node.pending);
        }
        return resources;
    }

    private appendResources (resources: ResourceRequirements, address: NodeAddress, mapping: ReadonlyVTGridMapping): void {
        if (!mapping.pages.length) {
            return;
        }
        const source = address.ancestor(Math.max(address.level, this._asset.data().minTileLevel));
        resources.sources.set(source.key(), source);
        for (const page of mapping.pages) {
            resources.pages.set(page.key(), page);
        }
    }

    private retain (address: NodeAddress, mapping: ReadonlyVTGridMapping): void {
        this._residency.request(address.ancestor(Math.max(address.level, this._asset.data().minTileLevel)));
        for (const page of mapping.pages) {
            this._vtRenderer.request(page);
        }
    }

    private release (address: NodeAddress, mapping: VTGridMapping): void {
        if (!mapping.pages.length) {
            return;
        }
        this._residency.release(address.ancestor(Math.max(address.level, this._asset.data().minTileLevel)));
        for (const page of mapping.pages) {
            this._vtRenderer.release(page);
        }
        mapping.pages = [];
        mapping.cells.fill(0);
    }

    // Resource preparation.
    // Returns whether a pending material mapping was published.
    private prepareResources (): boolean {
        if (!this._vtRenderer.pageTable().ensureCapacity(this._nodes.size)) {
            return false;
        }
        // VT submits source demand first; newly uploaded inputs compose on the next frame.
        const pagesUpdated = this._vtRenderer.update(config.VT_PAGE_UPDATE_BUDGET);
        assert(pagesUpdated, 'Landscape admitted VT requests must fit');
        const sourcesUpdated = this._residency.update(config.PAGE_UPLOAD_BUDGET);
        assert(sourcesUpdated, 'Landscape admitted sources must fit and load successfully');
        let published = false;
        // Releases below are applied by the next resource update, after model synchronization.
        // Parent levels precede children; publication never exposes partial mappings.
        const nodes = Array.from(this._nodes.values()).sort((a, b) => b.address.level - a.address.level || b.address.x - a.address.x || b.address.z - a.address.z);
        for (const node of nodes) {
            if (!node.pending.pages.length || this._residency.resolve(node.address).layer < 0) {
                continue;
            }
            if (node.pending.pages.some((page) => this._vtRenderer.needsComposition(page))) {
                continue;
            }
            assert(this._targetTree.find(node.address) !== null);
            assert(node.address.level === this._asset.data().maxTileLevel || this.queryMapping(node.address.ancestor(node.address.level + 1)) !== null);
            this.release(node.address, node.current);
            node.current = node.pending;
            node.pending = new VTGridMapping();
            published = true;
        }
        return published;
    }

}

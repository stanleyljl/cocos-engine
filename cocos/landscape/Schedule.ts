// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { error } from '../core/platform/debug';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { VTRenderer } from './VTRenderer';
import { Quadtree } from './Quadtree';
import { NodeAddress, NodeKey, VTGridMapping, VTPageAddress, VTPageLayout } from './Data';
import * as config from './Config';

interface PendingNode {
    node: NodeAddress;
    material: VTGridMapping;
}

interface Resources {
    sources: NodeAddress[];
    pages: VTPageAddress[];
}

function comparePages (a: VTPageAddress, b: VTPageAddress): number {
    return a.level - b.level || a.x - b.x || a.z - b.z;
}

function sameMapping (a: VTGridMapping, b: VTGridMapping): boolean {
    return a.pages.length === b.pages.length && a.cells.every((cell, i) => cell === b.cells[i])
        && a.pages.every((page, i) => page.key() === b.pages[i].key());
}

function sourcesFit (retained: ReadonlySet<NodeKey>, inputs: readonly NodeAddress[], capacity: number): boolean {
    return retained.size + inputs.filter((source) => !retained.has(source.key())).length <= capacity;
}

export class LandscapeSchedule {
    private _loadingStopped = false;
    private _lastUpdateFrame = -1;
    private _requestsDirty = true;
    private _pendingNodes: PendingNode[] = [];
    private _reservedResources: Resources = { sources: [], pages: [] };

    private readonly _asset: LandscapeAsset;
    private readonly _residency: LandscapeResidency;
    private readonly _vtRenderer: VTRenderer;
    private readonly _quadtree: Quadtree;

    constructor (_asset: LandscapeAsset, _residency: LandscapeResidency, _vtRenderer: VTRenderer, _quadtree: Quadtree) {
        this._asset = _asset;
        this._residency = _residency;
        this._vtRenderer = _vtRenderer;
        this._quadtree = _quadtree;
    }

    public isSettled (): boolean {
        return !this._loadingStopped && !this._asset.loadingFailed() && !this._requestsDirty && !this._pendingNodes.length
            && this._residency.ready() && this._vtRenderer.ready() && this._quadtree.sectorRootsReady() && !this._quadtree.renderSelectionChanged();
    }

    // The WebGL2 Scene view ticks on demand; failed loading must not keep it repainting.
    public needsUpdate (): boolean {
        return !this._loadingStopped && !this._asset.loadingFailed() && !this.isSettled();
    }

    public prepareResources (frame: number): void {
        if (this._loadingStopped || this._lastUpdateFrame === frame) {
            return;
        }
        this._lastUpdateFrame = frame;
        if (this._asset.loadingFailed()) {
            this.stopLoading();
            return;
        }
        this._residency.update(config.PAGE_UPLOAD_BUDGET);
        if (!this.prepareRequests() || !this.loadNodes()) {
            this.stopLoading();
            return;
        }
        this.composeNodes();
        if (!this.publishNodes()) {
            this.stopLoading();
        }
    }

    private prepareRequests (): boolean {
        this._requestsDirty = this._requestsDirty || this._quadtree.viewChanged() || this._quadtree.renderSelectionChanged();
        const previousCount = this._pendingNodes.length;
        this._pendingNodes = this._pendingNodes.filter((pending) => this._quadtree.find(pending.node));
        const cancelled = this._pendingNodes.length !== previousCount;
        this._requestsDirty = this._requestsDirty || cancelled;
        const loaded = this._residency.ready();
        if (!loaded && !cancelled) {
            return true;
        }
        const scan = this._requestsDirty && loaded;
        if (scan || cancelled) {
            const collected = this.collectResources();
            const rootsReady = this._quadtree.sectorRootsReady();
            const resources = scan ? this.admitNodes(collected.resources, collected.mappingCount, rootsReady) : collected.resources;
            if (!this._vtRenderer.reserve(resources.pages) || !this._residency.reserve(resources.sources)) {
                return false;
            }
            this._reservedResources = resources;
            if (scan) {
                this._requestsDirty = false;
                if (!rootsReady && !this._pendingNodes.length) {
                    return false;
                }
            }
        }
        return this._vtRenderer.ready() || this._residency.reserve(this.compositionSources());
    }

    private loadNodes (): boolean {
        if (!this._residency.ready() && !this._residency.loadReserved()) {
            return false;
        }
        for (const pending of this._pendingNodes) {
            const node = this._quadtree.find(pending.node)!;
            if (!node.loaded) {
                node.loaded = this._residency.resolve(pending.node).layer >= 0;
            }
        }
        return true;
    }

    private composeNodes (): void {
        if (this._vtRenderer.ready()) {
            return;
        }
        const eligible = new Set<NodeKey>();
        this._quadtree.forEachNode((node) => {
            if (node.ready) {
                node.material.pages.forEach((page) => eligible.add(page.key()));
            }
            return true;
        });
        for (const pending of this._pendingNodes) {
            if (this._quadtree.find(pending.node)!.loaded) {
                pending.material.pages.forEach((page) => eligible.add(page.key()));
            }
        }
        this._vtRenderer.renderPages(this._residency, this._reservedResources.pages.filter((page) => eligible.has(page.key())), config.VT_PAGE_UPDATE_BUDGET);
    }

    private publishNodes (): boolean {
        for (let i = 0; i < this._pendingNodes.length;) {
            const pending = this._pendingNodes[i];
            if (!this._quadtree.find(pending.node)!.loaded || pending.material.pages.some((page) => this._vtRenderer.needsComposition(page))) {
                ++i;
                continue;
            }
            if (!this._quadtree.publish(pending.node, pending.material)) {
                return false;
            }
            this._pendingNodes.splice(i, 1);
            this._requestsDirty = true;
        }
        return true;
    }

    private admitNodes (resources: Resources, mappingCount: number, rootsReady: boolean): Resources {
        const data = this._asset.data();
        const layout = new VTPageLayout(data.sectorSize);
        const pendingKeys = new Set(this._pendingNodes.map((pending) => pending.node.key()));
        for (const request of this._quadtree.requests()) {
            const node = this._quadtree.find(request)!;
            if (pendingKeys.has(request.key()) || (!rootsReady && (node.ready || request.level !== data.maxLevel))) {
                continue;
            }
            const pending = { node: request, material: new VTGridMapping() };
            if (!node.ready && request.level === data.maxLevel) {
                pending.material.pages = [new VTPageAddress(layout.rootLevel(), request.x, request.z)];
            } else {
                pending.material = layout.buildGridMapping(data.nodeRegion(request), node.geometry.minY, node.geometry.maxY, this._quadtree.localCameraPosition());
            }
            if (node.ready && sameMapping(node.material, pending.material)) {
                continue;
            }
            const candidate = { sources: resources.sources.slice(), pages: resources.pages.slice() };
            this.appendResources(candidate, pending);
            if (!this.canPrepare(candidate)) {
                continue;
            }
            const required = mappingCount + (node.ready ? 0 : 1);
            if (!this._vtRenderer.pageTable().ensureCapacity(required)) {
                continue;
            }
            resources = candidate;
            mappingCount = required;
            pendingKeys.add(request.key());
            this._pendingNodes.push(pending);
        }
        return resources;
    }

    private collectResources (): { resources: Resources; mappingCount: number } {
        const resources: Resources = { sources: [], pages: [] };
        let mappingCount = 0;
        const sourceKeys = new Set<NodeKey>();
        const pageKeys = new Set<NodeKey>();
        this._quadtree.forEachNode((node) => {
            if (!node.ready) {
                return true;
            }
            ++mappingCount;
            const address = node.geometry.address();
            const source = address.ancestor(Math.max(address.level, this._asset.data().minTileLevel));
            if (!sourceKeys.has(source.key())) {
                sourceKeys.add(source.key());
                resources.sources.push(source);
            }
            for (const page of node.material.pages) {
                if (!pageKeys.has(page.key())) {
                    pageKeys.add(page.key());
                    resources.pages.push(page);
                }
            }
            return true;
        });
        resources.pages.sort(comparePages);
        for (const pending of this._pendingNodes) {
            this.appendResources(resources, pending);
            if (!this._quadtree.find(pending.node)!.ready) {
                ++mappingCount;
            }
        }
        return { resources, mappingCount };
    }

    private appendResources (resources: Resources, pending: PendingNode): void {
        const source = pending.node.ancestor(Math.max(pending.node.level, this._asset.data().minTileLevel));
        if (!resources.sources.some((entry) => entry.key() === source.key())) {
            resources.sources.push(source);
        }
        for (const page of pending.material.pages) {
            if (!resources.pages.some((entry) => entry.key() === page.key())) {
                resources.pages.push(page);
            }
        }
        resources.pages.sort(comparePages);
    }

    private canPrepare (resources: Resources): boolean {
        if (!this._residency.canReserve(resources.sources) || !this._vtRenderer.canReserve(resources.pages)) {
            return false;
        }
        const retained = new Set(resources.sources.map((source) => source.key()));
        for (const page of resources.pages) {
            const inputs: NodeAddress[] = [];
            this._vtRenderer.selectSources(page, true).appendTo(inputs);
            if (!sourcesFit(retained, inputs, this._residency.layerCount())) {
                return false;
            }
        }
        return true;
    }

    private compositionSources (): NodeAddress[] {
        const sources = this._reservedResources.sources.slice();
        const keys = new Set(sources.map((source) => source.key()));
        for (const page of this._reservedResources.pages) {
            if (!this._vtRenderer.needsComposition(page)) {
                continue;
            }
            const inputs: NodeAddress[] = [];
            this._vtRenderer.selectSources(page, this._vtRenderer.getRefineFarDetail()).appendTo(inputs);
            if (!sourcesFit(keys, inputs, this._residency.layerCount())) {
                continue;
            }
            for (const source of inputs) {
                if (!keys.has(source.key())) {
                    keys.add(source.key());
                    sources.push(source);
                }
            }
        }
        return sources;
    }

    private stopLoading (): void {
        this._loadingStopped = true;
        error('[Landscape] Tile loading stopped; re-enable the landscape to reload.');
    }
}

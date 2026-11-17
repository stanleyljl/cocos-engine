// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { AABB, Frustum, intersect } from '../core/geometry';
import { Vec3, Vec4 } from '../core/math';
import type { Model } from '../render-scene/scene/model';
import { assert, log, warn } from '../core/platform/debug';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { VTRenderer } from './VTRenderer';
import { Quadtree } from './Quadtree';
import type { LandscapeRenderer, LandscapeRenderNode } from './Renderer';
import { LandscapeRegion, QuadNode, NodeAddress, NodeKey, ReadonlyVTGridMapping, VTGridMapping, VTPageAddress, VTPageLayout } from './Data';
import * as config from './Config';

interface Resources {
    sources: NodeAddress[];
    pages: VTPageAddress[];
    mappingCount: number;
}

interface PendingNode {
    node: NodeAddress;
    material: VTGridMapping;
}

interface Pass {
    planes: Vec4[];
    includeNearby: boolean;
}

function comparePages (a: VTPageAddress, b: VTPageAddress): number {
    return a.level - b.level || a.x - b.x || a.z - b.z;
}

function sameMapping (a: ReadonlyVTGridMapping, b: ReadonlyVTGridMapping): boolean {
    if (a.pages.length !== b.pages.length) {
        return false;
    }
    for (let i = 0; i < a.cells.length; ++i) {
        if (a.cells[i] !== b.cells[i]) {
            return false;
        }
    }
    return a.pages.every((page, i) => page.key() === b.pages[i].key());
}

export class LandscapeSchedule {
    private readonly _asset: LandscapeAsset;
    private readonly _residency: LandscapeResidency;
    private readonly _vtRenderer: VTRenderer;
    private readonly _renderer: LandscapeRenderer;
    private readonly _quadtree = new Quadtree();

    private _ready = false;
    private _destroyed = false;
    private _lastUpdateFrame = -1;
    // Retry after view/selection changes, cancellation or publication, not capacity rejection.
    private _requestsDirty = true;
    private _pendingNodes: PendingNode[] = [];
    // Admission snapshot; temporary composition inputs are owned by VT.
    private _requestedResources: Resources = { sources: [], pages: [], mappingCount: 0 };

    private readonly _passes: Pass[] = [];
    private _activePassCount = 0;
    private _lastMorphRangeWarning = false;

    constructor (asset: LandscapeAsset, renderer: LandscapeRenderer, lodQualityScale: number) {
        this._asset = asset;
        this._renderer = renderer;
        this._residency = renderer.residency();
        this._vtRenderer = renderer.vtRenderer();
        this._quadtree.init(asset, lodQualityScale);
        const morph = Array.from({ length: config.MAX_LOD_LEVELS }, () => new Vec4());
        this._quadtree.lodMorphRanges().forEach((range, i) => morph[i].set(range.start, range.end, 0, 0));
        renderer.setLodMorphRanges(morph);
        if (config.LANDSCAPE_DEBUG) {
            let message = '[Landscape] LOD ranges:';
            this._quadtree.lodMorphRanges().forEach((range, level) => {
                message += ` [LOD${level}: ${range.start.toFixed(1)},${range.end.toFixed(1)}]`;
            });
            log(message);
        }
    }

    public destroy (): void {
        this._destroyed = true;
        this.submitRequests({ sources: [], pages: [], mappingCount: 0 });
    }

    // Frame updates.
    public update (camera: Readonly<Vec3>, origin: Readonly<Vec3>): void {
        this._quadtree.updateTarget(camera, origin);
        this._renderer.setViewPosition(camera);
        this.prepareRequests();
    }

    public prepareRenderData (frame: number): void {
        const previousMappings = this._vtRenderer.pageTable().texture();
        this.prepareResources(frame);
        this._quadtree.selectRenderables();
        this._ready = this._ready || this.isRenderStateStable();
        const updated: LandscapeRenderNode[] = [];
        const removed: NodeKey[] = [];
        for (const key of this._quadtree.changedNodes()) {
            const node = this._quadtree.findRenderable(NodeAddress.fromKey(key));
            if (node) {
                updated.push(node);
            } else {
                removed.push(key);
            }
        }
        this._renderer.prepareRenderData(frame, previousMappings, updated, removed);
        this._quadtree.clearChangedNodes();
        if (EDITOR_NOT_IN_PREVIEW && this.needsUpdate()) {
            // Editor repaint requests must wait until its current tick has ended.
            void Promise.resolve().then(() => {
                if (!this._destroyed && this.needsUpdate()) {
                    globalThis.cce.Engine.repaintInEditMode();
                }
            });
        }
    }

    // Pass collection.
    public beginPasses (): void {
        this._activePassCount = 0;
    }

    public addPass (frustum: Readonly<Frustum>, includeNearby: boolean): void {
        if (this._activePassCount === this._passes.length) {
            this._passes.push({ planes: Array.from({ length: 6 }, () => new Vec4()), includeNearby: false });
        }
        const pass = this._passes[this._activePassCount++];
        pass.includeNearby = includeNearby;
        for (let i = 0; i < 6; ++i) {
            const plane = frustum.planes[i];
            pass.planes[i].set(plane.n.x, plane.n.y, plane.n.z, plane.d);
        }
    }

    public collectPassModels (frustum: Readonly<Frustum>, shadow: boolean, models: Model[]): void {
        this._quadtree.cull((node) => this._renderer.collectNodeModels(node.geometry, this._quadtree.region(node.geometry.address()), node.renderable, frustum, shadow, models));
    }

    public endPasses (): void {
        this._passes.length = this._activePassCount;
        this.checkMorphRanges();
    }

    // Latched after the first stable render state.
    public isReady (): boolean {
        return this._ready;
    }

    // Current capacity-admitted requests are ready and draw selection is stable.
    public isRenderStateStable (): boolean {
        return !this._requestsDirty && !this._pendingNodes.length
            && this._residency.ready() && this._vtRenderer.ready() && this._quadtree.sectorRootsReady() && !this._quadtree.renderSelectionChanged();
    }

    // Repaint while admitted work or render selection is still pending.
    public needsUpdate (): boolean {
        return !this.isRenderStateStable();
    }

    public visitDebugNodes (frustum: Readonly<Frustum>, origin: Readonly<Vec3>, visit: (node: QuadNode, region: LandscapeRegion) => void): void {
        this._quadtree.cull((treeNode) => {
            const node = treeNode.geometry;
            const region = this._quadtree.region(node.address());
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

    // Resource requests.
    private prepareRequests (): void {
        this._requestsDirty = this._requestsDirty || this._quadtree.viewChanged() || this._quadtree.renderSelectionChanged();
        const previousCount = this._pendingNodes.length;
        this._pendingNodes = this._pendingNodes.filter((pending) => this._quadtree.isInTargetTree(pending.node));
        const cancelled = this._pendingNodes.length !== previousCount;
        this._requestsDirty = this._requestsDirty || cancelled;
        const loaded = this._residency.ready();
        if (!loaded && !cancelled) {
            return;
        }
        const scan = this._requestsDirty && loaded;
        if (scan || cancelled) {
            const collected = this.collectResources();
            const rootsReady = this._quadtree.sectorRootsReady();
            const resources = scan ? this.schedulePendingNodes(collected, rootsReady) : collected;
            this.submitRequests(resources);
            if (scan) {
                this._requestsDirty = false;
            }
        }
    }

    private collectResources (): Resources {
        const resources: Resources = { sources: [], pages: [], mappingCount: 0 };
        const sourceKeys = new Set<NodeKey>();
        const pageKeys = new Set<NodeKey>();
        this._quadtree.forEachNode((node) => {
            if (!node.ready()) {
                return true;
            }
            ++resources.mappingCount;
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
            if (!this._quadtree.find(pending.node)!.ready()) {
                ++resources.mappingCount;
            }
        }
        return resources;
    }

    private schedulePendingNodes (resources: Resources, rootsReady: boolean): Resources {
        const capacity = this._vtRenderer.capacityQuery();
        const data = this._asset.data();
        const layout = new VTPageLayout(data.sectorSize);
        const pendingKeys = new Set(this._pendingNodes.map((pending) => pending.node.key()));
        for (const request of this._quadtree.requests()) {
            const node = this._quadtree.find(request)!;
            if (pendingKeys.has(request.key()) || (!rootsReady && (node.ready() || request.level !== data.maxTileLevel))) {
                continue;
            }
            const pending = { node: request, material: new VTGridMapping() };
            if (!node.ready() && request.level === data.maxTileLevel) {
                pending.material.pages = [new VTPageAddress(layout.rootLevel(), request.x, request.z)];
            } else {
                pending.material = layout.buildGridMapping(data, request, node.geometry.minY, node.geometry.maxY, this._quadtree.localCameraPosition());
            }
            if (node.ready() && sameMapping(node.material, pending.material)) {
                continue;
            }
            const candidate = { sources: resources.sources.slice(), pages: resources.pages.slice(), mappingCount: resources.mappingCount + (node.ready() ? 0 : 1) };
            this.appendResources(candidate, pending);
            if (!capacity.canPrepare(candidate.pages, candidate.sources)) {
                continue;
            }
            if (!this._vtRenderer.pageTable().canFit(candidate.mappingCount)) {
                continue;
            }
            resources = candidate;
            pendingKeys.add(request.key());
            this._pendingNodes.push(pending);
        }
        return resources;
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

    private submitRequests (resources: Resources): void {
        const sourceKeys = new Set(resources.sources.map((source) => source.key()));
        const pageKeys = new Set(resources.pages.map((page) => page.key()));
        for (const source of this._requestedResources.sources) {
            if (!sourceKeys.delete(source.key())) {
                this._residency.release(source);
            }
        }
        for (const page of this._requestedResources.pages) {
            if (!pageKeys.delete(page.key())) {
                this._vtRenderer.release(page);
            }
        }
        for (const source of resources.sources) {
            if (sourceKeys.has(source.key())) {
                this._residency.request(source);
            }
        }
        for (const page of resources.pages) {
            if (pageKeys.has(page.key())) {
                this._vtRenderer.request(page);
            }
        }
        this._requestedResources = resources;
    }

    // Resource preparation.
    private prepareResources (frame: number): void {
        if (this._lastUpdateFrame === frame) {
            return;
        }
        this._lastUpdateFrame = frame;
        if (!this._vtRenderer.pageTable().ensureCapacity(this._requestedResources.mappingCount)) {
            return;
        }
        // VT submits source demand first; newly uploaded inputs compose on the next frame.
        const pagesUpdated = this._vtRenderer.update(config.VT_PAGE_UPDATE_BUDGET);
        assert(pagesUpdated, 'Landscape admitted VT requests must fit');
        const sourcesUpdated = this._residency.update(config.PAGE_UPLOAD_BUDGET);
        assert(sourcesUpdated, 'Landscape admitted sources must fit and load successfully');
        this.publishNodes();
        // Selection can retire retained descendants after this stage; release their demands next tick.
        this._requestsDirty = this._requestsDirty || this._quadtree.viewChanged();
    }

    private publishNodes (): void {
        for (let i = 0; i < this._pendingNodes.length;) {
            const pending = this._pendingNodes[i];
            if (this._residency.resolve(pending.node).layer < 0 || pending.material.pages.some((page) => this._vtRenderer.needsComposition(page))) {
                ++i;
                continue;
            }
            const published = this._quadtree.publish(pending.node, pending.material);
            assert(published, 'Landscape ready nodes must satisfy publication requirements');
            this._pendingNodes.splice(i, 1);
            this._requestsDirty = true;
        }
    }

    // Morph diagnostics.
    private checkMorphRanges (): void {
        if (!config.LANDSCAPE_DEBUG || !this._quadtree.sectorRootsReady()) {
            return;
        }
        let morphRangeWarning = false;
        this._quadtree.forEachRenderable((node) => {
            if (!morphRangeWarning) {
                morphRangeWarning = this._quadtree.morphRangeTooSmall(node.geometry) && this.isMorphCheckVisible(node.geometry.address());
            }
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

    private isMorphCheckVisible (address: NodeAddress): boolean {
        const ranges = this._quadtree.lodMorphRanges();
        const distance = this._quadtree.distanceSquared(address);
        const range = ranges[address.level].end;
        if (distance > range * range) {
            return false;
        }
        const data = this._asset.data();
        const area = this._quadtree.region(address);
        const bounds = this._asset.heightRanges()[address.level][address.z * data.nodesX(address.level) + address.x];
        const origin = this._quadtree.worldOrigin();
        const center = new Vec3(area.x + area.size * 0.5 + origin.x, (bounds.minY + bounds.maxY) * 0.5 + origin.y, area.z + area.size * 0.5 + origin.z);
        const extent = new Vec3(area.size * 0.5, (bounds.maxY - bounds.minY) * 0.5, area.size * 0.5);
        const nearby = ranges[Math.min(1, data.maxTileLevel)].end;
        return this._passes.some((pass) => (pass.includeNearby && distance <= nearby * nearby) || pass.planes.every((plane) => {
            const radius = extent.x * Math.abs(plane.x) + extent.y * Math.abs(plane.y) + extent.z * Math.abs(plane.z);
            return center.x * plane.x + center.y * plane.y + center.z * plane.z + radius >= plane.w;
        }));
    }
}

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { AABB, Frustum, intersect } from '../core/geometry';
import { Vec3, Vec4 } from '../core/math';
import type { Model } from '../render-scene/scene/model';
import { error, log, warn } from '../core/platform/debug';
import { LandscapeAsset } from './Asset';
import { LandscapeResidency } from './Residency';
import { VTRenderer } from './VTRenderer';
import { Quadtree } from './Quadtree';
import type { LandscapeRenderer, LandscapeRenderNode } from './Renderer';
import { LandscapeRegion, QuadNode, NodeAddress, NodeKey, ReadonlyVTGridMapping, VTGridMapping, VTPageAddress, VTPageLayout } from './Data';
import * as config from './Config';

interface Pass {
    planes: Vec4[];
    includeNearby: boolean;
}

interface PendingNode {
    node: NodeAddress;
    material: VTGridMapping;
}

interface Resources {
    sources: NodeAddress[];
    pages: VTPageAddress[];
    mappingCount: number;
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

function sourcesFit (retained: ReadonlySet<NodeKey>, inputs: readonly NodeAddress[], capacity: number): boolean {
    return retained.size + inputs.filter((source) => !retained.has(source.key())).length <= capacity;
}

export class LandscapeSchedule {
    private readonly _passes: Pass[] = [];
    private _activePassCount = 0;
    // Camera, committed selection and pass changes invalidate diagnostics.
    private _needsMorphCheck = true;
    private _lastMorphRangeWarning = false;
    private _ready = false;
    private _destroyed = false;
    private _loadingStopped = false;
    private _lastUpdateFrame = -1;
    private _requestsDirty = true;
    private _pendingNodes: PendingNode[] = [];
    private _reservedResources: Resources = { sources: [], pages: [], mappingCount: 0 };

    private readonly _asset: LandscapeAsset;
    private readonly _residency: LandscapeResidency;
    private readonly _vtRenderer: VTRenderer;
    private readonly _quadtree = new Quadtree();
    private readonly _renderer: LandscapeRenderer;

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
    }

    public isReady (): boolean {
        return this._ready;
    }

    public beginPasses (): void {
        this._activePassCount = 0;
    }

    public addPass (frustum: Readonly<Frustum>, includeNearby: boolean): void {
        if (this._activePassCount === this._passes.length) {
            this._passes.push({ planes: Array.from({ length: 6 }, () => new Vec4()), includeNearby: false });
            this._needsMorphCheck = true;
        }
        const pass = this._passes[this._activePassCount++];
        this._needsMorphCheck = this._needsMorphCheck || pass.includeNearby !== includeNearby;
        pass.includeNearby = includeNearby;
        for (let i = 0; i < 6; ++i) {
            const plane = frustum.planes[i];
            const saved = pass.planes[i];
            this._needsMorphCheck = this._needsMorphCheck || saved.x !== plane.n.x || saved.y !== plane.n.y || saved.z !== plane.n.z || saved.w !== plane.d;
            saved.set(plane.n.x, plane.n.y, plane.n.z, plane.d);
        }
    }

    public endPasses (): void {
        this._needsMorphCheck = this._needsMorphCheck || this._activePassCount !== this._passes.length;
        this._passes.length = this._activePassCount;
        this.checkMorphRanges();
    }

    private checkMorphRanges (): void {
        if (!config.LANDSCAPE_DEBUG || !this._needsMorphCheck || !this._quadtree.sectorRootsReady()) {
            return;
        }
        let morphRangeWarning = false;
        this._quadtree.forEachRenderable((node) => {
            if (!morphRangeWarning) {
                morphRangeWarning = this.morphRangeTooSmall(node.geometry);
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
        this._needsMorphCheck = false;
    }

    public prepareRenderData (frame: number): void {
        const previousMappings = this._vtRenderer.pageTable().texture();
        this.prepareResources(frame);
        this._quadtree.selectRenderables();
        this._needsMorphCheck = this._needsMorphCheck || this._quadtree.renderSelectionChanged();
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

    public collectPassModels (frustum: Readonly<Frustum>, shadow: boolean, models: Model[]): void {
        this._quadtree.cull((node) => this._renderer.collectNodeModels(node.geometry, this._quadtree.region(node.geometry.address()), node.renderable, frustum, shadow, models));
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

    // Current capacity-admitted requests are ready and draw selection is stable.
    public isRenderStateStable (): boolean {
        return !this._loadingStopped && !this._asset.loadingFailed() && !this._requestsDirty && !this._pendingNodes.length
            && this._residency.ready() && this._vtRenderer.ready() && this._quadtree.sectorRootsReady() && !this._quadtree.renderSelectionChanged();
    }

    // The WebGL2 Scene view ticks on demand; failed loading must not keep it repainting.
    public needsUpdate (): boolean {
        return !this._loadingStopped && !this._asset.loadingFailed() && !this.isRenderStateStable();
    }

    public update (camera: Readonly<Vec3>, origin: Readonly<Vec3>): void {
        this._quadtree.updateTarget(camera, origin);
        this._needsMorphCheck = this._needsMorphCheck || this._quadtree.viewChanged();
        this._renderer.setViewPosition(camera);
        if (this._loadingStopped) {
            return;
        }
        if (this._asset.loadingFailed() || !this.prepareRequests() || !this._residency.loadReserved()) {
            this.stopLoading();
        }
    }

    private prepareResources (frame: number): void {
        if (this._loadingStopped || this._lastUpdateFrame === frame) {
            return;
        }
        this._lastUpdateFrame = frame;
        if (this._asset.loadingFailed()) {
            this.stopLoading();
            return;
        }
        if (!this._vtRenderer.pageTable().ensureCapacity(this._reservedResources.mappingCount)) {
            return;
        }
        this._residency.update(config.PAGE_UPLOAD_BUDGET);
        this.composeNodes();
        if (!this.publishNodes()) {
            this.stopLoading();
        }
        // Selection can retire retained descendants after this stage; release their reservations next tick.
        this._requestsDirty = this._requestsDirty || this._quadtree.viewChanged();
    }

    private prepareRequests (): boolean {
        this._requestsDirty = this._requestsDirty || this._quadtree.viewChanged() || this._quadtree.renderSelectionChanged();
        const previousCount = this._pendingNodes.length;
        this._pendingNodes = this._pendingNodes.filter((pending) => this._quadtree.isTarget(pending.node));
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
            const resources = scan ? this.schedulePendingNodes(collected, rootsReady) : collected;
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
            if (this._residency.resolve(pending.node).layer >= 0) {
                pending.material.pages.forEach((page) => eligible.add(page.key()));
            }
        }
        this._vtRenderer.renderPages(this._residency, this._reservedResources.pages.filter((page) => eligible.has(page.key())), config.VT_PAGE_UPDATE_BUDGET);
    }

    private publishNodes (): boolean {
        for (let i = 0; i < this._pendingNodes.length;) {
            const pending = this._pendingNodes[i];
            if (this._residency.resolve(pending.node).layer < 0 || pending.material.pages.some((page) => this._vtRenderer.needsComposition(page))) {
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

    private schedulePendingNodes (resources: Resources, rootsReady: boolean): Resources {
        // Worst-case source lists are fixed within this admission pass; capacity checks are not.
        const pageSources = new Map<NodeKey, NodeAddress[]>();
        const data = this._asset.data();
        const layout = new VTPageLayout(data.sectorSize);
        const pendingKeys = new Set(this._pendingNodes.map((pending) => pending.node.key()));
        for (const request of this._quadtree.requests()) {
            const node = this._quadtree.find(request)!;
            if (pendingKeys.has(request.key()) || (!rootsReady && (node.ready || request.level !== data.maxTileLevel))) {
                continue;
            }
            const pending = { node: request, material: new VTGridMapping() };
            if (!node.ready && request.level === data.maxTileLevel) {
                pending.material.pages = [new VTPageAddress(layout.rootLevel(), request.x, request.z)];
            } else {
                pending.material = layout.buildGridMapping(data, request, node.geometry.minY, node.geometry.maxY, this._quadtree.localCameraPosition());
            }
            if (node.ready && sameMapping(node.material, pending.material)) {
                continue;
            }
            const candidate = { sources: resources.sources.slice(), pages: resources.pages.slice(), mappingCount: resources.mappingCount + (node.ready ? 0 : 1) };
            this.appendResources(candidate, pending);
            if (!this.canPrepare(candidate, pageSources)) {
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

    private collectResources (): Resources {
        const resources: Resources = { sources: [], pages: [], mappingCount: 0 };
        const sourceKeys = new Set<NodeKey>();
        const pageKeys = new Set<NodeKey>();
        this._quadtree.forEachNode((node) => {
            if (!node.ready) {
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
            if (!this._quadtree.find(pending.node)!.ready) {
                ++resources.mappingCount;
            }
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

    private canPrepare (resources: Resources, pageSources: Map<NodeKey, NodeAddress[]>): boolean {
        if (!this._residency.canReserve(resources.sources) || !this._vtRenderer.canReserve(resources.pages)) {
            return false;
        }
        const retained = new Set(resources.sources.map((source) => source.key()));
        for (const page of resources.pages) {
            let inputs = pageSources.get(page.key());
            if (!inputs) {
                inputs = [];
                this._vtRenderer.selectSources(page, true, true).appendTo(inputs);
                pageSources.set(page.key(), inputs);
            }
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
            this._vtRenderer.selectSources(page, this._vtRenderer.getRefineFarDetail(), this._vtRenderer.getTriplanarEnabled()).appendTo(inputs);
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

    private morphRangeTooSmall (node: QuadNode): boolean {
        const address = node.address();
        const data = this._asset.data();
        const ranges = this._quadtree.lodMorphRanges();
        const origin = this._quadtree.worldOrigin();
        if (address.level === data.maxTileLevel) {
            return false;
        }
        const area = this._quadtree.region(address);
        const camera = this._quadtree.localCameraPosition();
        const x = Math.max(Math.abs(area.x - camera.x), Math.abs(area.x + area.size - camera.x));
        const y = Math.max(Math.abs(node.minY - camera.y), Math.abs(node.maxY - camera.y));
        const z = Math.max(Math.abs(area.z - camera.z), Math.abs(area.z + area.size - camera.z));
        const start = ranges[address.level + 1].start;
        if (x * x + y * y + z * z <= start * start) {
            return false;
        }
        const bounds = this._asset.heightRanges()[address.level][address.z * data.nodesX(address.level) + address.x];
        const dx = Math.max(area.x - camera.x, camera.x - area.x - area.size, 0);
        const dy = Math.max(bounds.minY - camera.y, camera.y - bounds.maxY, 0);
        const dz = Math.max(area.z - camera.z, camera.z - area.z - area.size, 0);
        const distance = dx * dx + dy * dy + dz * dz;
        const range = ranges[address.level].end;
        if (distance > range * range) {
            return false;
        }
        const center = new Vec3(area.x + area.size * 0.5 + origin.x, (bounds.minY + bounds.maxY) * 0.5 + origin.y, area.z + area.size * 0.5 + origin.z);
        const extent = new Vec3(area.size * 0.5, (bounds.maxY - bounds.minY) * 0.5, area.size * 0.5);
        const nearby = ranges[Math.min(1, data.maxTileLevel)].end;
        return this._passes.some((pass) => (pass.includeNearby && distance <= nearby * nearby) || pass.planes.every((plane) => {
            const radius = extent.x * Math.abs(plane.x) + extent.y * Math.abs(plane.y) + extent.z * Math.abs(plane.z);
            return center.x * plane.x + center.y * plane.y + center.z * plane.z + radius >= plane.w;
        }));
    }

    private stopLoading (): void {
        this._loadingStopped = true;
        error('[Landscape] Tile loading stopped; re-enable the landscape to reload.');
    }
}

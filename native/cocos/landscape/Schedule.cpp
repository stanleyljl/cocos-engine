// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Schedule.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "base/Log.h"
#include "base/StringUtil.h"
#include "core/geometry/AABB.h"
#include "core/geometry/Frustum.h"
#include "landscape/Renderer.h"
#include "base/std/container/unordered_set.h"
#include "landscape/Asset.h"
#include "landscape/Quadtree.h"
#include "landscape/Residency.h"
#include "landscape/VTRenderer.h"

namespace cc::landscape {
namespace {
bool sameMapping(const VTGridMapping &a, const VTGridMapping &b) {
    if (a.cells != b.cells) {
        return false;
    }
    if (a.pages.size() != b.pages.size()) {
        return false;
    }
    return std::equal(a.pages.begin(), a.pages.end(), b.pages.begin(), [](VTPageAddress left, VTPageAddress right) {
        return left.key() == right.key();
    });
}

bool sourcesFit(const ccstd::unordered_set<uint64_t> &retained, const ccstd::vector<NodeAddress> &inputs, uint32_t capacity) {
    const auto additional = std::count_if(inputs.begin(), inputs.end(), [&](NodeAddress source) {
        return retained.count(source.key()) == 0;
    });
    return retained.size() + additional <= capacity;
}
} // namespace

LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, LandscapeRenderer &renderer, float lodQualityScale)
: _asset(asset), _residency(renderer.residency()), _vtRenderer(renderer.vtRenderer()), _renderer(renderer), _quadtree(std::make_unique<Quadtree>()) {
    _quadtree->init(asset, lodQualityScale);
    ccstd::vector<Vec4> morph(config::MAX_LOD_LEVELS);
    const auto &ranges = _quadtree->lodMorphRanges();
    for (size_t i = 0; i < std::min(ranges.size(), morph.size()); ++i) {
        morph[i] = Vec4{ranges[i].start, ranges[i].end, 0, 0};
    }
    renderer.setLodMorphRanges(morph);
#if CC_LANDSCAPE_DEBUG
    ccstd::string message = "[Landscape] LOD ranges:";
    for (size_t level = 0; level < ranges.size(); ++level) {
        message += StringUtil::format(" [LOD%u: %.1f,%.1f]", static_cast<uint32_t>(level), ranges[level].start, ranges[level].end);
    }
    CC_LOG_INFO("%s", message.c_str());
#endif
}

LandscapeSchedule::~LandscapeSchedule() = default;

void LandscapeSchedule::beginPasses() {
    _activePassCount = 0;
}

void LandscapeSchedule::addPass(const geometry::Frustum &frustum, bool includeNearby) {
    if (_activePassCount == _passes.size()) {
        _passes.emplace_back();
        _needsMorphCheck = true;
    }
    auto &pass = _passes[_activePassCount++];
    _needsMorphCheck |= pass.includeNearby != includeNearby;
    pass.includeNearby = includeNearby;

    for (size_t i = 0; i < pass.planes.size(); ++i) {
        const auto &plane = *frustum.planes[i];
        const Vec4 value{plane.n.x, plane.n.y, plane.n.z, plane.d};
        _needsMorphCheck |= pass.planes[i] != value;
        pass.planes[i] = value;
    }
}

void LandscapeSchedule::endPasses() {
    _needsMorphCheck |= _activePassCount != _passes.size();
    _passes.resize(_activePassCount);
    checkMorphRanges();
}

void LandscapeSchedule::checkMorphRanges() {
#if CC_LANDSCAPE_DEBUG
    if (!_needsMorphCheck || !_quadtree->sectorRootsReady()) {
        return;
    }
    bool morphRangeWarning = false;
    _quadtree->forEachRenderable([&](const Quadtree::Node &node) {
        if (!morphRangeWarning) {
            morphRangeWarning = morphRangeTooSmall(node.geometry);
        }
    });
    if (morphRangeWarning != _lastMorphRangeWarning) {
        if (morphRangeWarning) {
            CC_LOG_WARNING("[Landscape] CDLOD granularity check: visibility ranges are too small for a guaranteed transition");
        } else {
            CC_LOG_INFO("[Landscape] CDLOD granularity check: OK");
        }
        _lastMorphRangeWarning = morphRangeWarning;
    }
    _needsMorphCheck = false;
#endif
}

void LandscapeSchedule::prepareRenderData(uint32_t frame) {
    auto *previousMappings = _vtRenderer.pageTable().texture();
    prepareResources(frame);
    _quadtree->selectRenderables();
    _needsMorphCheck |= _quadtree->renderSelectionChanged();
    _ready = _ready || isRenderStateStable();
    ccstd::vector<LandscapeRenderNode> updated;
    ccstd::vector<uint64_t> removed;
    for (const auto key : _quadtree->changedNodes()) {
        const auto *node = _quadtree->findRenderable(NodeAddress::fromKey(key));
        if (node) {
            updated.push_back({&node->geometry, &node->material, node->edgeMask});
        } else {
            removed.push_back(key);
        }
    }
    _renderer.prepareRenderData(frame, previousMappings, updated, removed);
    _quadtree->clearChangedNodes();
}

void LandscapeSchedule::collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const {
    _quadtree->cull([&](const Quadtree::Node &node) {
        return _renderer.collectNodeModels(node.geometry, _quadtree->region(node.geometry.address()), node.renderable, frustum, shadow, models);
    });
}

void LandscapeSchedule::visitDebugNodes(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const {
    _quadtree->cull([&](const Quadtree::Node &treeNode) {
        const auto &node = treeNode.geometry;
        const auto region = _quadtree->region(node.address());
        const geometry::AABB bounds{region.x + region.size * 0.5F + origin.x, (node.minY + node.maxY) * 0.5F + origin.y, region.z + region.size * 0.5F + origin.z, region.size * 0.5F, (node.maxY - node.minY) * 0.5F, region.size * 0.5F};
        if (!bounds.aabbFrustum(frustum)) {
            return false;
        }
        if (treeNode.renderable) {
            visit(node, region);
        }
        return true;
    });
}

bool LandscapeSchedule::isRenderStateStable() const {
    if (_loadingStopped) {
        return false;
    }
    if (_asset.loadingFailed()) {
        return false;
    }
    if (_requestsDirty) {
        return false;
    }
    if (!_pendingNodes.empty()) {
        return false;
    }
    if (!_residency.ready()) {
        return false;
    }
    if (!_vtRenderer.ready()) {
        return false;
    }
    if (!_quadtree->sectorRootsReady()) {
        return false;
    }
    // Selection runs after preparation and may expose another level of requests.
    return !_quadtree->renderSelectionChanged();
}

void LandscapeSchedule::update(const Vec3 &camera, const Vec3 &origin) {
    _quadtree->updateTarget(camera, origin);
    _needsMorphCheck |= _quadtree->viewChanged();
    _renderer.setViewPosition(camera);
    if (_loadingStopped) {
        return;
    }
    if (_asset.loadingFailed() || !prepareRequests() || !_residency.loadReserved()) {
        stopLoading();
    }
}

void LandscapeSchedule::prepareResources(uint32_t frame) {
    if (_loadingStopped) {
        return;
    }
    if (_lastUpdateFrame && *_lastUpdateFrame == frame) {
        return;
    }
    _lastUpdateFrame = frame;
    if (_asset.loadingFailed()) {
        stopLoading();
        return;
    }

    if (!_vtRenderer.pageTable().ensureCapacity(_reservedResources.mappingCount)) {
        return;
    }
    _residency.update(config::PAGE_UPLOAD_BUDGET);
    composeNodes();
    if (!publishNodes()) {
        stopLoading();
    }
    // Selection can retire retained descendants after this stage; release their reservations next tick.
    _requestsDirty |= _quadtree->viewChanged();
}

bool LandscapeSchedule::prepareRequests() {
    _requestsDirty |= _quadtree->viewChanged() || _quadtree->renderSelectionChanged();
    const auto previousPendingCount = _pendingNodes.size();
    _pendingNodes.erase(std::remove_if(_pendingNodes.begin(), _pendingNodes.end(), [&](const PendingNode &pending) {
        return !_quadtree->isTarget(pending.node);
    }), _pendingNodes.end());
    const bool requestsCancelled = _pendingNodes.size() != previousPendingCount;
    _requestsDirty |= requestsCancelled;

    // Keep the input batch pinned while loading; only cancellation replaces it early.
    const bool sourceBatchLoaded = _residency.ready();
    if (!sourceBatchLoaded && !requestsCancelled) {
        return true;
    }
    const bool scanRequests = _requestsDirty && sourceBatchLoaded;
    if (scanRequests || requestsCancelled) {
        auto resources = collectResources();
        const bool sectorRootsReady = _quadtree->sectorRootsReady();
        if (scanRequests) {
            schedulePendingNodes(resources, sectorRootsReady);
        }

        // Commit the base reservation before adding temporary composition inputs.
        // Cancellation can shrink it while loading, but never admits new requests.
        if (!_vtRenderer.reserve(resources.pages)) {
            return false;
        }
        if (!_residency.reserve(resources.sources)) {
            return false;
        }
        _reservedResources = std::move(resources);
        if (scanRequests) {
            _requestsDirty = false;
            if (!sectorRootsReady && _pendingNodes.empty()) {
                return false;
            }
        }
    }

    // Shared VT inputs use the remaining source capacity; node sources stay pinned.
    if (_vtRenderer.ready()) {
        return true;
    }
    return _residency.reserve(compositionSources());
}

void LandscapeSchedule::composeNodes() {
    if (_vtRenderer.ready()) {
        return;
    }
    ccstd::unordered_set<uint64_t> eligiblePageKeys;
    _quadtree->forEachNode([&](const Quadtree::Node &node) {
        // Ready pages may have been invalidated by material or decal changes.
        if (node.ready) {
            for (const auto page : node.material.pages) {
                eligiblePageKeys.insert(page.key());
            }
        }
        return true;
    });
    for (const auto &pending : _pendingNodes) {
        if (_residency.resolve(pending.node).layer < 0) {
            continue;
        }
        for (const auto page : pending.material.pages) {
            eligiblePageKeys.insert(page.key());
        }
    }
    ccstd::vector<VTPageAddress> pages;
    for (const auto page : _reservedResources.pages) {
        if (eligiblePageKeys.count(page.key())) {
            pages.push_back(page);
        }
    }
    _vtRenderer.renderPages(_residency, pages, config::VT_PAGE_UPDATE_BUDGET);
}

bool LandscapeSchedule::publishNodes() {
    for (auto it = _pendingNodes.begin(); it != _pendingNodes.end();) {
        if (_residency.resolve(it->node).layer < 0) {
            ++it;
            continue;
        }
        const bool pagesReady = std::all_of(it->material.pages.begin(), it->material.pages.end(), [&](VTPageAddress page) {
            return !_vtRenderer.needsComposition(page);
        });
        if (!pagesReady) {
            ++it;
            continue;
        }
        if (!_quadtree->publish(it->node, std::move(it->material))) {
            return false;
        }
        it = _pendingNodes.erase(it);
        _requestsDirty = true;
    }
    return true;
}

void LandscapeSchedule::schedulePendingNodes(Resources &resources, bool sectorRootsReady) {
    const auto &data = _asset.data();
    const VTPageLayout layout(data.sectorSize);
    // Worst-case source lists are fixed within this admission pass; capacity checks are not.
    ccstd::unordered_map<uint64_t, ccstd::vector<NodeAddress>> pageSources;
    ccstd::unordered_set<uint64_t> pendingNodeKeys;
    for (const auto &pending : _pendingNodes) {
        pendingNodeKeys.insert(pending.node.key());
    }
    for (const auto &request : _quadtree->requests()) {
        const auto &node = *_quadtree->find(request);
        if (pendingNodeKeys.count(request.key())) {
            continue;
        }
        if (!sectorRootsReady && (node.ready || request.level != data.maxTileLevel)) {
            continue;
        }
        PendingNode pending;
        pending.node = request;
        if (!node.ready && request.level == data.maxTileLevel) {
            // Reserve full coarse coverage before admitting finer requests.
            pending.material.pages = {{layout.rootLevel(), request.x, request.z}};
        } else {
            pending.material = layout.buildGridMapping(data, request, node.geometry.minY, node.geometry.maxY, _quadtree->localCameraPosition());
        }
        if (node.ready && sameMapping(node.material, pending.material)) {
            continue;
        }
        auto candidateResources = resources;
        appendResources(candidateResources, pending);
        if (!canPrepare(candidateResources, pageSources)) {
            continue;
        }
        candidateResources.mappingCount += node.ready ? 0U : 1U;
        if (!_vtRenderer.pageTable().canFit(candidateResources.mappingCount)) {
            continue;
        }
        resources = std::move(candidateResources);
        pendingNodeKeys.insert(pending.node.key());
        _pendingNodes.push_back(std::move(pending));
    }
}

LandscapeSchedule::Resources LandscapeSchedule::collectResources() const {
    Resources resources;
    ccstd::unordered_set<uint64_t> sourceKeys;
    ccstd::unordered_set<uint64_t> pageKeys;
    _quadtree->forEachNode([&](const Quadtree::Node &node) {
        if (!node.ready) {
            return true;
        }
        ++resources.mappingCount;
        const auto address = node.geometry.address();
        const auto source = address.ancestor(std::max(address.level, _asset.data().minTileLevel));
        if (sourceKeys.insert(source.key()).second) {
            resources.sources.push_back(source);
        }
        for (const auto page : node.material.pages) {
            if (pageKeys.insert(page.key()).second) {
                resources.pages.push_back(page);
            }
        }
        return true;
    });
    std::sort(resources.pages.begin(), resources.pages.end(), [](VTPageAddress a, VTPageAddress b) {
        return a.key() < b.key();
    });
    for (const auto &pending : _pendingNodes) {
        appendResources(resources, pending);
        // A replacement shares the ready node's page-table entry.
        if (!_quadtree->find(pending.node)->ready) {
            ++resources.mappingCount;
        }
    }
    return resources;
}

void LandscapeSchedule::appendResources(Resources &resources, const PendingNode &pending) const {
    const auto source = pending.node.ancestor(std::max(pending.node.level, _asset.data().minTileLevel));
    const auto sourceExists = std::any_of(resources.sources.begin(), resources.sources.end(), [&](NodeAddress entry) {
        return entry.key() == source.key();
    });
    if (!sourceExists) {
        resources.sources.push_back(source);
    }
    // Keep the retained page order while adding only this candidate's demand.
    // Existing mappings remain reserved until their replacement is published.
    for (const auto page : pending.material.pages) {
        const auto position = std::lower_bound(resources.pages.begin(), resources.pages.end(), page, [](VTPageAddress left, VTPageAddress right) {
            return left.key() < right.key();
        });
        if (position == resources.pages.end() || position->key() != page.key()) {
            resources.pages.insert(position, page);
        }
    }
}

bool LandscapeSchedule::canPrepare(const Resources &resources, ccstd::unordered_map<uint64_t, ccstd::vector<NodeAddress>> &pageSources) const {
    if (!_residency.canReserve(resources.sources)) {
        return false;
    }
    if (!_vtRenderer.canReserve(resources.pages)) {
        return false;
    }
    ccstd::unordered_set<uint64_t> retained;
    for (const auto source : resources.sources) {
        retained.insert(source.key());
    }
    // Reserve room for each page's worst-case composition footprint, including
    // far detail and triplanar projection. Composition inputs can then be streamed in bounded batches.
    for (const auto page : resources.pages) {
        const auto [entry, inserted] = pageSources.try_emplace(page.key());
        if (inserted) {
            _vtRenderer.selectSources(page, true, true).appendTo(entry->second);
        }
        if (!sourcesFit(retained, entry->second, _residency.layerCount())) {
            return false;
        }
    }
    return true;
}

ccstd::vector<NodeAddress> LandscapeSchedule::compositionSources() const {
    auto sources = _reservedResources.sources;
    ccstd::unordered_set<uint64_t> sourceKeys;
    for (const auto source : sources) {
        sourceKeys.insert(source.key());
    }
    for (const auto page : _reservedResources.pages) {
        if (!_vtRenderer.needsComposition(page)) {
            continue;
        }
        ccstd::vector<NodeAddress> inputs;
        _vtRenderer.selectSources(page, _vtRenderer.getRefineFarDetail(), _vtRenderer.getTriplanarEnabled()).appendTo(inputs);
        if (!sourcesFit(sourceKeys, inputs, _residency.layerCount())) {
            continue;
        }
        for (const auto source : inputs) {
            if (sourceKeys.insert(source.key()).second) {
                sources.push_back(source);
            }
        }
    }
    return sources;
}

#if CC_LANDSCAPE_DEBUG
bool LandscapeSchedule::morphRangeTooSmall(const QuadNode &node) const {
    const auto address = node.address();
    const auto &data = _asset.data();
    const auto &ranges = _quadtree->lodMorphRanges();
    const auto &camera = _quadtree->localCameraPosition();
    const auto &origin = _quadtree->worldOrigin();
    // Diagnose insufficient morph distance only for visible/nearby draws.
    if (address.level == data.maxTileLevel) {
        return false;
    }
    const auto area = _quadtree->region(address);
    const float x = std::max(std::abs(area.x - camera.x), std::abs(area.x + area.size - camera.x));
    const float y = std::max(std::abs(node.minY - camera.y), std::abs(node.maxY - camera.y));
    const float z = std::max(std::abs(area.z - camera.z), std::abs(area.z + area.size - camera.z));
    const float start = ranges[address.level + 1].start;
    if (x * x + y * y + z * z <= start * start) {
        return false;
    }
    const auto bounds = _asset.heightRanges()[address.level][static_cast<size_t>(address.z) * data.nodesX(address.level) + address.x];
    const float dx = std::max({area.x - camera.x, camera.x - area.x - area.size, 0.0F});
    const float dy = std::max({bounds.minY - camera.y, camera.y - bounds.maxY, 0.0F});
    const float dz = std::max({area.z - camera.z, camera.z - area.z - area.size, 0.0F});
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    const float range = ranges[address.level].end;
    if (distanceSq > range * range) {
        return false;
    }
    const Vec3 center{area.x + area.size * 0.5F + origin.x, (bounds.minY + bounds.maxY) * 0.5F + origin.y, area.z + area.size * 0.5F + origin.z};
    const Vec3 extent{area.size * 0.5F, (bounds.maxY - bounds.minY) * 0.5F, area.size * 0.5F};
    const float nearbyRange = ranges[std::min(1U, data.maxTileLevel)].end;
    return std::any_of(_passes.begin(), _passes.end(), [&](const Pass &pass) {
        if (pass.includeNearby && distanceSq <= nearbyRange * nearbyRange) {
            return true;
        }
        return std::all_of(pass.planes.begin(), pass.planes.end(), [&](const Vec4 &plane) {
            const float radius = extent.x * std::abs(plane.x) + extent.y * std::abs(plane.y) + extent.z * std::abs(plane.z);
            return center.x * plane.x + center.y * plane.y + center.z * plane.z + radius >= plane.w;
        });
    });
}
#endif

void LandscapeSchedule::stopLoading() {
    _loadingStopped = true;
    CC_LOG_ERROR("[Landscape] Tile loading stopped; re-enable the landscape to reload.");
}
} // namespace cc::landscape

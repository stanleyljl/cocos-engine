// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Schedule.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "base/Log.h"
#include "base/Macros.h"
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

} // namespace

LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, LandscapeRenderer &renderer, float lodQualityScale)
: _asset(asset)
, _residency(renderer.residency())
, _vtRenderer(renderer.vtRenderer())
, _renderer(renderer)
, _quadtree(std::make_unique<Quadtree>()) {
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

LandscapeSchedule::~LandscapeSchedule() {
    submitRequests({});
}

void LandscapeSchedule::update(const Vec3 &camera, const Vec3 &origin) {
    _quadtree->updateTarget(camera, origin);
    _renderer.setViewPosition(camera);
    prepareRequests();
}

void LandscapeSchedule::prepareRenderData(uint32_t frame) {
    auto *previousMappings = _vtRenderer.pageTable().texture();
    prepareResources(frame);
    _quadtree->selectRenderables();
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

void LandscapeSchedule::beginPasses() {
    _activePassCount = 0;
}

void LandscapeSchedule::addPass(const geometry::Frustum &frustum, bool includeNearby) {
    if (_activePassCount == _passes.size()) {
        _passes.emplace_back();
    }
    auto &pass = _passes[_activePassCount++];
    pass.includeNearby = includeNearby;

    for (size_t i = 0; i < pass.planes.size(); ++i) {
        const auto &plane = *frustum.planes[i];
        pass.planes[i] = Vec4{plane.n.x, plane.n.y, plane.n.z, plane.d};
    }
}

void LandscapeSchedule::collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const {
    _quadtree->cull([&](const Quadtree::Node &node) {
        return _renderer.collectNodeModels(node.geometry, _quadtree->region(node.geometry.address()), node.renderable, frustum, shadow, models);
    });
}

void LandscapeSchedule::endPasses() {
    _passes.resize(_activePassCount);
    checkMorphRanges();
}

bool LandscapeSchedule::isRenderStateStable() const {
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

void LandscapeSchedule::prepareRequests() {
    _requestsDirty |= _quadtree->viewChanged() || _quadtree->renderSelectionChanged();
    const auto previousPendingCount = _pendingNodes.size();
    _pendingNodes.erase(std::remove_if(_pendingNodes.begin(), _pendingNodes.end(), [&](const PendingNode &pending) {
        return !_quadtree->isInTargetTree(pending.node);
    }), _pendingNodes.end());
    const bool requestsCancelled = _pendingNodes.size() != previousPendingCount;
    _requestsDirty |= requestsCancelled;

    // Keep the input batch pinned while loading; only cancellation replaces it early.
    const bool sourceBatchLoaded = _residency.ready();
    if (!sourceBatchLoaded && !requestsCancelled) {
        return;
    }
    const bool scanRequests = _requestsDirty && sourceBatchLoaded;
    if (scanRequests || requestsCancelled) {
        auto resources = collectResources();
        const bool sectorRootsReady = _quadtree->sectorRootsReady();
        if (scanRequests) {
            schedulePendingNodes(resources, sectorRootsReady);
        }

        submitRequests(std::move(resources));
        if (scanRequests) {
            _requestsDirty = false;
        }
    }
}

LandscapeSchedule::Resources LandscapeSchedule::collectResources() const {
    Resources resources;
    ccstd::unordered_set<uint64_t> sourceKeys;
    ccstd::unordered_set<uint64_t> pageKeys;
    _quadtree->forEachNode([&](const Quadtree::Node &node) {
        if (!node.ready()) {
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
        if (!_quadtree->find(pending.node)->ready()) {
            ++resources.mappingCount;
        }
    }
    return resources;
}

void LandscapeSchedule::schedulePendingNodes(Resources &resources, bool sectorRootsReady) {
    const auto &data = _asset.data();
    const VTPageLayout layout(data.sectorSize);
    auto capacity = _vtRenderer.capacityQuery();
    ccstd::unordered_set<uint64_t> pendingNodeKeys;
    for (const auto &pending : _pendingNodes) {
        pendingNodeKeys.insert(pending.node.key());
    }
    for (const auto &request : _quadtree->requests()) {
        const auto &node = *_quadtree->find(request);
        if (pendingNodeKeys.count(request.key())) {
            continue;
        }
        if (!sectorRootsReady && (node.ready() || request.level != data.maxTileLevel)) {
            continue;
        }
        PendingNode pending;
        pending.node = request;
        if (!node.ready() && request.level == data.maxTileLevel) {
            // Reserve full coarse coverage before admitting finer requests.
            pending.material.pages = {{layout.rootLevel(), request.x, request.z}};
        } else {
            pending.material = layout.buildGridMapping(data, request, node.geometry.minY, node.geometry.maxY, _quadtree->localCameraPosition());
        }
        if (node.ready() && sameMapping(node.material, pending.material)) {
            continue;
        }
        auto candidateResources = resources;
        appendResources(candidateResources, pending);
        if (!capacity.canPrepare(candidateResources.pages, candidateResources.sources)) {
            continue;
        }
        candidateResources.mappingCount += node.ready() ? 0U : 1U;
        if (!_vtRenderer.pageTable().canFit(candidateResources.mappingCount)) {
            continue;
        }
        resources = std::move(candidateResources);
        pendingNodeKeys.insert(pending.node.key());
        _pendingNodes.push_back(std::move(pending));
    }
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

void LandscapeSchedule::submitRequests(Resources resources) {
    ccstd::unordered_set<uint64_t> sources;
    ccstd::unordered_set<uint64_t> pages;
    for (const auto source : resources.sources) {
        sources.insert(source.key());
    }
    for (const auto page : resources.pages) {
        pages.insert(page.key());
    }
    for (const auto source : _requestedResources.sources) {
        if (!sources.erase(source.key())) {
            _residency.release(source);
        }
    }
    for (const auto page : _requestedResources.pages) {
        if (!pages.erase(page.key())) {
            _vtRenderer.release(page);
        }
    }
    for (const auto source : resources.sources) {
        if (sources.count(source.key())) {
            _residency.request(source);
        }
    }
    for (const auto page : resources.pages) {
        if (pages.count(page.key())) {
            _vtRenderer.request(page);
        }
    }
    _requestedResources = std::move(resources);
}

void LandscapeSchedule::prepareResources(uint32_t frame) {
    if (_lastUpdateFrame && *_lastUpdateFrame == frame) {
        return;
    }
    _lastUpdateFrame = frame;
    if (!_vtRenderer.pageTable().ensureCapacity(_requestedResources.mappingCount)) {
        return;
    }
    // VT submits source demand first; newly uploaded inputs compose on the next frame.
    [[maybe_unused]] const bool pagesUpdated = _vtRenderer.update(config::VT_PAGE_UPDATE_BUDGET);
    CC_ASSERT(pagesUpdated);
    [[maybe_unused]] const bool sourcesUpdated = _residency.update(config::PAGE_UPLOAD_BUDGET);
    CC_ASSERT(sourcesUpdated);
    publishNodes();
    // Selection can retire retained descendants after this stage; release their demands next tick.
    _requestsDirty |= _quadtree->viewChanged();
}

void LandscapeSchedule::publishNodes() {
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
        [[maybe_unused]] const bool published = _quadtree->publish(it->node, std::move(it->material));
        CC_ASSERT(published);
        it = _pendingNodes.erase(it);
        _requestsDirty = true;
    }
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

void LandscapeSchedule::checkMorphRanges() {
#if CC_LANDSCAPE_DEBUG
    if (!_quadtree->sectorRootsReady()) {
        return;
    }
    bool morphRangeWarning = false;
    _quadtree->forEachRenderable([&](const Quadtree::Node &node) {
        if (!morphRangeWarning) {
            morphRangeWarning = _quadtree->morphRangeTooSmall(node.geometry) && isMorphCheckVisible(node.geometry.address());
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
#endif
}

bool LandscapeSchedule::isMorphCheckVisible(NodeAddress address) const {
    const auto &ranges = _quadtree->lodMorphRanges();
    const float distanceSq = _quadtree->distanceSquared(address);
    const float range = ranges[address.level].end;
    if (distanceSq > range * range) {
        return false;
    }
    const auto &data = _asset.data();
    const auto area = _quadtree->region(address);
    const auto bounds = _asset.heightRanges()[address.level][static_cast<size_t>(address.z) * data.nodesX(address.level) + address.x];
    const auto &origin = _quadtree->worldOrigin();
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

} // namespace cc::landscape

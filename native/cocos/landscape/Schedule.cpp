// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Schedule.h"

#include <algorithm>
#include <utility>
#include "base/Log.h"
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

// Resource preparation: load sources, compose pages, then publish ready nodes.
LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, LandscapeResidency &residency, VTRenderer &vtRenderer, Quadtree &quadtree)
: _asset(asset), _residency(residency), _vtRenderer(vtRenderer), _quadtree(quadtree) {}

bool LandscapeSchedule::isSettled() const {
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
    if (!_quadtree.sectorRootsReady()) {
        return false;
    }
    // Selection runs after preparation and may expose another level of requests.
    return !_quadtree.renderSelectionChanged();
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

    // Startup and streaming share the same asynchronous, per-frame resource stages.
    _residency.update(config::PAGE_UPLOAD_BUDGET);
    if (!prepareRequests()) {
        stopLoading();
        return;
    }
    if (!loadNodes()) {
        stopLoading();
        return;
    }
    composeNodes();
    if (!publishNodes()) {
        stopLoading();
    }
}

bool LandscapeSchedule::prepareRequests() {
    _requestsDirty |= _quadtree.viewChanged() || _quadtree.renderSelectionChanged();
    const auto previousPendingCount = _pendingNodes.size();
    _pendingNodes.erase(std::remove_if(_pendingNodes.begin(), _pendingNodes.end(), [&](const PendingNode &pending) {
        return !_quadtree.find(pending.node);
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
        uint32_t mappingCount = 0;
        auto resources = collectResources(mappingCount);
        const bool sectorRootsReady = _quadtree.sectorRootsReady();
        if (scanRequests) {
            admitNodes(resources, mappingCount, sectorRootsReady);
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

bool LandscapeSchedule::loadNodes() {
    if (!_residency.ready()) {
        if (!_residency.loadReserved()) {
            return false;
        }
    }
    for (const auto &pending : _pendingNodes) {
        auto &node = *_quadtree.find(pending.node);
        if (!node.loaded) {
            node.loaded = _residency.resolve(pending.node).layer >= 0;
        }
    }
    return true;
}

void LandscapeSchedule::composeNodes() {
    if (_vtRenderer.ready()) {
        return;
    }
    ccstd::unordered_set<uint64_t> eligiblePageKeys;
    _quadtree.forEachNode([&](const Quadtree::Node &node) {
        // Ready pages may have been invalidated by material or decal changes.
        if (node.ready) {
            for (const auto page : node.material.pages) {
                eligiblePageKeys.insert(page.key());
            }
        }
        return true;
    });
    for (const auto &pending : _pendingNodes) {
        if (!_quadtree.find(pending.node)->loaded) {
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
        const auto &node = *_quadtree.find(it->node);
        if (!node.loaded) {
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
        if (!_quadtree.publish(it->node, std::move(it->material))) {
            return false;
        }
        it = _pendingNodes.erase(it);
        _requestsDirty = true;
    }
    return true;
}

void LandscapeSchedule::admitNodes(Resources &resources, uint32_t mappingCount, bool sectorRootsReady) {
    const auto &data = _asset.data();
    const VTPageLayout layout(data.sectorSize);
    ccstd::unordered_set<uint64_t> pendingNodeKeys;
    for (const auto &pending : _pendingNodes) {
        pendingNodeKeys.insert(pending.node.key());
    }
    for (const auto &request : _quadtree.requests()) {
        const auto &node = *_quadtree.find(request);
        if (pendingNodeKeys.count(request.key())) {
            continue;
        }
        if (!sectorRootsReady && (node.ready || request.level != data.maxLevel)) {
            continue;
        }
        PendingNode pending;
        pending.node = request;
        if (!node.ready && request.level == data.maxLevel) {
            // Reserve full coarse coverage before admitting finer requests.
            pending.material.pages = {{layout.rootLevel(), request.x, request.z}};
        } else {
            pending.material = layout.buildGridMapping(data.nodeRegion(request), node.geometry.minY, node.geometry.maxY, _quadtree.localCameraPosition());
        }
        if (node.ready && sameMapping(node.material, pending.material)) {
            continue;
        }
        auto candidateResources = resources;
        appendResources(candidateResources, pending);
        if (!canPrepare(candidateResources)) {
            continue;
        }
        const uint32_t requiredMappingCount = mappingCount + (node.ready ? 0U : 1U);
        if (!_vtRenderer.pageTable().ensureCapacity(requiredMappingCount)) {
            continue;
        }
        resources = std::move(candidateResources);
        mappingCount = requiredMappingCount;
        pendingNodeKeys.insert(pending.node.key());
        _pendingNodes.push_back(std::move(pending));
    }
}

LandscapeSchedule::Resources LandscapeSchedule::collectResources(uint32_t &mappingCount) const {
    Resources resources;
    mappingCount = 0;
    ccstd::unordered_set<uint64_t> sourceKeys;
    ccstd::unordered_set<uint64_t> pageKeys;
    _quadtree.forEachNode([&](const Quadtree::Node &node) {
        if (!node.ready) {
            return true;
        }
        ++mappingCount;
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
        if (!_quadtree.find(pending.node)->ready) {
            ++mappingCount;
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

bool LandscapeSchedule::canPrepare(const Resources &resources) const {
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
    // far detail. Composition inputs can then be streamed in bounded batches.
    for (const auto page : resources.pages) {
        ccstd::vector<NodeAddress> inputs;
        _vtRenderer.selectSources(page, true).appendTo(inputs);
        if (!sourcesFit(retained, inputs, _residency.layerCount())) {
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
        _vtRenderer.selectSources(page, _vtRenderer.getRefineFarDetail()).appendTo(inputs);
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

void LandscapeSchedule::stopLoading() {
    _loadingStopped = true;
    CC_LOG_ERROR("[Landscape] Tile loading stopped; re-enable the landscape to reload.");
}
} // namespace cc::landscape

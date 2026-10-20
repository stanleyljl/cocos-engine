// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeSchedule.h"

#include <algorithm>
#include "base/Log.h"
#include "landscape/VTRenderer.h"

namespace cc::landscape {
namespace {
bool sameMapping(const VTGridMapping &a, const VTGridMapping &b) {
    return a.cells == b.cells && a.pages.size() == b.pages.size() &&
           std::equal(a.pages.begin(), a.pages.end(), b.pages.begin(), [](VTPageAddress left, VTPageAddress right) {
               return left.key() == right.key();
           });
}

// Page inputs are unique. Count only those not already in the protected set.
bool sourcesFit(const ccstd::unordered_set<uint64_t> &protectedSources, const ccstd::vector<NodeAddress> &inputs, uint32_t capacity) {
    const auto additional = std::count_if(inputs.begin(), inputs.end(), [&](NodeAddress source) {
        return protectedSources.count(source.key()) == 0;
    });
    return protectedSources.size() + additional <= capacity;
}
} // namespace

LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, TilePagePool &tiles, VTRenderer &vt)
: _asset(asset), _tiles(tiles), _vt(vt), _data(asset.data()),
  _layout(_data.sectorSize), _expectedTree(_data.maxLevel) {
    _currentTree.init(_data);
    // Start with complete sector coverage and one coarse material page per root.
    for (const auto &entry : _currentTree.nodes()) {
        const auto address = entry.second.geometry.address();
        CurrentTree::Node node;
        node.geometry = geometry(address);
        node.material.pages.push_back({_layout.rootLevel(), address.x, address.z});
        _pending.nodes.emplace(entry.first, std::move(node));
    }
}

QuadNode LandscapeSchedule::geometry(NodeAddress address) const {
    float minY = _data.minHeight();
    float maxY = _data.maxHeight();
    _asset.getHeightRange(address.level, address.x, address.z, minY, maxY);
    const auto source = address.ancestor(std::max(address.level, _data.minTileLevel));
    float sourceMin = minY;
    float sourceMax = maxY;
    _asset.getHeightRange(source.level, source.x, source.z, sourceMin, sourceMax);
    return {address.level, address.x, address.z, std::min(minY, sourceMin), std::max(maxY, sourceMax)};
}

float LandscapeSchedule::distance(NodeAddress node) const {
    const auto &camera = _expectedTree.camera();
    const auto bounds = geometry(node);
    const auto region = _data.nodeRegion(node);
    const float x = std::max({region.x - camera.x, camera.x - region.x - region.size, 0.0F});
    const float y = std::max({bounds.minY - camera.y, camera.y - bounds.maxY, 0.0F});
    const float z = std::max({region.z - camera.z, camera.z - region.z - region.size, 0.0F});
    return x * x + y * y + z * z;
}

CurrentTree::Node LandscapeSchedule::desiredNode(NodeAddress address) const {
    CurrentTree::Node result;
    result.geometry = geometry(address);
    result.material = _layout.gridMapping(_data.nodeRegion(address), result.geometry.minY,
                                         result.geometry.maxY, _expectedTree.camera());
    return result;
}

void LandscapeSchedule::buildRequests() {
    _requests.clear();
    _requestIndex = 0;
    const auto append = [this](NodeAddress node, RequestType type, uint32_t priority) {
        _requests.push_back({node, type, priority, _expectedTree.visible(node), distance(node)});
    };
    for (const auto &request : _currentTree.requests(_expectedTree)) {
        append(request.node, request.split ? RequestType::SPLIT : RequestType::MERGE, request.priority);
    }
    for (const auto &entry : _currentTree.nodes()) {
        append(entry.second.geometry.address(), RequestType::MATERIAL, 0);
    }
    // Release capacity first; otherwise favor visible, nearby work.
    std::sort(_requests.begin(), _requests.end(), [](const Request &a, const Request &b) {
        if ((a.type == RequestType::MERGE) != (b.type == RequestType::MERGE)) {
            return a.type == RequestType::MERGE;
        }
        if (a.visible != b.visible) {
            return a.visible;
        }
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        if (a.type != b.type) {
            return a.type < b.type;
        }
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.node.key() < b.node.key();
    });
    _rebuildRequests = false;
}

bool LandscapeSchedule::selectNextChange() {
    if (_rebuildRequests) {
        buildRequests();
    }
    while (_requestIndex < _requests.size()) {
        const auto request = _requests[_requestIndex++];
        if (request.type == RequestType::MATERIAL) {
            auto node = desiredNode(request.node);
            if (sameMapping(node.material, _currentTree.nodes().at(request.node.key()).material)) {
                continue;
            }
            _pending.nodes.emplace(request.node.key(), std::move(node));
        } else {
            if (!_currentTree.plan({request.node, request.type == RequestType::SPLIT, request.priority}, _pending)) {
                continue;
            }
            for (auto &entry : _pending.nodes) {
                entry.second = desiredNode(entry.second.geometry.address());
            }
        }
        if (admitChange()) {
            return true;
        }
        // A refused candidate owns no resources and leaves the current tree intact.
        _pending = {};
    }
    return false;
}

LandscapeSchedule::Resources LandscapeSchedule::collectResources() const {
    Resources resources;
    ccstd::unordered_set<uint64_t> sources;
    ccstd::unordered_set<uint64_t> pages;
    _vt.requiredSources(resources.sources);
    for (const auto source : resources.sources) {
        sources.insert(source.key());
    }
    const auto collect = [&](const auto &nodes) {
        for (const auto &entry : nodes) {
            const auto address = entry.second.geometry.address();
            const auto source = address.ancestor(std::max(address.level, _data.minTileLevel));
            if (sources.insert(source.key()).second) {
                resources.sources.push_back(source);
            }
            for (const auto page : entry.second.material.pages) {
                if (pages.insert(page.key()).second) {
                    resources.pages.push_back(page);
                }
            }
        }
    };
    // Old bindings remain protected until the whole local change is committed.
    collect(_currentTree.nodes());
    collect(_pending.nodes);
    std::sort(resources.pages.begin(), resources.pages.end(), [](auto a, auto b) {
        return a.key() < b.key();
    });
    return resources;
}

bool LandscapeSchedule::canPrepare(const Resources &resources) const {
    if (!_tiles.canReserve(resources.sources) || !_vt.pages().canReserve(resources.pages)) {
        return false;
    }
    ccstd::unordered_set<uint64_t> pinned;
    for (const auto source : resources.sources) {
        pinned.insert(source.key());
    }
    // Every page must fit with all displayed/candidate height and projection
    // sources, including the larger footprint needed when normal baking is enabled.
    // Other composition inputs can be released between batches.
    for (const auto page : resources.pages) {
        ccstd::vector<NodeAddress> inputs;
        _tiles.pageSources(page, true, inputs);
        if (!sourcesFit(pinned, inputs, _tiles.layerCount())) {
            return false;
        }
    }
    return true;
}

bool LandscapeSchedule::admitChange() {
    auto resources = collectResources();
    if (!canPrepare(resources)) {
        return false;
    }
    const auto currentCount = _currentTree.nodes().size();
    const auto nextCount = _pending.removed.empty() ? currentCount
                                                  : currentCount - _pending.removed.size() + _pending.nodes.size();
    if (!_vt.ensureGridMappingCapacity(static_cast<uint32_t>(std::max(currentCount, nextCount)))) {
        return false;
    }
    // Admission checks the entire transition before either pool changes ownership.
    if (!_vt.pages().reserve(resources.pages) || !_tiles.reserve(resources.sources)) {
        return false;
    }
    _resources = std::move(resources);
    return true;
}

ccstd::vector<NodeAddress> LandscapeSchedule::compositionSources() const {
    auto sources = _resources.sources;
    ccstd::unordered_set<uint64_t> working;
    for (const auto source : sources) {
        working.insert(source.key());
    }
    for (const auto page : _resources.pages) {
        if (!_vt.pages().needsComposition(page)) {
            continue;
        }
        ccstd::vector<NodeAddress> inputs;
        _tiles.pageSources(page, _vt.isBakeNormalEnabled(), inputs);
        if (!sourcesFit(working, inputs, _tiles.layerCount())) {
            continue;
        }
        for (const auto source : inputs) {
            if (working.insert(source.key()).second) {
                sources.push_back(source);
            }
        }
    }
    return sources;
}

bool LandscapeSchedule::resourcesReady() const {
    return _tiles.ready() && _vt.pages().ready() && _vt.sourcesReady();
}

void LandscapeSchedule::stopLoading() {
    _failed = true;
    CC_LOG_ERROR("[Landscape] Tile loading stopped; re-enable the landscape to reload.");
}

bool LandscapeSchedule::prepareResources(bool synchronous) {
    while (!resourcesReady()) {
        // Preserve the current batch while its IO/uploads are incomplete. This
        // prevents view or material edits from continually replacing in-flight work.
        if (!_tiles.ready() && !_tiles.loadReserved(synchronous)) {
            stopLoading();
            return false;
        }
        if (!_tiles.ready()) {
            return false;
        }
        if (!_tiles.reserve(compositionSources()) || !_tiles.loadReserved(synchronous)) {
            stopLoading();
            return false;
        }
        const auto budget = synchronous ? config::VT_PAGE_COUNT : _remainingPages;
        const auto composed = _vt.render(_tiles, budget);
        if (!synchronous) {
            _remainingPages -= composed;
        }
        if (resourcesReady()) {
            return true;
        }
        if (!synchronous && (composed == 0 || _remainingPages == 0)) {
            return false;
        }
        // Synchronous loading has completed every input in this batch. Admission
        // guarantees that at least one dirty page fits, so no progress is an error.
        if (synchronous && composed == 0) {
            stopLoading();
            return false;
        }
    }
    return true;
}

void LandscapeSchedule::commitChange() {
    _currentTree.commit(_pending);
    _pending = {};
    _rebuildRequests = true;
    // The old tree is no longer displayed. Keep only committed height sources,
    // material pages and projection references; composition inputs are temporary.
    _resources = collectResources();
    _vt.pages().reserve(_resources.pages);
    _tiles.reserve(_resources.sources);
}

void LandscapeSchedule::update(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surface, const Vec3 &camera, uint32_t frame, bool frozen) {
    _currentTree.clearChanges();
    if (_failed || (frozen && _ready)) {
        return;
    }
    if (_asset.loadingFailed()) {
        stopLoading();
        return;
    }
    _rebuildRequests |= _expectedTree.target(geometryNodes);
    _rebuildRequests |= _expectedTree.setView(surface, camera);
    if (!_frame || *_frame != frame) {
        _frame = frame;
        _remainingPages = config::VT_PAGE_UPDATE_BUDGET;
        if (_ready) {
            _tiles.update(config::PAGE_UPLOAD_BUDGET);
        }
    }
    if (_resources.pages.empty() && !admitChange()) {
        stopLoading();
        return;
    }
    // One flow serves initial warmup, streaming transitions and invalidated
    // materials. Only warmup blocks on IO and ignores the per-frame page budget.
    for (;;) {
        if (!prepareResources(!_ready)) {
            return;
        }
        if (!_pending.empty()) {
            commitChange();
        }
        if (!selectNextChange()) {
            _ready = true;
            return;
        }
    }
}
} // namespace cc::landscape

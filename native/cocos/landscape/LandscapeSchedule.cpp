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
}

bool LandscapeSchedule::beginInitialChange() {
    Transaction transaction;
    // Bootstrap uses the same admission/prepare/commit path as later changes.
    // Every sector starts with one coarse material page and complete coverage.
    for (const auto &entry : _currentTree.nodes()) {
        const auto address = entry.second.geometry.address();
        CurrentTree::Node node;
        node.geometry = geometry(address);
        node.material.pages.push_back({_layout.rootLevel(), address.x, address.z});
        transaction.topology.added.push_back(address);
        transaction.nodes.push_back(std::move(node));
    }
    return admit(std::move(transaction));
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
    const auto &camera = _camera;
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
                                         result.geometry.maxY, _camera);
    return result;
}

bool LandscapeSchedule::beginNextChange() {
    if (!_needsPlan) {
        return false;
    }
    struct Work {
        CurrentTree::Candidate candidate;
        bool visible;
        float distance;
        uint32_t priority;
    };
    ccstd::vector<Work> work;
    for (const auto candidate : _currentTree.candidates(_expectedTree)) {
        const auto priority = candidate.operation == CurrentTree::Operation::SPLIT ? _expectedTree.refinementPriority(candidate.node) : 0;
        work.push_back({candidate, _expectedTree.visible(candidate.node), distance(candidate.node),
                        priority});
    }
    // Release capacity first; otherwise favor visible, nearby work. All priorities
    // refer to this cut and view and are discarded as soon as one change is admitted.
    using Operation = CurrentTree::Operation;
    std::sort(work.begin(), work.end(), [](const Work &a, const Work &b) {
        const auto left = a.candidate.operation;
        const auto right = b.candidate.operation;
        if ((left == Operation::MERGE) != (right == Operation::MERGE)) {
            return left == Operation::MERGE;
        }
        if (a.visible != b.visible) {
            return a.visible;
        }
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        if (left != right) {
            return left < right;
        }
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.candidate.node.key() < b.candidate.node.key();
    });
    for (const auto &item : work) {
        Transaction transaction;
        if (item.candidate.operation == Operation::REFRESH) {
            auto node = desiredNode(item.candidate.node);
            if (sameMapping(node.material, _currentTree.nodes().at(item.candidate.node.key()).material)) {
                continue;
            }
            // Most view changes leave a grid's material mapping unchanged.
            // Build a refresh transaction only after finding an actual change.
            transaction.nodes.push_back(std::move(node));
        }
        if (!_currentTree.plan(item.candidate, transaction.topology)) {
            continue;
        }
        if (item.candidate.operation != Operation::REFRESH) {
            for (const auto address : transaction.topology.added) {
                transaction.nodes.push_back(desiredNode(address));
            }
        }
        if (admit(std::move(transaction))) {
            return true;
        }
    }
    // Retry only after demand/view changes or a commit releases resources.
    _needsPlan = false;
    return false;
}

LandscapeSchedule::Resources LandscapeSchedule::collectResources(const ccstd::vector<CurrentTree::Node> &added) const {
    Resources resources;
    ccstd::unordered_set<uint64_t> sources;
    ccstd::unordered_set<uint64_t> pages;
    const auto collect = [&](const CurrentTree::Node &node) {
        const auto address = node.geometry.address();
        const auto source = address.ancestor(std::max(address.level, _data.minTileLevel));
        if (sources.insert(source.key()).second) {
            resources.sources.push_back(source);
        }
        for (const auto page : node.material.pages) {
            if (pages.insert(page.key()).second) {
                resources.pages.push_back(page);
            }
        }
    };
    // Keep all displayed bindings alive, including nodes retired by the transaction.
    for (const auto &entry : _currentTree.nodes()) {
        collect(entry.second);
    }
    for (const auto &node : added) {
        collect(node);
    }
    std::sort(resources.pages.begin(), resources.pages.end(), [](auto a, auto b) {
        return a.key() < b.key();
    });
    return resources;
}

bool LandscapeSchedule::canPrepare(const Resources &resources) const {
    if (!_tiles.canReserve(resources.sources) || !_vt.canReserve(resources.pages)) {
        return false;
    }
    ccstd::unordered_set<uint64_t> pinned;
    for (const auto source : resources.sources) {
        pinned.insert(source.key());
    }
    // Every page must fit with all displayed/candidate height sources,
    // including the larger footprint needed when far normal detail is enabled.
    // Other composition inputs can be released between batches.
    for (const auto page : resources.pages) {
        ccstd::vector<NodeAddress> inputs;
        _vt.sources(page, true).appendTo(inputs);
        if (!sourcesFit(pinned, inputs, _tiles.layerCount())) {
            return false;
        }
    }
    return true;
}

bool LandscapeSchedule::admit(Transaction transaction) {
    transaction.resources = collectResources(transaction.nodes);
    const auto &resources = transaction.resources;
    if (!canPrepare(resources)) {
        return false;
    }
    const auto currentCount = _currentTree.nodes().size();
    const auto &topology = transaction.topology;
    const auto nextCount = topology.removed.empty() ? currentCount
                                                  : currentCount - topology.removed.size() + topology.added.size();
    if (!_vt.pageTable().ensureCapacity(static_cast<uint32_t>(std::max(currentCount, nextCount)))) {
        return false;
    }
    // Validate the entire union before changing either pool's reservations.
    if (!_vt.reserve(resources.pages) || !_tiles.reserve(resources.sources)) {
        return false;
    }
    _pending = std::move(transaction);
    return true;
}

const LandscapeSchedule::Resources &LandscapeSchedule::activeResources() const {
    return _pending ? _pending->resources : _resources;
}

ccstd::vector<NodeAddress> LandscapeSchedule::compositionSources() const {
    const auto &resources = activeResources();
    auto sources = resources.sources;
    ccstd::unordered_set<uint64_t> working;
    for (const auto source : sources) {
        working.insert(source.key());
    }
    for (const auto page : resources.pages) {
        if (!_vt.needsComposition(page)) {
            continue;
        }
        ccstd::vector<NodeAddress> inputs;
        _vt.sources(page, _vt.getRefineFarDetail()).appendTo(inputs);
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
    return _tiles.ready() && _vt.ready();
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
    _currentTree.commit(_pending->topology, _pending->nodes);
    _pending.reset();
    _needsPlan = true;
    // The old tree is no longer displayed. Keep only committed height sources,
    // material pages and projection references; composition inputs are temporary.
    _resources = collectResources();
    _vt.reserve(_resources.pages);
    _tiles.reserve(_resources.sources);
}

void LandscapeSchedule::update(ExpectedTree expected, const Vec3 &camera, uint32_t frame) {
    _currentTree.clearChanges();
    if (_failed) {
        return;
    }
    if (_asset.loadingFailed()) {
        stopLoading();
        return;
    }
    _needsPlan |= !(_expectedTree == expected);
    _expectedTree = std::move(expected);
    _needsPlan |= _camera.x != camera.x || _camera.y != camera.y || _camera.z != camera.z;
    _camera = camera;
    if (!_frame || *_frame != frame) {
        _frame = frame;
        _remainingPages = config::VT_PAGE_UPDATE_BUDGET;
        if (_ready) {
            _tiles.update(config::PAGE_UPLOAD_BUDGET);
        }
    }
    if (!_pending && _resources.pages.empty() && !beginInitialChange()) {
        stopLoading();
        return;
    }
    // One flow serves initial warmup, streaming transitions and invalidated
    // materials. Only warmup blocks on IO and ignores the per-frame page budget.
    for (;;) {
        if (!prepareResources(!_ready)) {
            return;
        }
        if (_pending) {
            commitChange();
        }
        if (!beginNextChange()) {
            _ready = true;
            return;
        }
    }
}
} // namespace cc::landscape

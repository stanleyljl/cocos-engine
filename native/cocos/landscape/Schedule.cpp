// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Schedule.h"

#include <algorithm>
#include "base/Log.h"
#include "landscape/Asset.h"
#include "landscape/Residency.h"
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

LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, LandscapeResidency &residency, VTRenderer &vt, Quadtree &quadtree)
: _asset(asset), _residency(residency), _vt(vt), _data(asset.data()),
  _layout(_data.sectorSize), _quadtree(quadtree) {}

bool LandscapeSchedule::beginInitialChange() {
    Transaction transaction;
    // Bootstrap uses the same admission/prepare/commit path as later changes.
    // Every sector starts with one coarse material page and complete coverage.
    for (const auto &entry : _quadtree.nodes()) {
        const auto address = entry.second.geometry.address();
        Quadtree::Node node;
        node.geometry = _quadtree.geometry(address);
        node.material.pages.push_back({_layout.rootLevel(), address.x, address.z});
        transaction.topology.added.push_back(address);
        transaction.nodes.push_back(std::move(node));
    }
    return admit(std::move(transaction));
}

Quadtree::Node LandscapeSchedule::desiredNode(NodeAddress address) const {
    Quadtree::Node result;
    result.geometry = _quadtree.geometry(address);
    result.material = _layout.buildGridMapping(_data.nodeRegion(address), result.geometry.minY,
                                         result.geometry.maxY, _quadtree.camera());
    return result;
}

bool LandscapeSchedule::beginNextChange() {
    if (!_needsPlan) {
        return false;
    }
    using Operation = Quadtree::Operation;
    for (const auto &candidate : _quadtree.candidates()) {
        Transaction transaction;
        if (candidate.operation == Operation::REFRESH) {
            auto node = desiredNode(candidate.node);
            if (sameMapping(node.material, _quadtree.nodes().at(candidate.node.key()).material)) {
                continue;
            }
            // Most view changes leave a grid's material mapping unchanged.
            // Build a refresh transaction only after finding an actual change.
            transaction.nodes.push_back(std::move(node));
        }
        if (!_quadtree.plan(candidate, transaction.topology)) {
            continue;
        }
        if (candidate.operation != Operation::REFRESH) {
            for (const auto address : transaction.topology.added) {
                transaction.nodes.push_back(desiredNode(address));
            }
        }
        if (admit(std::move(transaction))) {
            return true;
        }
    }
    // Retry after view changes or a commit releases resources.
    _needsPlan = false;
    return false;
}

LandscapeSchedule::Resources LandscapeSchedule::collectResources(const ccstd::vector<Quadtree::Node> &added) const {
    Resources resources;
    ccstd::unordered_set<uint64_t> sources;
    ccstd::unordered_set<uint64_t> pages;
    const auto collect = [&](const Quadtree::Node &node) {
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
    for (const auto &entry : _quadtree.nodes()) {
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
    if (!_residency.canReserve(resources.sources) || !_vt.canReserve(resources.pages)) {
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
        _vt.selectSources(page, true).appendTo(inputs);
        if (!sourcesFit(pinned, inputs, _residency.layerCount())) {
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
    const auto currentCount = _quadtree.nodes().size();
    const auto &topology = transaction.topology;
    const auto nextCount = topology.removed.empty() ? currentCount
                                                  : currentCount - topology.removed.size() + topology.added.size();
    if (!_vt.pageTable().ensureCapacity(static_cast<uint32_t>(std::max(currentCount, nextCount)))) {
        return false;
    }
    // Validate the entire union before changing source data or VT reservations.
    if (!_vt.reserve(resources.pages) || !_residency.reserve(resources.sources)) {
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
        _vt.selectSources(page, _vt.getRefineFarDetail()).appendTo(inputs);
        if (!sourcesFit(working, inputs, _residency.layerCount())) {
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
    return _residency.ready() && _vt.ready();
}

void LandscapeSchedule::stopLoading() {
    _failed = true;
    CC_LOG_ERROR("[Landscape] Tile loading stopped; re-enable the landscape to reload.");
}

bool LandscapeSchedule::prepareResources(bool synchronous) {
    while (!resourcesReady()) {
        // Preserve the current batch while its IO/uploads are incomplete. This
        // prevents view or material edits from continually replacing in-flight work.
        if (!_residency.ready() && !_residency.loadReserved(synchronous)) {
            stopLoading();
            return false;
        }
        if (!_residency.ready()) {
            return false;
        }
        if (!_residency.reserve(compositionSources()) || !_residency.loadReserved(synchronous)) {
            stopLoading();
            return false;
        }
        const auto budget = synchronous ? config::VT_PAGE_COUNT : _remainingPages;
        const auto composed = _vt.renderPages(_residency, budget);
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
    _quadtree.commit(_pending->topology, _pending->nodes);
    _pending.reset();
    _needsPlan = true;
    // Replaced nodes no longer draw. Keep only committed height sources,
    // material pages and projection references; composition inputs are temporary.
    _resources = collectResources();
    _vt.reserve(_resources.pages);
    _residency.reserve(_resources.sources);
}

void LandscapeSchedule::update(uint32_t frame) {
    _quadtree.clearChanges();
    if (_failed) {
        return;
    }
    if (_asset.loadingFailed()) {
        stopLoading();
        return;
    }
    _needsPlan |= _quadtree.viewChanged();
    if (!_frame || *_frame != frame) {
        _frame = frame;
        _remainingPages = config::VT_PAGE_UPDATE_BUDGET;
        if (_ready) {
            _residency.update(config::PAGE_UPLOAD_BUDGET);
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

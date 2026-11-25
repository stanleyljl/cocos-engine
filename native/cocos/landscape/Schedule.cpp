// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Schedule.h"

#include <algorithm>
#include <utility>
#include "base/Macros.h"
#include "landscape/Asset.h"
#include "landscape/Config.h"
#include "landscape/Quadtree.h"
#include "landscape/Residency.h"
#include "landscape/VTRenderer.h"

namespace cc::landscape {
LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, LandscapeResidency &residency, VTRenderer &vtRenderer, float lodQualityScale)
: _asset(asset)
, _residency(residency)
, _vtRenderer(vtRenderer)
, _targetTree(std::make_unique<TargetTree>())
, _renderTree(std::make_unique<RenderTree>()) {
    _targetTree->init(asset, lodQualityScale);
}

LandscapeSchedule::~LandscapeSchedule() {
    for (auto &entry : _nodes) {
        release(entry.second.address, entry.second.current);
        release(entry.second.address, entry.second.pending);
    }
}

void LandscapeSchedule::update(const Vec3 &camera, const Vec3 &origin) {
    if (_targetTree->updateTarget(camera, origin)) {
        _requestsDirty = true;
        _renderDirty = true;
    }
    if (_requestsDirty && prepareRequests()) {
        _requestsDirty = false;
    }
}

RenderTree::Changes LandscapeSchedule::prepareRenderTree() {
    const bool published = prepareResources();
    RenderTree::Changes changes;
    if (_renderDirty || published) {
        changes = _renderTree->select(*_targetTree, [&](NodeAddress address) { return queryMapping(address); });
    }
    const bool renderChanged = !changes.added.empty() || !changes.updated.empty() || !changes.removed.empty();
    _requestsDirty |= published || renderChanged;
    _renderDirty = _renderTree->empty();
    return changes;
}

void LandscapeSchedule::finishUpdate() {
    // Retire resources only after models stop referring to the previous snapshot.
    bool released = false;
    for (auto it = _nodes.begin(); it != _nodes.end();) {
        if (!_targetTree->find(it->second.address) && !_renderTree->findRenderable(it->second.address)) {
            release(it->second.address, it->second.current);
            release(it->second.address, it->second.pending);
            it = _nodes.erase(it);
            released = true;
        } else {
            ++it;
        }
    }
    _requestsDirty |= released;
    _ready = _ready || isStable();
}

bool LandscapeSchedule::isStable() const {
    if (_requestsDirty || _renderDirty) {
        return false;
    }
    if (std::any_of(_nodes.begin(), _nodes.end(), [](const auto &entry) {
            return !entry.second.pending.pages.empty();
        })) {
        return false;
    }
    if (!_residency.ready() || !_vtRenderer.ready()) {
        return false;
    }
    return !_renderTree->empty();
}

const VTGridMapping *LandscapeSchedule::queryMapping(NodeAddress address) const {
    const auto found = _nodes.find(address.key());
    return found == _nodes.end() || found->second.current.pages.empty() ? nullptr : &found->second.current;
}

ccstd::vector<NodeAddress> LandscapeSchedule::requests() const {
    ccstd::vector<NodeAddress> missing;
    ccstd::vector<NodeAddress> refresh;
    _targetTree->traverse([&](const TargetTree::Node &node) {
        const auto address = node.geometry.address();
        if (address.level > _asset.data().maxTileLevel) {
            return true;
        }
        if (!queryMapping(address)) {
            missing.push_back(address);
            return false;
        }
        if (_renderTree->findRenderable(address) || !node.children) {
            refresh.push_back(address);
        }
        return node.children != nullptr;
    });
    const auto priority = [](NodeAddress a, NodeAddress b) {
        return a.key() < b.key();
    };
    std::sort(missing.begin(), missing.end(), priority);
    std::sort(refresh.begin(), refresh.end(), priority);
    missing.insert(missing.end(), refresh.begin(), refresh.end());
    return missing;
}

bool LandscapeSchedule::prepareRequests() {
    const bool loaded = _residency.ready();
    for (auto it = _nodes.begin(); it != _nodes.end();) {
        auto &node = it->second;
        if (!_targetTree->find(node.address)) {
            release(node.address, node.pending);
        }
        if (node.current.pages.empty() && node.pending.pages.empty()) {
            it = _nodes.erase(it);
        } else {
            ++it;
        }
    }
    // Keep an admitted source batch pinned until it loads; target cancellation is immediate.
    if (!loaded) {
        return false;
    }
    auto resources = collectResources();
    const bool rootsReady = !_renderTree->empty();
    const auto &data = _asset.data();
    const VTPageLayout layout(data.sectorSize);
    auto capacity = _vtRenderer.capacityQuery();
    for (const auto address : requests()) {
        const auto found = _nodes.find(address.key());
        if (found != _nodes.end() && !found->second.pending.pages.empty()) {
            continue;
        }
        const auto *current = queryMapping(address);
        if (!rootsReady && (current || address.level != data.maxTileLevel)) {
            continue;
        }
        VTGridMapping pending;
        if (!current && address.level == data.maxTileLevel) {
            pending.pages = {{layout.rootLevel(), address.x, address.z}};
        } else {
            const auto &geometry = _targetTree->find(address)->geometry;
            pending = layout.buildGridMapping(data, address, geometry.minY, geometry.maxY, _targetTree->localCameraPosition());
        }
        if (current && *current == pending) {
            continue;
        }
        const auto mappingCount = static_cast<uint32_t>(_nodes.size()) + (found == _nodes.end() ? 1U : 0U);
        if (!_vtRenderer.pageTable().canFit(mappingCount)) {
            continue;
        }
        const auto source = address.ancestor(std::max(address.level, data.minTileLevel));
        if (!capacity.canPrepare(pending.pages, {source}, resources)) {
            continue;
        }
        appendResources(resources, address, pending);
        retain(address, pending);
        auto &node = _nodes[address.key()];
        node.address = address;
        node.pending = std::move(pending);
    }
    return true;
}

ResourceRequirements LandscapeSchedule::collectResources() const {
    ResourceRequirements resources;
    for (const auto &entry : _nodes) {
        appendResources(resources, entry.second.address, entry.second.current);
        appendResources(resources, entry.second.address, entry.second.pending);
    }
    return resources;
}

void LandscapeSchedule::appendResources(ResourceRequirements &resources, NodeAddress address, const VTGridMapping &mapping) const {
    if (mapping.pages.empty()) {
        return;
    }
    const auto source = address.ancestor(std::max(address.level, _asset.data().minTileLevel));
    resources.sources.insert(source);
    resources.pages.insert(mapping.pages.begin(), mapping.pages.end());
}

void LandscapeSchedule::retain(NodeAddress address, const VTGridMapping &mapping) {
    _residency.request(address.ancestor(std::max(address.level, _asset.data().minTileLevel)));
    for (const auto page : mapping.pages) {
        _vtRenderer.request(page);
    }
}

void LandscapeSchedule::release(NodeAddress address, VTGridMapping &mapping) {
    if (mapping.pages.empty()) {
        return;
    }
    _residency.release(address.ancestor(std::max(address.level, _asset.data().minTileLevel)));
    for (const auto page : mapping.pages) {
        _vtRenderer.release(page);
    }
    mapping = {};
}

bool LandscapeSchedule::prepareResources() {
    if (!_vtRenderer.pageTable().ensureCapacity(static_cast<uint32_t>(_nodes.size()))) {
        return false;
    }
    // VT submits source demand first; newly uploaded inputs compose on the next frame.
    [[maybe_unused]] const bool pagesUpdated = _vtRenderer.update(config::VT_PAGE_UPDATE_BUDGET);
    CC_ASSERT(pagesUpdated);
    [[maybe_unused]] const bool sourcesUpdated = _residency.update(config::PAGE_UPLOAD_BUDGET);
    CC_ASSERT(sourcesUpdated);
    bool published = false;
    // Releases below are applied by the next resource update, after model synchronization.
    // Parent levels precede children; publication never exposes partial mappings.
    for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
        auto &node = it->second;
        if (node.pending.pages.empty() || _residency.resolve(node.address).layer < 0) {
            continue;
        }
        if (std::any_of(node.pending.pages.begin(), node.pending.pages.end(), [&](VTPageAddress page) { return _vtRenderer.needsComposition(page); })) {
            continue;
        }
        CC_ASSERT(_targetTree->find(node.address));
        CC_ASSERT(node.address.level == _asset.data().maxTileLevel || queryMapping(node.address.ancestor(node.address.level + 1)));
        release(node.address, node.current);
        node.current = std::move(node.pending);
        node.pending = {};
        published = true;
    }
    return published;
}

void LandscapeSchedule::forEachVisibleNode(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const {
    _renderTree->forEachVisibleNode(*_targetTree, frustum, origin, visit);
}

} // namespace cc::landscape

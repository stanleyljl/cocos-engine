// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Schedule.h"

#include <algorithm>
#include <utility>
#include "base/Macros.h"
#include "landscape/Renderer.h"
#include "landscape/Asset.h"
#include "landscape/Config.h"
#include "landscape/Quadtree.h"
#include "landscape/Residency.h"
#include "landscape/VTRenderer.h"

namespace cc::landscape {
LandscapeSchedule::LandscapeSchedule(LandscapeAsset &asset, LandscapeRenderer &renderer, float lodQualityScale)
: _asset(asset)
, _residency(renderer.residency())
, _vtRenderer(renderer.vtRenderer())
, _renderer(renderer)
, _targetTree(std::make_unique<TargetTree>())
, _renderTree(std::make_unique<RenderTree>()) {
    _targetTree->init(asset, lodQualityScale);
    ccstd::vector<Vec4> morph(config::MAX_LOD_LEVELS);
    const auto &ranges = _targetTree->lodMorphRanges();
    for (size_t i = 0; i < std::min(ranges.size(), morph.size()); ++i) {
        morph[i] = Vec4{ranges[i].start, ranges[i].end, 0, 0};
    }
    renderer.setLodMorphRanges(morph);
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
    _renderer.setViewPosition(camera);
    prepareRequests();
}

void LandscapeSchedule::prepareRenderData(uint32_t frame) {
    auto *previousMappings = _vtRenderer.pageTable().texture();
    prepareResources();
    RenderTree::Changes changes;
    if (_renderDirty) {
        changes = _renderTree->select(*_targetTree, [&](NodeAddress address) { return queryMaterialMapping(address); });
        _renderDirty = _renderTree->empty();
        _requestsDirty |= !changes.added.empty() || !changes.removed.empty();
    }
    const auto renderNodes = [&](const ccstd::vector<uint64_t> &keys) {
        ccstd::vector<LandscapeRenderNode> nodes;
        nodes.reserve(keys.size());
        for (const auto key : keys) {
            const auto &node = *_renderTree->findRenderable(NodeAddress::fromKey(key));
            nodes.push_back({&node.geometry, &node.material, node.edgeMask});
        }
        return nodes;
    };
    const auto added = renderNodes(changes.added);
    const auto updated = renderNodes(changes.updated);
    _renderer.prepareRenderData(frame, previousMappings, added, updated, changes.removed);
    // Retire resources only after models stop referring to the previous snapshot.
    for (auto it = _nodes.begin(); it != _nodes.end();) {
        if (!_targetTree->find(it->second.address) && !_renderTree->findRenderable(it->second.address)) {
            release(it->second.address, it->second.current);
            release(it->second.address, it->second.pending);
            it = _nodes.erase(it);
            _requestsDirty = true;
        } else {
            ++it;
        }
    }
    _ready = _ready || isRenderStateStable();
}

void LandscapeSchedule::collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const {
    _renderTree->traverse([&](const RenderTree::Node &node) {
        return _renderer.collectNodeModels(node.geometry, _targetTree->region(node.geometry.address()), node.renderable, frustum, shadow, models);
    });
}

bool LandscapeSchedule::isRenderStateStable() const {
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

const VTGridMapping *LandscapeSchedule::queryMaterialMapping(NodeAddress address) const {
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
        if (!queryMaterialMapping(address)) {
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

void LandscapeSchedule::prepareRequests() {
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
    if (!_requestsDirty || !loaded) {
        return;
    }
    _requestsDirty = false;
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
        const auto *current = queryMaterialMapping(address);
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
        auto candidate = resources;
        appendResources(candidate, address, pending);
        const auto mappingCount = static_cast<uint32_t>(_nodes.size()) + (found == _nodes.end() ? 1U : 0U);
        if (!capacity.canPrepare(candidate.pages, candidate.sources) || !_vtRenderer.pageTable().canFit(mappingCount)) {
            continue;
        }
        resources = std::move(candidate);
        retain(address, pending);
        auto &node = _nodes[address.key()];
        node.address = address;
        node.pending = std::move(pending);
    }
}

LandscapeSchedule::ResourceRequirements LandscapeSchedule::collectResources() const {
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
    if (std::none_of(resources.sources.begin(), resources.sources.end(), [&](NodeAddress entry) { return entry.key() == source.key(); })) {
        resources.sources.push_back(source);
    }
    for (const auto page : mapping.pages) {
        const auto position = std::lower_bound(resources.pages.begin(), resources.pages.end(), page, [](VTPageAddress left, VTPageAddress right) { return left.key() < right.key(); });
        if (position == resources.pages.end() || position->key() != page.key()) {
            resources.pages.insert(position, page);
        }
    }
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

void LandscapeSchedule::prepareResources() {
    if (!_vtRenderer.pageTable().ensureCapacity(static_cast<uint32_t>(_nodes.size()))) {
        return;
    }
    // VT submits source demand first; newly uploaded inputs compose on the next frame.
    [[maybe_unused]] const bool pagesUpdated = _vtRenderer.update(config::VT_PAGE_UPDATE_BUDGET);
    CC_ASSERT(pagesUpdated);
    [[maybe_unused]] const bool sourcesUpdated = _residency.update(config::PAGE_UPLOAD_BUDGET);
    CC_ASSERT(sourcesUpdated);
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
        CC_ASSERT(node.address.level == _asset.data().maxTileLevel || queryMaterialMapping(node.address.ancestor(node.address.level + 1)));
        release(node.address, node.current);
        node.current = std::move(node.pending);
        node.pending = {};
        _requestsDirty = true;
        _renderDirty = true;
    }
}

void LandscapeSchedule::forEachVisibleNode(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const {
    _renderTree->forEachVisibleNode(*_targetTree, frustum, origin, visit);
}

} // namespace cc::landscape

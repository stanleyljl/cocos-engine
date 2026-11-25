/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/
#include "landscape/Quadtree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include "base/Log.h"
#include "base/StringUtil.h"
#include "landscape/Asset.h"
#include "landscape/Config.h"
#include "core/geometry/AABB.h"
#include "core/geometry/Frustum.h"

namespace cc::landscape {
namespace {

float pointAABBMinDistanceSquared(const Vec3 &point, const LandscapeRegion &region, float minY, float maxY) {
    const float x = std::max({region.x - point.x, point.x - region.x - region.size, 0.0F});
    const float y = std::max({minY - point.y, point.y - maxY, 0.0F});
    const float z = std::max({region.z - point.z, point.z - region.z - region.size, 0.0F});
    return x * x + y * y + z * z;
}

float pointAABBMaxDistanceSquared(const Vec3 &point, const LandscapeRegion &region, float minY, float maxY) {
    const float x = std::max(std::abs(region.x - point.x), std::abs(region.x + region.size - point.x));
    const float y = std::max(std::abs(minY - point.y), std::abs(maxY - point.y));
    const float z = std::max(std::abs(region.z - point.z), std::abs(region.z + region.size - point.z));
    return x * x + y * y + z * z;
}

} // namespace

void TargetTree::init(const LandscapeAsset &asset, float lodQualityScale) {
    _data = asset.data();
    _heightRanges = asset.heightRanges();
    _root = std::make_unique<Node>();
    uint32_t rootLevel = _data.maxTileLevel;
    for (uint32_t side = 1; side < std::max(_data.sectorsX, _data.sectorsZ); side *= 2) {
        ++rootLevel;
    }
    buildRoot(*_root, {rootLevel, 0, 0});
    initLodMorphRanges(lodQualityScale);
}

bool TargetTree::updateTarget(const Vec3 &worldCameraPosition, const Vec3 &worldOrigin) {
    const Vec3 localCameraPosition = worldCameraPosition - worldOrigin;
    if (!_needsUpdate && localCameraPosition == _localCameraPosition && worldOrigin == _worldOrigin) {
        return false;
    }
    _localCameraPosition = localCameraPosition;
    _worldOrigin = worldOrigin;
    updateTargetNode(*_root);
    _needsUpdate = false;
    return true;
}

const TargetTree::Node *TargetTree::find(NodeAddress address) const {
    if (!contains(address)) {
        return nullptr;
    }
    if (address.level > _root->geometry.level) {
        return nullptr;
    }
    const Node *node = _root.get();
    while (node->geometry.level > address.level) {
        if (!node->children) {
            return nullptr;
        }
        const auto shift = node->geometry.level - 1 - address.level;
        const auto quadrant = ((address.x >> shift) & 1U) | (((address.z >> shift) & 1U) << 1U);
        node = &node->children[quadrant];
    }
    return node;
}

LandscapeRegion TargetTree::region(NodeAddress address) const {
    const float size = std::ldexp(_data.sectorSize, static_cast<int>(address.level) - static_cast<int>(_data.maxTileLevel));
    return {address.x * size, address.z * size, size};
}

bool TargetTree::morphRangeTooSmall(const QuadNode &node) const {
    const auto address = node.address();
    if (address.level == _data.maxTileLevel) {
        return false;
    }
    const float range = _lodMorphRanges[address.level].end;
    if (distanceSquared(address) > range * range) {
        return false;
    }
    // Use the geometry bounds, including the source height range used during morphing.
    const auto area = region(address);
    const float distanceSq = pointAABBMaxDistanceSquared(_localCameraPosition, area, node.minY, node.maxY);
    const float start = _lodMorphRanges[address.level + 1].start;
    return distanceSq > start * start;
}

float TargetTree::distanceSquared(NodeAddress address) const {
    const auto bounds = heightRange(address);
    const auto area = region(address);
    return pointAABBMinDistanceSquared(_localCameraPosition, area, bounds.minY, bounds.maxY);
}

void TargetTree::traverse(const std::function<bool(const Node &)> &visit) const {
    traverseNode(static_cast<const Node &>(*_root), visit);
}

void TargetTree::initLodMorphRanges(float lodQualityScale) {
    _lodMorphRanges.assign(static_cast<size_t>(_data.maxTileLevel) + 1, MorphRange{});

    float totalWeight = 0.0F;
    float weight = 1.0F;
    for (uint32_t level = 0; level <= _data.maxTileLevel; ++level) {
        totalWeight += weight;
        weight *= config::LOD_DISTANCE_RATIO;
    }
    const float section = std::hypot(_data.width(), _data.depth()) * lodQualityScale / totalWeight;
    float end = 0.0F;
    float start = 0.0F;
    weight = 1.0F;
    for (auto &range : _lodMorphRanges) {
        end += section * weight;
        start += (end - start) * config::MORPH_START_RATIO;
        range = {start, end};
        weight *= config::LOD_DISTANCE_RATIO;
    }

#if CC_LANDSCAPE_DEBUG
    ccstd::string message = "[Landscape] LOD ranges:";
    for (size_t level = 0; level < _lodMorphRanges.size(); ++level) {
        message += StringUtil::format(" [LOD%u: %.1f,%.1f]", static_cast<uint32_t>(level), _lodMorphRanges[level].start, _lodMorphRanges[level].end);
    }
    CC_LOG_INFO("%s", message.c_str());
#endif
}

void TargetTree::buildRoot(Node &node, NodeAddress address) {
    node.geometry = {address.level, address.x, address.z, 0.0F, 0.0F};
    if (!contains(address)) {
        return;
    }
    if (address.level == _data.maxTileLevel) {
        node.geometry = geometry(address);
        return;
    }
    // Management nodes aggregate bounds but own no height or material resources.
    node.children = std::make_unique<Node[]>(4);
    HeightRange bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    for (uint32_t q = 0; q < 4; ++q) {
        const auto addressOfChild = address.child(q);
        buildRoot(node.children[q], addressOfChild);
        if (contains(addressOfChild)) {
            bounds.minY = std::min(bounds.minY, node.children[q].geometry.minY);
            bounds.maxY = std::max(bounds.maxY, node.children[q].geometry.maxY);
        }
    }
    node.geometry.minY = bounds.minY;
    node.geometry.maxY = bounds.maxY;
}

void TargetTree::updateTargetNode(Node &node) {
    const auto address = node.geometry.address();
    if (!contains(address)) {
        return;
    }
    bool refine = address.level > _data.maxTileLevel;
    // Distance alone determines refinement; rotating a view must not discard
    // ready detail or change the target used to balance neighboring draws.
    if (!refine && address.level > 0) {
        const float range = _lodMorphRanges[address.level - 1].end;
        for (uint32_t q = 0; q < 4; ++q) {
            if (distanceSquared(address.child(q)) <= range * range) {
                refine = true;
                break;
            }
        }
    }
    if (!refine) {
        node.children.reset();
        return;
    }
    if (!node.children) {
        node.children = std::make_unique<Node[]>(4);
        for (uint32_t q = 0; q < 4; ++q) {
            node.children[q].geometry = geometry(address.child(q));
        }
    }
    for (uint32_t q = 0; q < 4; ++q) {
        updateTargetNode(node.children[q]);
    }
}

bool TargetTree::contains(NodeAddress address) const {
    if (address.level <= _data.maxTileLevel) {
        const auto sector = address.ancestor(_data.maxTileLevel);
        return sector.x < _data.sectorsX && sector.z < _data.sectorsZ;
    }
    const auto shift = address.level - _data.maxTileLevel;
    return (static_cast<uint64_t>(address.x) << shift) < _data.sectorsX &&
           (static_cast<uint64_t>(address.z) << shift) < _data.sectorsZ;
}

QuadNode TargetTree::geometry(NodeAddress address) const {
    auto bounds = heightRange(address);
    if (address.level < _data.minTileLevel) {
        const auto source = heightRange(address.ancestor(_data.minTileLevel));
        bounds.minY = std::min(bounds.minY, source.minY);
        bounds.maxY = std::max(bounds.maxY, source.maxY);
    }
    return {address.level, address.x, address.z, bounds.minY, bounds.maxY};
}

HeightRange TargetTree::heightRange(NodeAddress address) const {
    if (address.level > _data.maxTileLevel) {
        const auto &bounds = find(address)->geometry;
        return {bounds.minY, bounds.maxY};
    }
    return _heightRanges[address.level][static_cast<size_t>(address.z) * _data.nodesX(address.level) + address.x];
}

void TargetTree::traverseNode(const Node &node, const std::function<bool(const Node &)> &visit) const {
    if (!contains(node.geometry.address())) {
        return;
    }
    if (!visit(node)) {
        return;
    }
    if (!node.children) {
        return;
    }
    for (uint32_t q = 0; q < 4; ++q) {
        traverseNode(static_cast<const Node &>(node.children[q]), visit);
    }
}

RenderTree::Changes RenderTree::select(const TargetTree &target, const MaterialMappingQuery &queryMapping) {
    Changes changes;
    const auto &data = target.data();
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            if (!queryMapping({data.maxTileLevel, x, z})) {
                return changes;
            }
        }
    }
    auto root = std::make_unique<Node>();
    ccstd::vector<Node *> current;
    buildRoots(*root, target.root(), target, queryMapping, current);
    while (!current.empty()) {
        ccstd::vector<Node *> next;
        for (auto *node : current) {
            const auto *desired = target.find(node->geometry.address());
            if (!desired->children) {
                continue;
            }
            std::array<const VTGridMapping *, 4> mappings{};
            for (uint32_t q = 0; q < 4; ++q) {
                mappings[q] = queryMapping(desired->children[q].geometry.address());
            }
            if (std::any_of(mappings.begin(), mappings.end(), [](const VTGridMapping *mapping) { return mapping == nullptr; })) {
                continue;
            }
            if ((computeEdgeMask(*root, node->geometry.address(), data) & 0x55U) != 0) {
                continue;
            }
            node->renderable = false;
            node->material = {};
            node->children = std::make_unique<Node[]>(4);
            for (uint32_t q = 0; q < 4; ++q) {
                auto &child = node->children[q];
                child.geometry = desired->children[q].geometry;
                child.material = *mappings[q];
                child.valid = true;
                child.renderable = true;
                next.push_back(&child);
            }
        }
        current = std::move(next);
    }
    ccstd::unordered_map<uint64_t, const Node *> renderableNodes;
    traverseNode(*root, [&](Node &node) {
        if (!node.renderable) {
            return true;
        }
        const auto address = node.geometry.address();
        node.edgeMask = computeEdgeMask(*root, address, data);
        const auto *previous = findRenderable(address);
        if (!previous) {
            changes.added.push_back(address.key());
        } else if (previous->edgeMask != node.edgeMask || previous->material != node.material) {
            changes.updated.push_back(address.key());
        }
        renderableNodes.emplace(address.key(), &node);
        return false;
    });
    for (const auto &entry : _renderableNodes) {
        if (renderableNodes.count(entry.first) == 0) {
            changes.removed.push_back(entry.first);
        }
    }
    _root = std::move(root);
    _renderableNodes = std::move(renderableNodes);
    checkMorphRanges(target);
    return changes;
}

void RenderTree::buildRoots(Node &node, const TargetTree::Node &target, const TargetTree &tree, const MaterialMappingQuery &queryMapping, ccstd::vector<Node *> &roots) const {
    node.geometry = target.geometry;
    const auto address = node.geometry.address();
    node.valid = tree.contains(address);
    if (!node.valid) {
        return;
    }
    if (address.level == tree.data().maxTileLevel) {
        node.material = *queryMapping(address);
        node.renderable = true;
        roots.push_back(&node);
        return;
    }
    node.children = std::make_unique<Node[]>(4);
    for (uint32_t q = 0; q < 4; ++q) {
        buildRoots(node.children[q], target.children[q], tree, queryMapping, roots);
    }
}

const RenderTree::Node *RenderTree::findRenderable(NodeAddress address) const {
    const auto found = _renderableNodes.find(address.key());
    return found == _renderableNodes.end() ? nullptr : found->second;
}

void RenderTree::forEachRenderableNode(const std::function<void(const Node &)> &visit) const {
    traverse([&](const Node &node) {
        if (node.renderable) {
            visit(node);
        }
        return true;
    });
}

void RenderTree::traverse(const std::function<bool(const Node &)> &visit) const {
    if (_root) {
        traverseNode(*_root, visit);
    }
}

void RenderTree::traverseNode(Node &node, const std::function<bool(Node &)> &visit) const {
    if (!node.valid) {
        return;
    }
    if (!visit(node)) {
        return;
    }
    if (!node.children) {
        return;
    }

    for (uint32_t q = 0; q < 4; ++q) {
        traverseNode(node.children[q], visit);
    }
}

NodeAddress RenderTree::renderableAt(const Node &root, uint32_t x, uint32_t z) const {
    const Node *node = &root;
    while (!node->renderable) {
        const auto shift = node->geometry.level - 1;
        const auto quadrant = ((x >> shift) & 1U) | (((z >> shift) & 1U) << 1U);
        node = &node->children[quadrant];
    }
    return node->geometry.address();
}

uint8_t RenderTree::computeEdgeMask(const Node &root, NodeAddress address, const LandscapeData &data) const {
    const uint32_t width = data.sectorsX << data.maxTileLevel;
    const uint32_t depth = data.sectorsZ << data.maxTileLevel;
    // In the balanced cut, a whole edge meets one same/coarser node or two
    // children one level finer. One sample per edge determines its morph mask.
    const uint32_t gridX = address.x << address.level;
    const uint32_t gridZ = address.z << address.level;
    const uint32_t size = 1U << address.level;
    uint8_t edges{0};
    const auto addEdge = [&](NodeAddress neighbor, uint32_t edge) {
        if (neighbor.level > address.level) {
            edges |= static_cast<uint8_t>(1U << (edge * 2));
        } else if (neighbor.level < address.level) {
            edges |= static_cast<uint8_t>(2U << (edge * 2));
        }
    };
    // Left, right, bottom, top; outer terrain boundaries have no neighbor.
    if (gridX > 0) {
        addEdge(renderableAt(root, gridX - 1, gridZ), 0);
    }
    if (gridX + size < width) {
        addEdge(renderableAt(root, gridX + size, gridZ), 1);
    }
    if (gridZ > 0) {
        addEdge(renderableAt(root, gridX, gridZ - 1), 2);
    }
    if (gridZ + size < depth) {
        addEdge(renderableAt(root, gridX, gridZ + size), 3);
    }
    return edges;
}

void RenderTree::forEachVisibleNode(const TargetTree &target, const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const {
    traverse([&](const RenderTree::Node &treeNode) {
        const auto &node = treeNode.geometry;
        const auto region = target.region(node.address());
        const geometry::AABB bounds{
            region.x + region.size * 0.5F + origin.x, (node.minY + node.maxY) * 0.5F + origin.y, region.z + region.size * 0.5F + origin.z,
            region.size * 0.5F, (node.maxY - node.minY) * 0.5F, region.size * 0.5F
        };
        if (!bounds.aabbFrustum(frustum)) {
            return false;
        }
        if (treeNode.renderable) {
            visit(node, region);
        }
        return true;
    });
}

void RenderTree::checkMorphRanges(const TargetTree &target) {
#if CC_LANDSCAPE_DEBUG
    bool morphRangeWarning = false;
    forEachRenderableNode([&](const RenderTree::Node &node) {
        if (morphRangeWarning) {
            return;
        }
        morphRangeWarning = target.morphRangeTooSmall(node.geometry);
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

} // namespace cc::landscape

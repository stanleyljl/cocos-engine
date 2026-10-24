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
#include <cmath>
#include <limits>
#include <utility>

#include "core/geometry/Frustum.h"
#include "landscape/Asset.h"

namespace cc {
namespace landscape {

bool Quadtree::init(const LandscapeAsset &asset, float lodQualityScale) {
    return asset.valid() && init(asset.data(), asset.heightRanges(), lodQualityScale);
}

bool Quadtree::init(const LandscapeData &data, const ccstd::vector<HeightRange> &heightRanges, float lodQualityScale) {
    if (!data.valid() || lodQualityScale <= 0.0F) {
        return false;
    }
    _data = data;
    _nodeIndexLayout = NodeIndexLayout(data);
    if (heightRanges.size() != _nodeIndexLayout.nodeCount()) {
        return false;
    }
    _heightRanges = heightRanges;
    _nodes.clear();
    _changed.clear();
    _rootRanges.clear();
    _passes.clear();
    _passCount = 0;
    _rootLevel = data.maxLevel;
    uint32_t sectors = 1;
    while (sectors < std::max(data.sectorsX, data.sectorsZ)) {
        sectors *= 2;
        ++_rootLevel;
    }
    buildRoot(root());
    computeRanges(lodQualityScale);
    return true;
}

uint8_t Quadtree::childMask(NodeAddress node) const {
    if (node.level == 0) {
        return 0;
    }
    if (node.level <= _data.maxLevel) {
        return config::ALL_QUADRANTS;
    }
    uint8_t mask = 0;
    const auto shift = node.level - 1 - _data.maxLevel;
    for (uint32_t q = 0; q < 4; ++q) {
        const auto next = child(node, q);
        if ((next.x << shift) < _data.sectorsX && (next.z << shift) < _data.sectorsZ) {
            mask |= static_cast<uint8_t>(1U << q);
        }
    }
    return mask;
}

LandscapeRegion Quadtree::region(NodeAddress node) const {
    const float size = std::ldexp(_data.sectorSize, static_cast<int>(node.level) - static_cast<int>(_data.maxLevel));
    return {node.x * size, node.z * size, size};
}

HeightRange Quadtree::heightRange(NodeAddress node) const {
    if (node.level > _data.maxLevel) {
        return _rootRanges.at(node.key());
    }
    return _heightRanges[_nodeIndexLayout.globalNodeIndex(node.level, node.x, node.z)];
}

HeightRange Quadtree::buildRoot(NodeAddress node) {
    if (node.level == _data.maxLevel) {
        Node leaf;
        leaf.geometry = geometry(node);
        _nodes.emplace(node.key(), std::move(leaf));
        return heightRange(node);
    }
    HeightRange bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    const auto mask = childMask(node);
    for (uint32_t q = 0; q < 4; ++q) {
        if (mask & (1U << q)) {
            const auto next = buildRoot(child(node, q));
            bounds.minY = std::min(bounds.minY, next.minY);
            bounds.maxY = std::max(bounds.maxY, next.maxY);
        }
    }
    _rootRanges.emplace(node.key(), bounds);
    return bounds;
}

QuadNode Quadtree::geometry(NodeAddress node) const {
    auto bounds = heightRange(node);
    if (node.level < _data.minTileLevel) {
        const auto source = heightRange(node.ancestor(_data.minTileLevel));
        bounds.minY = std::min(bounds.minY, source.minY);
        bounds.maxY = std::max(bounds.maxY, source.maxY);
    }
    return {node.level, node.x, node.z, bounds.minY, bounds.maxY};
}

void Quadtree::beginUpdate(const Vec3 &camera, const Vec3 &worldOrigin) {
    const Vec3 local = camera - worldOrigin;
    _viewChanged = local != _camera || worldOrigin != _worldOrigin;
    _camera = local;
    _worldOrigin = worldOrigin;
    _passCount = 0;
}

void Quadtree::includePass(const geometry::Frustum &frustum, bool preloadNearby) {
    if (_passCount == _passes.size()) {
        _passes.emplace_back();
        _viewChanged = true;
    }
    auto &pass = _passes[_passCount++];
    _viewChanged |= pass.preloadNearby != preloadNearby;
    pass.preloadNearby = preloadNearby;
    for (size_t i = 0; i < pass.planes.size(); ++i) {
        const auto &plane = *frustum.planes[i];
        const Vec4 value{plane.n.x, plane.n.y, plane.n.z, plane.d};
        _viewChanged |= pass.planes[i] != value;
        pass.planes[i] = value;
    }
}

void Quadtree::endUpdate() {
    _viewChanged |= _passCount != _passes.size();
    _passes.resize(_passCount);
}

float Quadtree::distance(NodeAddress node) const {
    const auto bounds = heightRange(node);
    const auto area = region(node);
    const float x = std::max({area.x - _camera.x, _camera.x - area.x - area.size, 0.0F});
    const float y = std::max({bounds.minY - _camera.y, _camera.y - bounds.maxY, 0.0F});
    const float z = std::max({area.z - _camera.z, _camera.z - area.z - area.size, 0.0F});
    return x * x + y * y + z * z;
}

bool Quadtree::visible(NodeAddress node, bool colorOnly) const {
    const float minDistance = distance(node);
    const float range = _lodRange[std::min(node.level, _data.maxLevel)];
    if (minDistance > range * range) {
        return false;
    }
    const float preload = _lodRange[std::min(1U, _data.maxLevel)];
    const auto bounds = heightRange(node);
    const auto area = region(node);
    const Vec3 center{area.x + area.size * 0.5F + _worldOrigin.x, (bounds.minY + bounds.maxY) * 0.5F + _worldOrigin.y, area.z + area.size * 0.5F + _worldOrigin.z};
    const Vec3 extent{area.size * 0.5F, (bounds.maxY - bounds.minY) * 0.5F, area.size * 0.5F};
    for (const auto &pass : _passes) {
        if (colorOnly && !pass.preloadNearby) {
            continue;
        }
        if (pass.preloadNearby && minDistance <= preload * preload) {
            return true;
        }
        // Same conservative six-plane test as AABB::aabbFrustum. Store plane
        // values so rotating/replacing a pass invalidates settled scheduling.
        const bool intersects = std::all_of(pass.planes.begin(), pass.planes.end(), [&](const Vec4 &plane) {
            const float radius = extent.x * std::abs(plane.x) + extent.y * std::abs(plane.y) + extent.z * std::abs(plane.z);
            return center.x * plane.x + center.y * plane.y + center.z * plane.z + radius >= plane.w;
        });
        if (intersects) {
            return true;
        }
    }
    return false;
}

bool Quadtree::needsSplit(NodeAddress node) const {
    if (node.level == 0 || !visible(node, false)) {
        return false;
    }
    // Inspect only the next level. Loading never creates a separate target tree.
    for (uint32_t q = 0; q < 4; ++q) {
        if (visible(child(node, q), false)) {
            return true;
        }
    }
    return false;
}

ccstd::vector<Quadtree::Candidate> Quadtree::candidates() {
    ccstd::vector<Candidate> result;
    _visibilityDistanceTooSmall = false;
    cull([&](NodeAddress node, bool leaf) {
        if (node.level > _data.maxLevel) {
            return true;
        }
        const bool refine = needsSplit(node);
        const bool color = visible(node, true);
        const float nearDistance = distance(node);
        if (leaf) {
            if (refine) {
                result.push_back({node, Operation::SPLIT, color, nearDistance});
            }
            result.push_back({node, Operation::REFRESH, color, nearDistance});
            if (node.level < _data.maxLevel && visible(node, false)) {
                const auto bounds = heightRange(node);
                const auto area = region(node);
                const float x = std::max(std::abs(area.x - _camera.x), std::abs(area.x + area.size - _camera.x));
                const float y = std::max(std::abs(bounds.minY - _camera.y), std::abs(bounds.maxY - _camera.y));
                const float z = std::max(std::abs(area.z - _camera.z), std::abs(area.z + area.size - _camera.z));
                const float morphStart = _lodMorphStart[node.level + 1];
                _visibilityDistanceTooSmall |= x * x + y * y + z * z > morphStart * morphStart;
            }
        } else if (!refine && hasLeafChildren(node)) {
            result.push_back({node, Operation::MERGE, color, nearDistance});
        }
        return true;
    });
    std::sort(result.begin(), result.end(), [](const Candidate &a, const Candidate &b) {
        if ((a.operation == Operation::MERGE) != (b.operation == Operation::MERGE)) {
            return a.operation == Operation::MERGE;
        }
        if (a.visible != b.visible) {
            return a.visible;
        }
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        if (a.operation != b.operation) {
            return a.operation < b.operation;
        }
        return a.node.key() < b.node.key();
    });
    return result;
}

void Quadtree::computeRanges(float lodQualityScale) {
    const size_t levelCount = static_cast<size_t>(_data.maxLevel) + 1U;
    _lodRange.assign(levelCount, 0.0F);
    _lodMorphStart.assign(levelCount, 0.0F);
    _lodMorphEnd.assign(levelCount, 0.0F);

    float totalWeight = 0.0F;
    float currentWeight = 1.0F;
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        totalWeight += currentWeight;
        currentWeight *= config::LOD_DISTANCE_RATIO;
    }

    // Horizontal terrain diagonal, independent of height range and camera far clip.
    // A larger scale keeps finer LODs visible farther away.
    const float visibilityDistance = std::hypot(_data.width(), _data.depth()) * lodQualityScale;
    const float section = visibilityDistance / totalWeight;
    float previousRange = 0.0F;
    currentWeight = 1.0F;
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        _lodRange[level] = previousRange + section * currentWeight;
        previousRange = _lodRange[level];
        currentWeight *= config::LOD_DISTANCE_RATIO;
    }

    float previousMorphStart = 0.0F;
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        _lodMorphEnd[level] = _lodRange[level];
        _lodMorphStart[level] = previousMorphStart +
                                (_lodMorphEnd[level] - previousMorphStart) * config::MORPH_START_RATIO;
        previousMorphStart = _lodMorphStart[level];
    }
}

NodeAddress Quadtree::child(NodeAddress node, uint32_t quadrant) {
    return {node.level - 1, node.x * 2 + (quadrant & 1), node.z * 2 + (quadrant >> 1)};
}

bool Quadtree::hasLeafChildren(NodeAddress node) const {
    if (node.level == 0) {
        return false;
    }
    for (uint32_t q = 0; q < 4; ++q) {
        if (!_nodes.count(child(node, q).key())) {
            return false;
        }
    }
    return true;
}

bool Quadtree::plan(Candidate candidate, Change &change) const {
    change = {};
    const auto node = candidate.node;
    if (node.level > _data.maxLevel) {
        return false;
    }
    if (candidate.operation == Operation::REFRESH) {
        if (!_nodes.count(node.key())) {
            return false;
        }
        change.added.push_back(node);
        return true;
    }
    if (candidate.operation == Operation::MERGE) {
        if (!hasLeafChildren(node)) {
            return false;
        }
        bool balanced = true;
        neighbors(node, [&](NodeAddress neighbor, uint32_t) {
            balanced &= neighbor.level + 1 >= node.level;
        });
        if (!balanced) {
            return false;
        }
        for (uint32_t q = 0; q < 4; ++q) {
            change.removed.push_back(child(node, q));
        }
        change.added.push_back(node);
        return true;
    }
    if (node.level == 0 || !_nodes.count(node.key())) {
        return false;
    }
    // Splitting a leaf may require splitting coarser neighbors first. Compute
    // that closure against the unchanged current cut, then commit it atomically.
    ccstd::unordered_set<uint64_t> included{node.key()};
    ccstd::vector<NodeAddress> splits{node};
    for (size_t i = 0; i < splits.size(); ++i) {
        const auto parent = splits[i];
        neighbors(parent, [&](NodeAddress neighbor, uint32_t) {
            if (neighbor.level > parent.level && included.insert(neighbor.key()).second) {
                splits.push_back(neighbor);
            }
        });
    }
    if (_nodes.size() + 3 * splits.size() > 8192) {
        return false;
    }
    change.removed = std::move(splits);
    for (const auto parent : change.removed) {
        for (uint32_t q = 0; q < 4; ++q) {
            change.added.push_back(child(parent, q));
        }
    }
    return true;
}

void Quadtree::commit(const Change &change, ccstd::vector<Node> &prepared) {
    // Mark old and new neighbors: both sides of a changed LOD boundary need
    // refreshed morph masks, even when their geometry and materials stay put.
    for (const auto node : change.removed) {
        _changed.insert(node.key());
        neighbors(node, [this](NodeAddress neighbor, uint32_t) {
            _changed.insert(neighbor.key());
        });
    }
    for (const auto node : change.removed) {
        _nodes.erase(node.key());
    }
    for (auto &node : prepared) {
        const auto key = node.geometry.address().key();
        _nodes.insert_or_assign(key, std::move(node));
        _changed.insert(key);
    }
    if (!change.removed.empty()) {
        for (const auto node : change.added) {
            neighbors(node, [this](NodeAddress neighbor, uint32_t) {
                _changed.insert(neighbor.key());
            });
        }
    }
}

NodeAddress Quadtree::at(uint32_t x, uint32_t z) const {
    NodeAddress node{_rootLevel, x >> _rootLevel, z >> _rootLevel};
    while (!_nodes.count(node.key()) && node.level > 0) {
        --node.level;
        node.x = x >> node.level;
        node.z = z >> node.level;
    }
    return node;
}

Quadtree::EdgeMasks Quadtree::edgeMasks(NodeAddress node) const {
    EdgeMasks masks;
    neighbors(node, [&](NodeAddress neighbor, uint32_t edge) {
        if (neighbor.level > node.level) {
            masks.coarser |= static_cast<uint8_t>(1U << edge);
        } else if (neighbor.level < node.level) {
            masks.finer |= static_cast<uint8_t>(1U << edge);
        }
    });
    return masks;
}

} // namespace landscape
} // namespace cc

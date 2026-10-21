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
#include <utility>

#include "core/geometry/AABB.h"
#include "core/geometry/Frustum.h"
#include "landscape/LandscapeAsset.h"

namespace cc {
namespace landscape {

namespace {
float distanceToAABB(const Vec3 &point, const geometry::AABB &box) {
    const Vec3 &center = box.getCenter();
    const Vec3 &halfExtents = box.getHalfExtents();
    const float dx = std::max(0.0F, std::abs(point.x - center.x) - halfExtents.x);
    const float dy = std::max(0.0F, std::abs(point.y - center.y) - halfExtents.y);
    const float dz = std::max(0.0F, std::abs(point.z - center.z) - halfExtents.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float maxDistanceToAABB(const Vec3 &point, const geometry::AABB &box) {
    const Vec3 &center = box.getCenter();
    const Vec3 &halfExtents = box.getHalfExtents();
    const float dx = std::abs(point.x - center.x) + halfExtents.x;
    const float dy = std::abs(point.y - center.y) + halfExtents.y;
    const float dz = std::abs(point.z - center.z) + halfExtents.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

Quadtree::Quadtree() {
    _box = ccnew geometry::AABB();
}

Quadtree::~Quadtree() = default;

bool Quadtree::init(const LandscapeAsset &asset, float lodQualityScale) {
    if (!asset.valid() || lodQualityScale <= 0.0F) {
        return false;
    }
    _data = asset.data();
    _selected.clear();
    _nodeIndexLayout = NodeIndexLayout(_data);
    const auto &heightRanges = asset.heightRanges();
    if (heightRanges.size() != _nodeIndexLayout.nodeCount()) {
        return false;
    }
    _heightRanges = heightRanges;
    computeRanges(lodQualityScale);
    return true;
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

void Quadtree::nodeHeightRange(uint32_t level, uint32_t ix, uint32_t iz, float &minY, float &maxY) const {
    minY = _data.minHeight();
    maxY = _data.maxHeight();
    if (_heightRanges.empty() || _sectorX >= _data.sectorsX || _sectorZ >= _data.sectorsZ ||
        level > _data.maxLevel) {
        return;
    }
    const size_t index = _nodeIndexLayout.sectorNodeIndex(_sectorX, _sectorZ, level, ix, iz);
    if (index >= _heightRanges.size()) {
        return;
    }
    const HeightRange &range = _heightRanges[index];
    minY = range.minY;
    maxY = range.maxY;
}

const ccstd::vector<QuadNode> &Quadtree::select(const Vec3 &camPos, const geometry::Frustum &frustum, const Vec3 &landscapeWorldOrigin, uint32_t sectorX, uint32_t sectorZ, bool preloadNearby) {
    _camPos = camPos;
    _frustum = &frustum;
    _landscapeWorldOrigin = landscapeWorldOrigin;
    _sectorX = sectorX;
    _sectorZ = sectorZ;
    _selected.clear();
    _visibilityDistanceTooSmall = false;
    if (!_data.valid() || sectorX >= _data.sectorsX || sectorZ >= _data.sectorsZ) {
        return _selected;
    }
    // Keep the two nearest LOD bands ready in every direction. This is a
    // residency demand only; actual color/shadow draws still use their frustum.
    _preloadDistance = preloadNearby ? _lodRange[std::min(1U, _data.maxLevel)] : -1.0F;
    traverse(_data.maxLevel, 0U, 0U);
    return _selected;
}

Quadtree::SelectResult Quadtree::traverse(uint32_t level, uint32_t ix, uint32_t iz) {
    const float size = _data.nodeSize(level);
    float minY = _data.minHeight();
    float maxY = _data.maxHeight();
    nodeHeightRange(level, ix, iz, minY, maxY);
    const auto address = _data.nodeInSector(_sectorX, _sectorZ, level, ix, iz);
    const auto region = _data.nodeRegion(address);
    const float x = region.x + _landscapeWorldOrigin.x;
    const float z = region.z + _landscapeWorldOrigin.z;
    _box->setCenter(x + size * 0.5F, (minY + maxY) * 0.5F + _landscapeWorldOrigin.y, z + size * 0.5F);
    _box->setHalfExtents(size * 0.5F, (maxY - minY) * 0.5F, size * 0.5F);

    const float minDistance = distanceToAABB(_camPos, *_box);
    if (minDistance > _preloadDistance && !_box->aabbFrustum(*_frustum)) {
        return SelectResult::CULLED;
    }

    if (minDistance > _lodRange[level]) {
        return SelectResult::OUT_OF_RANGE;
    }
    const float maxDistance = maxDistanceToAABB(_camPos, *_box);
    const auto addSelection = [&](uint8_t quadrantMask) {
        _selected.push_back(QuadNode{level, address.x, address.z, minY, maxY, quadrantMask});
        if (level < _data.maxLevel && maxDistance > _lodMorphStart[level + 1U]) {
            _visibilityDistanceTooSmall = true;
        }
    };

    if (level == 0U || minDistance > _lodRange[level - 1U]) {
        addSelection(config::ALL_QUADRANTS);
        return SelectResult::SELECTED;
    }

    uint8_t parentMask = 0U;
    for (uint32_t dz = 0; dz < 2U; ++dz) {
        for (uint32_t dx = 0; dx < 2U; ++dx) {
            const uint32_t quadrant = dz * 2U + dx;
            const auto childResult = traverse(level - 1U, ix * 2U + dx, iz * 2U + dz);
            if (childResult == SelectResult::OUT_OF_RANGE) {
                parentMask |= static_cast<uint8_t>(1U << quadrant);
            }
        }
    }
    if (parentMask != 0U) {
        addSelection(parentMask);
    }
    return SelectResult::SELECTED;
}

void ExpectedTree::include(const ccstd::vector<QuadNode> &nodes, bool colorPass) {
    for (const auto &node : nodes) {
        auto address = node.address();
        while (address.level < _maxLevel) {
            address = address.ancestor(address.level + 1);
            auto &priority = _demands[address.key()].refinement;
            priority = std::max(priority, _maxLevel - node.level + 1);
        }
        if (!colorPass) {
            continue;
        }
        // Visibility is scheduling priority, not draw culling. Preserve conservative
        // overlap for partially selected nodes and their descendants.
        address = node.address();
        _demands[address.key()].covered = true;
        for (;;) {
            _demands[address.key()].containsVisible = true;
            if (address.level == _maxLevel) {
                break;
            }
            address = address.ancestor(address.level + 1);
        }
    }
}

uint32_t ExpectedTree::refinementPriority(NodeAddress node) const {
    const auto it = _demands.find(node.key());
    return it == _demands.end() ? 0 : it->second.refinement;
}

bool ExpectedTree::visible(NodeAddress node) const {
    const auto it = _demands.find(node.key());
    if (it != _demands.end() && it->second.containsVisible) {
        return true;
    }
    while (node.level < _maxLevel) {
        node = node.ancestor(node.level + 1);
        const auto ancestor = _demands.find(node.key());
        if (ancestor != _demands.end() && ancestor->second.covered) {
            return true;
        }
    }
    return false;
}

void CurrentTree::init(const LandscapeData &data) {
    _data = data;
    _nodes.clear();
    _changed.clear();
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            Node node;
            node.geometry = {data.maxLevel, x, z, data.minHeight(), data.maxHeight()};
            _nodes.emplace(node.geometry.address().key(), std::move(node));
        }
    }
}

NodeAddress CurrentTree::child(NodeAddress node, uint32_t quadrant) {
    return {node.level - 1, node.x * 2 + (quadrant & 1), node.z * 2 + (quadrant >> 1)};
}

bool CurrentTree::hasLeafChildren(NodeAddress node) const {
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

ccstd::vector<CurrentTree::Candidate> CurrentTree::candidates(const ExpectedTree &expected) const {
    ccstd::vector<Candidate> result;
    cull([&](NodeAddress node, bool leaf) {
        const bool refine = expected.refinementPriority(node) != 0;
        if (leaf) {
            if (refine) {
                result.push_back({node, Operation::SPLIT});
            }
            // A refused split must not prevent this leaf's material from updating.
            result.push_back({node, Operation::REFRESH});
        } else if (!refine && hasLeafChildren(node)) {
            result.push_back({node, Operation::MERGE});
        }
        return true;
    });
    return result;
}

bool CurrentTree::plan(Candidate candidate, Change &change) const {
    change = {};
    const auto node = candidate.node;
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

void CurrentTree::commit(const Change &change, ccstd::vector<Node> &prepared) {
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

NodeAddress CurrentTree::at(uint32_t x, uint32_t z) const {
    NodeAddress node{_data.maxLevel, x >> _data.maxLevel, z >> _data.maxLevel};
    while (!_nodes.count(node.key()) && node.level > 0) {
        --node.level;
        node.x = x >> node.level;
        node.z = z >> node.level;
    }
    return node;
}

CurrentTree::EdgeMasks CurrentTree::edgeMasks(NodeAddress node) const {
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

/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

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

#pragma once

#include <algorithm>
#include <cstddef>
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"

#include "base/Ptr.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeData.h"
#include "math/Vec3.h"

namespace cc {
namespace geometry {
class AABB;
class Frustum;
} // namespace geometry

namespace landscape {

class LandscapeAsset;

struct QuadNode {
    // Always global node coordinates, including Quadtree's selection output.
    uint32_t level{0};
    uint32_t ix{0};
    uint32_t iz{0};
    float minY{0.0F};
    float maxY{0.0F};
    uint8_t quadrantMask{config::ALL_QUADRANTS};
    NodeAddress address() const { return {level, ix, iz}; }
};

class Quadtree {
public:
    Quadtree();
    ~Quadtree();

    // Copies the metadata needed for selection; does not load or retain the asset.
    bool init(const LandscapeAsset &asset, float lodQualityScale = 1.0F);

    // Inputs are world space; output QuadNodes always use global node coordinates.
    // Each pass traverses from the root with its own frustum and the same LOD position.
    // Color residency can additionally keep the nearest two LOD bands in all
    // directions. Callers must still cull actual draws against the pass frustum.
    const ccstd::vector<QuadNode> &select(const Vec3 &camPos, const geometry::Frustum &frustum,
                                          const Vec3 &landscapeWorldOrigin, uint32_t sectorX, uint32_t sectorZ,
                                          bool preloadNearby = false);

    const ccstd::vector<float> &lodMorphStart() const { return _lodMorphStart; }
    const ccstd::vector<float> &lodMorphEnd() const { return _lodMorphEnd; }
    bool visibilityDistanceTooSmall() const { return _visibilityDistanceTooSmall; }

private:
    enum class SelectResult {
        CULLED,       // Outside the frustum; the parent must not fill this area.
        OUT_OF_RANGE, // The parent must draw this quadrant instead.
        SELECTED,     // Covered by this node or its descendants.
    };

    struct HeightRange {
        float minY{0.0F};
        float maxY{0.0F};
    };

    void computeRanges(float lodQualityScale);
    SelectResult traverse(uint32_t level, uint32_t ix, uint32_t iz);
    void nodeHeightRange(uint32_t level, uint32_t ix, uint32_t iz, float &minY, float &maxY) const;

    LandscapeData _data;

    Vec3 _camPos;
    Vec3 _landscapeWorldOrigin;
    uint32_t _sectorX{0};
    uint32_t _sectorZ{0};
    const geometry::Frustum *_frustum{nullptr};
    float _preloadDistance{-1.0F};
    IntrusivePtr<geometry::AABB> _box;
    ccstd::vector<QuadNode> _selected;
    NodeRangeLayout _nodeLayout;
    ccstd::vector<HeightRange> _heightRanges;
    ccstd::vector<float> _lodRange;
    ccstd::vector<float> _lodMorphStart;
    ccstd::vector<float> _lodMorphEnd;
    bool _visibilityDistanceTooSmall{false};
};

// One committed draw cut, independent of the distance selector and resource
// caches. A transition stores only replaced leaves, never a second forest.
class RenderSelection {
public:
    struct Change {
        ccstd::vector<NodeAddress> removed, added;
        bool empty() const { return removed.empty() && added.empty(); }
    };
    struct Request {
        NodeAddress node;
        bool split;
        uint32_t priority;
    };
    void init(const LandscapeData &data) {
        _data = data;
        _leaves.clear();
        _refine.clear();
        for (uint32_t z = 0; z < data.sectorsZ; ++z) {
            for (uint32_t x = 0; x < data.sectorsX; ++x) {
                _leaves.insert(NodeAddress{data.maxLevel, x, z}.key());
            }
        }
    }
    const ccstd::unordered_set<uint64_t> &leaves() const { return _leaves; }
    // Index each target's path once: request construction is O(targets * depth),
    // instead of comparing every committed leaf against every target patch.
    void target(const ccstd::vector<QuadNode> &nodes) {
        _refine.clear();
        for (const auto &n : nodes) {
            auto parent = n.address();
            while (parent.level < _data.maxLevel) {
                parent = parent.ancestor(parent.level + 1);
                auto &priority = _refine[parent.key()];
                priority = std::max(priority, _data.maxLevel - n.level + 1);
            }
        }
    }
    ccstd::vector<Request> requests() const {
        ccstd::vector<Request> result;
        ccstd::unordered_set<uint64_t> parents;
        for (const auto key : _leaves) {
            const auto node = NodeAddress::fromKey(key);
            const auto wanted = _refine.find(key);
            if (wanted != _refine.end()) {
                result.push_back({node, true, wanted->second});
            }
            if (node.level < _data.maxLevel) {
                const auto parent = node.ancestor(node.level + 1);
                if (!_refine.count(parent.key()) && parents.insert(parent.key()).second) {
                    result.push_back({parent, false, 0});
                }
            }
        }
        std::sort(result.begin(), result.end(), [](const Request &a, const Request &b) {
            if (a.split != b.split) {
                return !a.split;
            }
            if (!a.split && a.node.level != b.node.level) {
                return a.node.level < b.node.level;
            }
            if (a.priority != b.priority) {
                return a.priority > b.priority;
            }
            return a.node.key() < b.node.key();
        });
        return result;
    }
    bool plan(const Request &request, Change &change) const {
        change = {};
        const auto node = request.node;
        if (request.split) {
            ccstd::unordered_set<uint64_t> visited;
            if (!split(node, change, visited)) {
                change = {};
                return false;
            }
            if (_leaves.size() + change.added.size() - change.removed.size() > 8192) {
                change = {};
                return false;
            }
            return true;
        }
        if (node.level == 0) {
            return false;
        }
        for (uint32_t q = 0; q < 4; ++q) {
            const NodeAddress child{node.level - 1, node.x * 2 + (q & 1), node.z * 2 + (q >> 1)};
            if (!_leaves.count(child.key())) {
                return false;
            }
            change.removed.push_back(child);
        }
        bool balanced = true;
        neighbors(node, [&](NodeAddress n, uint32_t) { balanced &= n.level + 1 >= node.level; });
        if (!balanced) {
            change = {};
            return false;
        }
        change.added.push_back(node);
        return true;
    }
    void commit(const Change &change) {
        for (const auto n : change.removed) {
            _leaves.erase(n.key());
        }
        for (const auto n : change.added) {
            _leaves.insert(n.key());
        }
    }
    NodeAddress at(uint32_t x, uint32_t z) const {
        NodeAddress n{_data.maxLevel, x >> _data.maxLevel, z >> _data.maxLevel};
        while (!_leaves.count(n.key()) && n.level > 0) {
            --n.level;
            n.x = x >> n.level;
            n.z = z >> n.level;
        }
        return n;
    }
    template <class Visit>
    void neighbors(NodeAddress node, const Visit &visit) const {
        const uint32_t x = node.x << node.level, z = node.z << node.level, size = 1U << node.level;
        const uint32_t width = _data.sectorsX << _data.maxLevel, depth = _data.sectorsZ << _data.maxLevel;
        for (uint32_t edge = 0; edge < 4; ++edge) {
            if ((edge == 0 && x == 0) || (edge == 1 && x + size == width) ||
                (edge == 2 && z == 0) || (edge == 3 && z + size == depth)) {
                continue;
            }
            uint32_t cursor = edge < 2 ? z : x;
            const uint32_t end = cursor + size;
            while (cursor < end) {
                const auto n = edge < 2 ? at(edge == 0 ? x - 1 : x + size, cursor)
                                        : at(cursor, edge == 2 ? z - 1 : z + size);
                visit(n, edge);
                cursor = ((edge < 2 ? n.z : n.x) + 1U) << n.level;
            }
        }
    }
    uint8_t edgeMask(NodeAddress node) const {
        uint8_t mask = 0;
        neighbors(node, [&](NodeAddress n, uint32_t edge) {
            if (n.level > node.level) {
                mask |= static_cast<uint8_t>(1U << edge);
            }
        });
        return mask;
    }
    // Visibility belongs to the committed cut, not to target selections. Coarse
    // reconstruction can be visible even when all desired fine bounds are culled.
    // The predicate must conservatively bound the region and all its descendants.
    template <class Intersects, class Visit>
    void cull(const Intersects &intersects, const Visit &visit) const {
        for (uint32_t z = 0; z < _data.sectorsZ; ++z) {
            for (uint32_t x = 0; x < _data.sectorsX; ++x) {
                cullNode({_data.maxLevel, x, z}, intersects, visit);
            }
        }
    }

private:
    template <class Intersects, class Visit>
    void cullNode(NodeAddress node, const Intersects &intersects, const Visit &visit) const {
        if (!intersects(node)) {
            return;
        }
        if (_leaves.count(node.key())) {
            visit(node);
            return;
        }
        for (uint32_t q = 0; q < 4; ++q) {
            cullNode({node.level - 1, node.x * 2 + (q & 1), node.z * 2 + (q >> 1)}, intersects, visit);
        }
    }

    bool split(NodeAddress node, Change &change, ccstd::unordered_set<uint64_t> &visited) const {
        if (node.level == 0 || !_leaves.count(node.key())) {
            return false;
        }
        if (!visited.insert(node.key()).second) {
            return true;
        }
        bool valid = true;
        neighbors(node, [&](NodeAddress n, uint32_t) {
            if (n.level > node.level) {
                valid &= split(n, change, visited);
            }
        });
        if (!valid) {
            return false;
        }
        change.removed.push_back(node);
        for (uint32_t q = 0; q < 4; ++q) {
            change.added.push_back({node.level - 1, node.x * 2 + (q & 1), node.z * 2 + (q >> 1)});
        }
        return true;
    }
    LandscapeData _data;
    ccstd::unordered_set<uint64_t> _leaves;
    ccstd::unordered_map<uint64_t, uint32_t> _refine;
};

} // namespace landscape
} // namespace cc

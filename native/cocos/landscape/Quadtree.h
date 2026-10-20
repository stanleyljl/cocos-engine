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
#include <utility>
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"

#include "base/Ptr.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeData.h"
#include "landscape/VirtualTexture.h"
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

    // Copies the already decoded height ranges; does not load or retain the asset.
    bool init(const LandscapeAsset &asset, float lodQualityScale = 1.0F);

    // Inputs are world space; output QuadNodes always use global node coordinates.
    // Each pass traverses from the root with its own frustum and the same LOD position.
    // Color residency can additionally keep the nearest two LOD bands in all
    // directions. Callers must still cull actual draws against the pass frustum.
    const ccstd::vector<QuadNode> &select(const Vec3 &camPos, const geometry::Frustum &frustum, const Vec3 &landscapeWorldOrigin, uint32_t sectorX, uint32_t sectorZ, bool preloadNearby = false);

    const ccstd::vector<float> &lodMorphStart() const { return _lodMorphStart; }
    const ccstd::vector<float> &lodMorphEnd() const { return _lodMorphEnd; }
    bool visibilityDistanceTooSmall() const { return _visibilityDistanceTooSmall; }

private:
    enum class SelectResult {
        CULLED,       // Outside the frustum; the parent must not fill this area.
        OUT_OF_RANGE, // The parent must draw this quadrant instead.
        SELECTED,     // Covered by this node or its descendants.
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
    NodeIndexLayout _nodeIndexLayout;
    ccstd::vector<HeightRange> _heightRanges;
    ccstd::vector<float> _lodRange;
    ccstd::vector<float> _lodMorphStart;
    ccstd::vector<float> _lodMorphEnd;
    bool _visibilityDistanceTooSmall{false};
};

// Desired geometry and view priorities. It owns no resident resources.
class ExpectedTree {
public:
    explicit ExpectedTree(uint32_t maxLevel = 0) : _maxLevel(maxLevel) {}
    // Index each target's path once: request construction is O(targets * depth),
    // instead of comparing every committed leaf against every target node.
    bool target(const ccstd::vector<QuadNode> &nodes) {
        if (matches(nodes)) {
            return false;
        }
        _nodes = nodes;
        _refine.clear();
        for (const auto &n : nodes) {
            auto parent = n.address();
            while (parent.level < _maxLevel) {
                parent = parent.ancestor(parent.level + 1);
                auto &priority = _refine[parent.key()];
                priority = std::max(priority, _maxLevel - n.level + 1);
            }
        }
        return true;
    }
    bool matches(const ccstd::vector<QuadNode> &nodes) const {
        return _nodes.size() == nodes.size() && std::equal(_nodes.begin(), _nodes.end(), nodes.begin(), [](const QuadNode &a, const QuadNode &b) {
            return a.address().key() == b.address().key() && a.quadrantMask == b.quadrantMask;
        });
    }
    const ccstd::unordered_map<uint64_t, uint32_t> &refinements() const { return _refine; }
    bool setView(const ccstd::vector<QuadNode> &surface, const Vec3 &camera) {
        const bool moved = _camera.x != camera.x || _camera.y != camera.y || _camera.z != camera.z;
        _camera = camera;
        const bool changed = _surface.size() != surface.size() ||
                             !std::equal(_surface.begin(), _surface.end(), surface.begin(), [](const QuadNode &a, const QuadNode &b) {
                                 return a.address().key() == b.address().key() && a.quadrantMask == b.quadrantMask;
                             });
        if (!changed) {
            return moved;
        }
        _surface = surface;
        _visiblePaths.clear();
        _visibleNodes.clear();
        for (const auto &node : surface) {
            auto address = node.address();
            _visibleNodes.insert(address.key());
            for (;;) {
                _visiblePaths.insert(address.key());
                if (address.level == _maxLevel) {
                    break;
                }
                address = address.ancestor(address.level + 1);
            }
        }
        return true;
    }
    bool visible(NodeAddress node) const {
        if (_visiblePaths.count(node.key())) {
            return true;
        }
        while (node.level < _maxLevel) {
            node = node.ancestor(node.level + 1);
            if (_visibleNodes.count(node.key())) {
                return true;
            }
        }
        return false;
    }
    const Vec3 &camera() const { return _camera; }

private:
    uint32_t _maxLevel;
    ccstd::vector<QuadNode> _nodes;
    ccstd::unordered_map<uint64_t, uint32_t> _refine;
    ccstd::vector<QuadNode> _surface;
    ccstd::unordered_set<uint64_t> _visiblePaths;
    ccstd::unordered_set<uint64_t> _visibleNodes;
    Vec3 _camera;
};

// The committed tree is the single owner of geometry and material mappings.
// Planning never changes it. LandscapeSchedule fills a change's node records
// and prepares their resources before committing topology and materials together.
class CurrentTree {
public:
    struct Node {
        QuadNode geometry;
        VTGridMapping material;
    };
    struct Change {
        ccstd::vector<NodeAddress> removed;
        // New leaves or replacements of existing material mappings.
        ccstd::unordered_map<uint64_t, Node> nodes;
        bool empty() const { return removed.empty() && nodes.empty(); }
    };
    struct Request {
        NodeAddress node;
        bool split;
        uint32_t priority;
    };
    struct EdgeMasks {
        uint8_t coarser{0};
        uint8_t finer{0};
    };
    void init(const LandscapeData &data) {
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
    const ccstd::unordered_map<uint64_t, Node> &nodes() const { return _nodes; }
    const ccstd::unordered_set<uint64_t> &changedNodes() const { return _changed; }
    void clearChanges() { _changed.clear(); }
    // Topology differences only; LandscapeSchedule owns their processing order.
    ccstd::vector<Request> requests(const ExpectedTree &expected) const {
        const auto &refine = expected.refinements();
        ccstd::vector<Request> result;
        ccstd::unordered_set<uint64_t> parents;
        for (const auto &entry : _nodes) {
            const auto key = entry.first;
            const auto node = NodeAddress::fromKey(key);
            const auto wanted = refine.find(key);
            if (wanted != refine.end()) {
                result.push_back({node, true, wanted->second});
            }
            if (node.level < _data.maxLevel) {
                const auto parent = node.ancestor(node.level + 1);
                if (!refine.count(parent.key()) && parents.insert(parent.key()).second) {
                    result.push_back({parent, false, 0});
                }
            }
        }
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
            if (_nodes.size() + change.nodes.size() - change.removed.size() > 8192) {
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
            if (!_nodes.count(child.key())) {
                change = {};
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
        addNode(change, node);
        return true;
    }
    void commit(Change &change) {
        for (const auto n : change.removed) {
            _changed.insert(n.key());
            neighbors(n, [this](NodeAddress neighbor, uint32_t) {
                _changed.insert(neighbor.key());
            });
        }
        for (const auto n : change.removed) {
            _nodes.erase(n.key());
        }
        for (auto &entry : change.nodes) {
            _nodes.insert_or_assign(entry.first, std::move(entry.second));
            _changed.insert(entry.first);
        }
        if (!change.removed.empty()) {
            for (const auto &entry : change.nodes) {
                neighbors(NodeAddress::fromKey(entry.first), [this](NodeAddress neighbor, uint32_t) {
                    _changed.insert(neighbor.key());
                });
            }
        }
    }
    NodeAddress at(uint32_t x, uint32_t z) const {
        NodeAddress n{_data.maxLevel, x >> _data.maxLevel, z >> _data.maxLevel};
        while (!_nodes.count(n.key()) && n.level > 0) {
            --n.level;
            n.x = x >> n.level;
            n.z = z >> n.level;
        }
        return n;
    }
    template <class Visit>
    void neighbors(NodeAddress node, const Visit &visit) const {
        const uint32_t x = node.x << node.level;
        const uint32_t z = node.z << node.level;
        const uint32_t size = 1U << node.level;
        const uint32_t width = _data.sectorsX << _data.maxLevel;
        const uint32_t depth = _data.sectorsZ << _data.maxLevel;
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
    // Both sides of an actual LOD boundary constrain geomorph: the fine edge
    // collapses to the coarse lattice, which must stay fixed along that edge.
    EdgeMasks edgeMasks(NodeAddress node) const {
        EdgeMasks masks;
        neighbors(node, [&](NodeAddress n, uint32_t edge) {
            if (n.level > node.level) {
                masks.coarser |= static_cast<uint8_t>(1U << edge);
            } else if (n.level < node.level) {
                masks.finer |= static_cast<uint8_t>(1U << edge);
            }
        });
        return masks;
    }
    // Visibility belongs to the committed cut, not to target selections. Coarse
    // reconstruction can be visible even when all desired fine bounds are culled.
    // Visit each node once. Return false to prune an internal node; leaves
    // collect their models directly, using the committed model bounds.
    template <class Visit>
    void cull(const Visit &visit) const {
        for (uint32_t z = 0; z < _data.sectorsZ; ++z) {
            for (uint32_t x = 0; x < _data.sectorsX; ++x) {
                cullNode({_data.maxLevel, x, z}, visit);
            }
        }
    }

private:
    void addNode(Change &change, NodeAddress address) const {
        Node node;
        // Conservative bounds for topology planning. The scheduler replaces
        // this record with asset bounds and desired materials before admission.
        node.geometry = {address.level, address.x, address.z, _data.minHeight(), _data.maxHeight()};
        change.nodes.emplace(address.key(), std::move(node));
    }

    template <class Visit>
    void cullNode(NodeAddress node, const Visit &visit) const {
        const bool leaf = _nodes.count(node.key()) != 0;
        if (!visit(node, leaf) || leaf) {
            return;
        }
        for (uint32_t q = 0; q < 4; ++q) {
            cullNode({node.level - 1, node.x * 2 + (q & 1), node.z * 2 + (q >> 1)}, visit);
        }
    }

    bool split(NodeAddress node, Change &change, ccstd::unordered_set<uint64_t> &visited) const {
        if (node.level == 0 || !_nodes.count(node.key())) {
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
            addNode(change, {node.level - 1, node.x * 2 + (q & 1), node.z * 2 + (q >> 1)});
        }
        return true;
    }
    LandscapeData _data;
    ccstd::unordered_map<uint64_t, Node> _nodes;
    ccstd::unordered_set<uint64_t> _changed;
};

} // namespace landscape
} // namespace cc

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

#include <array>
#include "base/std/container/vector.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "landscape/Data.h"
#include "math/Vec3.h"
#include "math/Vec4.h"

namespace cc {
namespace geometry {
class Frustum;
} // namespace geometry

namespace landscape {

class LandscapeAsset;

// One balanced rendering tree per Landscape. Distance requests only the next
// local change; the schedule retains current coverage until its resources are ready.
class Quadtree {
public:
    struct Node {
        QuadNode geometry;
        VTGridMapping material;
    };
    enum class Operation {
        MERGE,
        SPLIT,
        REFRESH,
    };
    struct Candidate {
        NodeAddress node;
        Operation operation;
        bool visible{false};
        float distance{0.0F};
    };
    struct Change {
        ccstd::vector<NodeAddress> removed;
        ccstd::vector<NodeAddress> added;
        bool empty() const {
            return added.empty();
        }
    };
    struct EdgeMasks {
        uint8_t coarser{0};
        uint8_t finer{0};
    };

    bool init(const LandscapeAsset &asset, float lodQualityScale = 1.0F);
    void beginUpdate(const Vec3 &camera, const Vec3 &worldOrigin);
    void includePass(const geometry::Frustum &frustum, bool preloadNearby);
    void endUpdate();
    bool viewChanged() const {
        return _viewChanged;
    }
    const Vec3 &camera() const {
        return _camera;
    }
    const ccstd::vector<float> &lodMorphStart() const {
        return _lodMorphStart;
    }
    const ccstd::vector<float> &lodMorphEnd() const {
        return _lodMorphEnd;
    }
    bool visibilityDistanceTooSmall() const {
        return _visibilityDistanceTooSmall;
    }
    NodeAddress root() const {
        return {_rootLevel, 0, 0};
    }
    uint8_t childMask(NodeAddress node) const;
    LandscapeRegion region(NodeAddress node) const;
    QuadNode geometry(NodeAddress node) const;
    const ccstd::unordered_map<uint64_t, Node> &nodes() const {
        return _nodes;
    }
    const ccstd::unordered_set<uint64_t> &changedNodes() const {
        return _changed;
    }
    void clearChanges() {
        _changed.clear();
    }
    ccstd::vector<Candidate> candidates();
    bool plan(Candidate candidate, Change &change) const;
    void commit(const Change &change, ccstd::vector<Node> &prepared);
    NodeAddress at(uint32_t x, uint32_t z) const;
    EdgeMasks edgeMasks(NodeAddress node) const;

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
                const auto neighbor = edge < 2 ? at(edge == 0 ? x - 1 : x + size, cursor)
                                               : at(cursor, edge == 2 ? z - 1 : z + size);
                visit(neighbor, edge);
                cursor = ((edge < 2 ? neighbor.z : neighbor.x) + 1U) << neighbor.level;
            }
        }
    }

    // Traverse current coverage from the single root, skipping absent children.
    // Management branches are always internal and never own geometry/resources.
    template <class Visit>
    void cull(const Visit &visit) const {
        visitNode(root(), visit);
    }

protected:
    // Decoded metadata initialization also supports CPU-only topology validation.
    bool init(const LandscapeData &data, const ccstd::vector<HeightRange> &heightRanges, float lodQualityScale);

private:
    struct Pass {
        std::array<Vec4, 6> planes;
        bool preloadNearby{false};
    };
    template <class Visit>
    void visitNode(NodeAddress node, const Visit &visit) const {
        const bool leaf = _nodes.count(node.key()) != 0;
        if (!visit(node, leaf) || leaf) {
            return;
        }
        const auto mask = childMask(node);
        for (uint32_t q = 0; q < 4; ++q) {
            if (mask & (1U << q)) {
                visitNode(child(node, q), visit);
            }
        }
    }
    static NodeAddress child(NodeAddress node, uint32_t quadrant);
    bool hasLeafChildren(NodeAddress node) const;
    HeightRange buildRoot(NodeAddress node);
    HeightRange heightRange(NodeAddress node) const;
    void computeRanges(float lodQualityScale);
    bool visible(NodeAddress node, bool colorOnly) const;
    bool needsSplit(NodeAddress node) const;
    float distance(NodeAddress node) const;

    LandscapeData _data;
    NodeIndexLayout _nodeIndexLayout;
    ccstd::vector<HeightRange> _heightRanges;
    ccstd::unordered_map<uint64_t, HeightRange> _rootRanges;
    uint32_t _rootLevel{0};
    ccstd::unordered_map<uint64_t, Node> _nodes;
    ccstd::unordered_set<uint64_t> _changed;
    ccstd::vector<float> _lodRange;
    ccstd::vector<float> _lodMorphStart;
    ccstd::vector<float> _lodMorphEnd;
    Vec3 _camera;
    Vec3 _worldOrigin;
    ccstd::vector<Pass> _passes;
    size_t _passCount{0};
    bool _viewChanged{true};
    bool _visibilityDistanceTooSmall{false};
};

} // namespace landscape
} // namespace cc

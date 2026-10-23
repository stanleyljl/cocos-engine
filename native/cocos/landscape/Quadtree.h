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

#include "base/Ptr.h"
#include "base/std/container/vector.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "landscape/LandscapeData.h"
#include "landscape/VTRenderer.h"
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

// A sparse demand tree, not a second set of resident geometry/material records.
// An entry says whether to refine this region and whether color demand overlaps it.
class ExpectedTree {
public:
    explicit ExpectedTree(uint32_t maxLevel = 0) : _maxLevel(maxLevel) {}
    // Accumulate each pass directly. Color demand also sets scheduling visibility;
    // shadow demand only contributes refinement. Repeated nodes are harmless.
    void include(const ccstd::vector<QuadNode> &nodes, bool colorPass);
    bool operator==(const ExpectedTree &other) const { return _maxLevel == other._maxLevel && _demands == other._demands; }
    uint32_t refinementPriority(NodeAddress node) const;
    bool visible(NodeAddress node) const;

private:
    struct Demand {
        uint32_t refinement{0};
        bool containsVisible{false};
        bool covered{false};
        bool operator==(const Demand &other) const {
            return refinement == other.refinement && containsVisible == other.containsVisible && covered == other.covered;
        }
    };
    uint32_t _maxLevel;
    ccstd::unordered_map<uint64_t, Demand> _demands;
};

// The complete, balanced cut currently displayed. Only commit changes it.
// Topology planning contains addresses only; the schedule prepares actual node
// bounds/materials and retains their resources before committing the replacement.
class CurrentTree {
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
    };
    struct Change {
        ccstd::vector<NodeAddress> removed;
        ccstd::vector<NodeAddress> added;
        bool empty() const { return added.empty(); }
    };
    struct EdgeMasks {
        uint8_t coarser{0};
        uint8_t finer{0};
    };

    void init(const LandscapeData &data);
    const ccstd::unordered_map<uint64_t, Node> &nodes() const { return _nodes; }
    const ccstd::unordered_set<uint64_t> &changedNodes() const { return _changed; }
    void clearChanges() { _changed.clear(); }
    // Walk the committed hierarchy once. The schedule ranks these candidates
    // and rejects unchanged materials or replacements that cannot fit.
    ccstd::vector<Candidate> candidates(const ExpectedTree &expected) const;
    bool plan(Candidate candidate, Change &change) const;
    // Prepared records correspond exactly to change.added, with resources ready.
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

    // Pass visibility follows committed coverage, including coarse bounds while
    // a replacement is loading. Returning false prunes the entire subtree.
    template <class Visit>
    void cull(const Visit &visit) const {
        for (uint32_t z = 0; z < _data.sectorsZ; ++z) {
            for (uint32_t x = 0; x < _data.sectorsX; ++x) {
                visitNode({_data.maxLevel, x, z}, visit);
            }
        }
    }

private:
    template <class Visit>
    void visitNode(NodeAddress node, const Visit &visit) const {
        const bool leaf = _nodes.count(node.key()) != 0;
        if (!visit(node, leaf) || leaf) {
            return;
        }
        for (uint32_t q = 0; q < 4; ++q) {
            visitNode(child(node, q), visit);
        }
    }
    static NodeAddress child(NodeAddress node, uint32_t quadrant);
    bool hasLeafChildren(NodeAddress node) const;
    LandscapeData _data;
    ccstd::unordered_map<uint64_t, Node> _nodes;
    ccstd::unordered_set<uint64_t> _changed;
};

} // namespace landscape
} // namespace cc

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
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/Data.h"
#include "math/Vec3.h"

namespace cc::geometry {
class Frustum;
}

namespace cc::landscape {
class LandscapeAsset;

// Distance-based target topology. Resource readiness never changes this tree.
class TargetTree {
public:
    struct MorphRange {
        float start{0.0F};
        float end{0.0F};
    };
    struct Node {
        QuadNode geometry;
        std::unique_ptr<Node[]> children;
    };

    void init(const LandscapeAsset &asset, float lodQualityScale = 1.0F);
    bool updateTarget(const Vec3 &worldCameraPosition, const Vec3 &worldOrigin);
    const Node &root() const { return *_root; }
    const LandscapeData &data() const { return _data; }
    const Node *find(NodeAddress address) const;
    bool contains(NodeAddress address) const;
    LandscapeRegion region(NodeAddress address) const;
    const Vec3 &localCameraPosition() const { return _localCameraPosition; }
    const Vec3 &worldOrigin() const { return _worldOrigin; }
    const ccstd::vector<MorphRange> &lodMorphRanges() const { return _lodMorphRanges; }
    // Diagnose morph constraints for nodes within their LOD distance range.
    bool morphRangeTooSmall(const QuadNode &node) const;
    float distanceSquared(NodeAddress address) const;
    HeightRange heightRange(NodeAddress address) const;
    // Pre-order traversal of valid nodes from the root, excluding out-of-bounds quadrants.
    // Returning false skips only this node's descendants; siblings are still visited.
    void traverse(const std::function<bool(const Node &)> &visit) const;

private:
    void initLodMorphRanges(float lodQualityScale);
    void buildRoot(Node &node, NodeAddress address);
    void updateTargetNode(Node &node);
    QuadNode geometry(NodeAddress address) const;
    void traverseNode(const Node &node, const std::function<bool(const Node &)> &visit) const;

    LandscapeData _data;
    ccstd::vector<ccstd::vector<HeightRange>> _heightRanges;
    ccstd::vector<MorphRange> _lodMorphRanges;
    std::unique_ptr<Node> _root;
    Vec3 _localCameraPosition;
    Vec3 _worldOrigin;
    bool _needsUpdate{true}; // Force the first target update
};

// Committed render snapshot; owns its geometry and mappings independently of the target.
class RenderTree {
public:
    struct Node {
        QuadNode geometry;
        VTGridMapping material;
        std::unique_ptr<Node[]> children;
        uint8_t edgeMask{0}; // Two bits per edge: same/boundary, coarser, finer.
        bool valid{false}; // False for out-of-bounds quadrant placeholders.
        bool renderable{false};
    };
    struct Changes {
        ccstd::vector<uint64_t> added;
        ccstd::vector<uint64_t> updated;
        ccstd::vector<uint64_t> removed;
    };
    // Borrowed ready mapping, or null while unavailable; select copies it into the snapshot.
    using MaterialMappingQuery = std::function<const VTGridMapping *(NodeAddress)>;

    // Missing sector coverage leaves the previous snapshot intact.
    Changes select(const TargetTree &target, const MaterialMappingQuery &queryMapping);
    bool empty() const { return !_root; }
    // Borrowed node; valid until the next committed selection.
    const Node *findRenderable(NodeAddress address) const;
    // Pre-order traversal of valid nodes from the root, including internal management nodes.
    // Returning false skips only this node's descendants; siblings are still visited.
    void traverse(const std::function<bool(const Node &)> &visit) const;
    // Visits every renderable leaf in traversal order, without visibility filtering.
    void forEachRenderableNode(const std::function<void(const Node &)> &visit) const;
    // Visits renderable leaves whose geometry bounds intersect the frustum, in traversal order.
    // Passes each leaf's geometry and terrain-local region; internal nodes are only used for pruning.
    void forEachVisibleNode(const TargetTree &target, const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const;

private:
    // Diagnose each committed selection regardless of frustum visibility.
    void checkMorphRanges(const TargetTree &target);
    void buildRoots(Node &node, const TargetTree::Node &target, const TargetTree &tree, const MaterialMappingQuery &queryMapping, ccstd::vector<Node *> &roots) const;
    NodeAddress renderableAt(const Node &root, uint32_t x, uint32_t z) const;
    uint8_t computeEdgeMask(const Node &root, NodeAddress address, const LandscapeData &data) const;
    void traverseNode(Node &node, const std::function<bool(Node &)> &visit) const;

    std::unique_ptr<Node> _root;
    // Rebuilt with each snapshot; pointers are owned by _root.
    ccstd::unordered_map<uint64_t, const Node *> _renderableNodes;
    bool _lastMorphRangeWarning{false};
};
} // namespace cc::landscape

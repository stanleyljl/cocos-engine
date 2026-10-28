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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "landscape/Config.h"
#include "landscape/Data.h"
#include "math/Vec3.h"
#include "math/Vec4.h"

namespace cc::geometry {
class Frustum;
}

namespace cc::landscape {
class LandscapeAsset;

// Owns the target hierarchy, published node mappings and balanced render selection.
// Schedule supplies completed resources; Renderer consumes selected nodes and changes.
class Quadtree {
public:
    struct MorphRange {
        float start{0.0F};
        float end{0.0F};
    };
    struct Node {
        QuadNode geometry;
        VTGridMapping material;
        std::unique_ptr<Node[]> children; // Target structure: zero or four children.
        bool loaded{false}; // Schedule has loaded and reserved the node's source tile.
        bool ready{false}; // Complete mapping published; resource-owning ancestors are ready.
        bool renderable{false}; // Selected in the complete, balanced render set.
        uint8_t edgeMask{0};// Valid when renderable; Left, right, bottom, top: two bits each, 00 same/boundary, 01 coarser, 10 finer.
    };

    // Landscape validates LOD quality and loads the asset before initialization.
    void init(const LandscapeAsset &asset, float lodQualityScale = 1.0F);
    void beginUpdate(const Vec3 &worldCameraPosition, const Vec3 &worldOrigin);
    void addPass(const geometry::Frustum &frustum, bool includeNearby);
    // Stage 1: update only the distance target. Selection follows resource preparation.
    void updateTarget();
    // Missing nodes whose parents are ready, then displayed/target-leaf refreshes.
    // Each group is ordered by ascending LOD, with node keys breaking ties.
    ccstd::vector<NodeAddress> requests() const;
    // Called only after the complete source/mapping reservation is drawable;
    // publication establishes both loaded and ready, including shared source tiles.
    bool publish(NodeAddress address, VTGridMapping material);
    // Stage 3: select a complete, balanced render set and report draw changes.
    bool selectRenderables();

    // Queries and render-change notifications.
    bool viewChanged() const { return _viewChanged; }
    bool renderSelectionChanged() const { return _renderSelectionChanged; }
    bool sectorRootsReady() const;
    const Vec3 &localCameraPosition() const { return _localCameraPosition; }
    const ccstd::vector<MorphRange> &lodMorphRanges() const { return _lodMorphRanges; }
    // Includes management levels above sector roots, unlike source-tile sizing.
    LandscapeRegion region(NodeAddress address) const;
    const Node *find(NodeAddress address) const;
    Node *find(NodeAddress address);
    const Node *findRenderable(NodeAddress address) const;
    const ccstd::unordered_set<uint64_t> &changedNodes() const { return _changedNodes; }
    void clearChangedNodes() { _changedNodes.clear(); }

    // Traversal helpers. Boolean callbacks return false to skip a node's descendants.
    void forEachNode(const std::function<bool(const Node &)> &visit) const;
    void cull(const std::function<bool(NodeAddress, bool)> &visit) const;
    void forEachRenderable(const std::function<void(const Node &)> &visit) const;

private:
    struct Pass {
        std::array<Vec4, 6> planes;
        bool includeNearby{false}; // Include near-camera nodes in morph diagnostics.
    };

    // Target hierarchy and traversal.
    void initLodMorphRanges(float lodQualityScale);
    void buildRoot(Node &node, NodeAddress address);
    void updateTarget(Node &node);
    static NodeAddress child(NodeAddress address, uint32_t quadrant);
    void visitNodes(const Node &node, const std::function<bool(const Node &)> &visit) const;
    void visitNodes(Node &node, const std::function<bool(Node &)> &visit) const;

    // Bounds include management levels above sector roots.
    bool contains(NodeAddress address) const;
    QuadNode geometry(NodeAddress address) const;
    HeightRange heightRange(NodeAddress address) const;
    float distanceSquared(NodeAddress address) const;
    // Morph helpers; edge sampling requires a balanced render set.
    NodeAddress renderableAt(uint32_t x, uint32_t z) const;
    uint8_t computeEdgeMask(NodeAddress address) const;
#if CC_LANDSCAPE_DEBUG
    // Diagnostics only; never changes selection, morph ranges or edge constraints.
    bool morphRangeTooSmall(const Node &node) const;
#endif

    // Immutable metadata copied from the validated asset.
    LandscapeData _data;
    NodeIndexLayout _nodeIndexLayout;
    ccstd::vector<HeightRange> _heightRanges;
    std::unique_ptr<Node> _root;
    // Snapshot of selected addresses, updated only by selectRenderables. IDs survive
    // target deletion for retirement notifications; edge values remain on the nodes.
    ccstd::unordered_set<uint64_t> _renderNodeKeys;
    // Retained until the next selection so Schedule can rescan after draw syncing.
    bool _renderSelectionChanged{false};
    // Reuse the selected cut until target or readiness changes.
    bool _needsRenderSelection{true};
    // Renderer consumes these keys, including mapping-only updates and removals.
    ccstd::unordered_set<uint64_t> _changedNodes;
    // Fixed LOD transition ranges derived during initialization.
    ccstd::vector<MorphRange> _lodMorphRanges;

    Vec3 _localCameraPosition;
    Vec3 _worldOrigin;
    ccstd::vector<Pass> _passes;
    size_t _activePassCount{0};
    bool _viewChanged{true};
#if CC_LANDSCAPE_DEBUG
    bool _lastMorphRangeWarning{false}; // Log only diagnostic state transitions.
#endif
};
} // namespace cc::landscape

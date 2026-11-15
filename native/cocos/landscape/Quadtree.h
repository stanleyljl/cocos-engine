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
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "landscape/Config.h"
#include "landscape/Data.h"
#include "math/Vec3.h"

namespace cc::landscape {
class LandscapeAsset;

// Owns the target hierarchy, published mappings and balanced render selection.
class Quadtree {
public:
    struct MorphRange {
        float start{0.0F};
        float end{0.0F};
    };
    struct Node {
        QuadNode geometry;
        VTGridMapping material;
        std::unique_ptr<Node[]> _children;
        bool refined{false}; // Target subdivision; children survive until render selection.
        bool ready{false};
        bool renderable{false};
        uint8_t edgeMask{0}; // Left/right/bottom/top: 00 same or boundary, 01 coarser, 10 finer; valid when renderable.
    };

    void init(const LandscapeAsset &asset, float lodQualityScale = 1.0F);

    // Target and render state.
    void updateTarget(const Vec3 &worldCameraPosition, const Vec3 &worldOrigin);
    // Missing coverage before refreshes; each group sorts by LOD, then node key.
    ccstd::vector<NodeAddress> requests() const;
    // Publish only after the source and complete mapping are ready.
    bool publish(NodeAddress address, VTGridMapping material);
    bool selectRenderables();

    // State and changes.
    bool viewChanged() const { return _viewChanged; }
    bool sectorRootsReady() const;
    // Retained until the next selection, independently of changedNodes.
    bool renderSelectionChanged() const { return _renderSelectionChanged; }
    const ccstd::unordered_set<uint64_t> &changedNodes() const { return _changedNodes; }
    void clearChangedNodes() { _changedNodes.clear(); }

    // Node queries.
    const Node *find(NodeAddress address) const;
    const Node *findRenderable(NodeAddress address) const;
    bool isTarget(NodeAddress address) const;
    // Includes management levels above sector roots.
    LandscapeRegion region(NodeAddress address) const;
    const Vec3 &localCameraPosition() const { return _localCameraPosition; }
    const Vec3 &worldOrigin() const { return _worldOrigin; }
    const ccstd::vector<MorphRange> &lodMorphRanges() const { return _lodMorphRanges; }

    // Traversal: boolean callbacks return false to prune descendants.
    void forEachNode(const std::function<bool(const Node &)> &visit) const;
    void forEachRenderable(const std::function<void(const Node &)> &visit) const;
    void cull(const std::function<bool(const Node &)> &visit) const;

private:
    // Hierarchy.
    void initLodMorphRanges(float lodQualityScale);
    void buildRoot(Node &node, NodeAddress address);
    void updateTarget(Node &node);
    static NodeAddress child(NodeAddress address, uint32_t quadrant);
    Node *findNode(NodeAddress address);
    bool contains(NodeAddress address) const;

    // Bounds and morph edges.
    QuadNode geometry(NodeAddress address) const;
    HeightRange heightRange(NodeAddress address) const;
    float distanceSquared(NodeAddress address) const;
    NodeAddress renderableAt(uint32_t x, uint32_t z) const;
    uint8_t computeEdgeMask(NodeAddress address) const;

    void visitNodes(const Node &node, const std::function<bool(const Node &)> &visit) const;
    void visitNodes(Node &node, const std::function<bool(Node &)> &visit) const;

    // Immutable asset metadata.
    LandscapeData _data;
    ccstd::vector<ccstd::vector<HeightRange>> _heightRanges;
    ccstd::vector<MorphRange> _lodMorphRanges;

    std::unique_ptr<Node> _root;
    Vec3 _localCameraPosition;
    Vec3 _worldOrigin;
    bool _viewChanged{true};

    // Previous selection survives target changes until draws are synchronized.
    ccstd::unordered_set<uint64_t> _renderNodeKeys;
    bool _needsRenderSelection{true};
    bool _renderSelectionChanged{false};
    ccstd::unordered_set<uint64_t> _changedNodes;
};
} // namespace cc::landscape

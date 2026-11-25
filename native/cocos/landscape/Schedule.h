// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <functional>
#include <memory>
#include "base/std/container/vector.h"
#include "base/std/container/map.h"
#include "landscape/Quadtree.h"
#include "math/Vec3.h"

namespace cc::geometry {
class Frustum;
}

namespace cc::landscape {
class LandscapeAsset;
class LandscapeResidency;
class VTRenderer;
struct ResourceRequirements;

// Owns target/render trees and coordinates resource requests and publication.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, LandscapeResidency &residency, VTRenderer &vtRenderer, float lodQualityScale);
    ~LandscapeSchedule();

    // Frame updates.
    void update(const Vec3 &camera, const Vec3 &origin);
    RenderTree::Changes prepareRenderTree();
    // Call after Renderer has synchronized models to the prepared tree.
    void finishUpdate();

    const RenderTree &renderTree() const { return *_renderTree; }
    const ccstd::vector<TargetTree::MorphRange> &lodMorphRanges() const { return _targetTree->lodMorphRanges(); }

    // Latched after the first stable render state.
    bool isReady() const { return _ready; }
    // Current capacity-admitted requests are ready and draw selection is stable.
    bool isStable() const;

    void forEachVisibleNode(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const;

private:
    struct NodeResources {
        NodeAddress address;
        VTGridMapping current;
        VTGridMapping pending;
    };

    // Resource requests.
    // Returns false while waiting for the admitted source batch; capacity rejection completes the scan.
    bool prepareRequests();
    ccstd::vector<NodeAddress> requests() const;
    const VTGridMapping *queryMapping(NodeAddress address) const;
    ResourceRequirements collectResources() const;
    void appendResources(ResourceRequirements &resources, NodeAddress address, const VTGridMapping &mapping) const;
    void retain(NodeAddress address, const VTGridMapping &mapping);
    void release(NodeAddress address, VTGridMapping &mapping);

    // Resource preparation.
    // Returns whether a pending material mapping was published.
    bool prepareResources();

    // Borrowed dependencies outlive Schedule; both trees are owned here.
    LandscapeAsset &_asset;
    LandscapeResidency &_residency;
    VTRenderer &_vtRenderer;
    std::unique_ptr<TargetTree> _targetTree;
    std::unique_ptr<RenderTree> _renderTree;

    bool _ready{false};
    // Retry after view/selection changes, cancellation or publication, not capacity rejection.
    bool _requestsDirty{true};
    bool _renderDirty{true};
    // Owns resource lifetimes; RenderTree only owns immutable draw snapshots.
    ccstd::map<uint64_t, NodeResources> _nodes;
};
} // namespace cc::landscape

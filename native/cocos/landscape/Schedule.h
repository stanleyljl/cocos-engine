// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <functional>
#include <memory>
#include "base/std/container/vector.h"
#include "base/std/container/map.h"
#include "landscape/Data.h"
#include "math/Vec3.h"

namespace cc::geometry {
class Frustum;
}
namespace cc::scene {
class Model;
}

namespace cc::landscape {
class LandscapeRenderer;
class LandscapeAsset;
class LandscapeResidency;
class VTRenderer;
class TargetTree;
class RenderTree;

// Coordinates terrain updates, resource readiness and pass collection.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, LandscapeRenderer &renderer, float lodQualityScale);
    ~LandscapeSchedule();

    // Frame updates.
    void update(const Vec3 &camera, const Vec3 &origin);
    void prepareRenderData(uint32_t frame);

    // Pass collection.
    void collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const;

    // Latched after the first stable render state.
    bool isReady() const { return _ready; }
    // Current capacity-admitted requests are ready and draw selection is stable.
    bool isRenderStateStable() const;

    void forEachVisibleNode(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const;

private:
    struct ResourceRequirements {
        // Temporary capacity query; resource ownership lives in NodeResources.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
    };
    struct NodeResources {
        NodeAddress address;
        VTGridMapping current;
        VTGridMapping pending;
    };

    // Resource requests.
    void prepareRequests();
    ResourceRequirements collectResources() const;
    ccstd::vector<NodeAddress> requests() const;
    const VTGridMapping *queryMaterialMapping(NodeAddress address) const;
    void appendResources(ResourceRequirements &resources, NodeAddress address, const VTGridMapping &mapping) const;
    void retain(NodeAddress address, const VTGridMapping &mapping);
    void release(NodeAddress address, VTGridMapping &mapping);

    // Resource preparation.
    void prepareResources();

    // Borrowed dependencies outlive Schedule; both trees are owned here.
    LandscapeAsset &_asset;
    LandscapeResidency &_residency;
    VTRenderer &_vtRenderer;
    LandscapeRenderer &_renderer;
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

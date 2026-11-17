// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include "base/std/container/vector.h"
#include "landscape/Config.h"
#include "landscape/Data.h"
#include "math/Vec4.h"

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
class Quadtree;

// Coordinates terrain updates, resource readiness and pass collection.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, LandscapeRenderer &renderer, float lodQualityScale);
    ~LandscapeSchedule();

    // Frame updates.
    void update(const Vec3 &camera, const Vec3 &origin);
    void prepareRenderData(uint32_t frame);

    // Pass collection.
    void beginPasses();
    void addPass(const geometry::Frustum &frustum, bool includeNearby);
    void collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void endPasses();

    // Latched after the first stable render state.
    bool isReady() const { return _ready; }
    // Current capacity-admitted requests are ready and draw selection is stable.
    bool isRenderStateStable() const;

    void visitDebugNodes(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const;

private:
    struct Resources {
        // Unique sources in collection order; unique pages sorted by key.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
        uint32_t mappingCount{0};
    };
    struct PendingNode {
        NodeAddress node;
        VTGridMapping material; // Replacement; the published mapping remains drawable.
    };
    struct Pass {
        std::array<Vec4, 6> planes;
        bool includeNearby{false}; // Include near-camera nodes in morph diagnostics.
    };

    // Resource requests.
    void prepareRequests();
    Resources collectResources() const;
    void schedulePendingNodes(Resources &resources, bool sectorRootsReady);
    void appendResources(Resources &resources, const PendingNode &pending) const;
    void submitRequests(Resources resources);

    // Resource preparation.
    void prepareResources(uint32_t frame);
    void publishNodes();

    // Morph diagnostics.
    void checkMorphRanges();
    bool isMorphCheckVisible(NodeAddress address) const;

    // Borrowed dependencies outlive Schedule; Quadtree is owned here.
    LandscapeAsset &_asset;
    LandscapeResidency &_residency;
    VTRenderer &_vtRenderer;
    LandscapeRenderer &_renderer;
    std::unique_ptr<Quadtree> _quadtree;

    bool _ready{false};
    std::optional<uint32_t> _lastUpdateFrame;
    // Retry after view/selection changes, cancellation or publication, not capacity rejection.
    bool _requestsDirty{true};
    ccstd::vector<PendingNode> _pendingNodes;
    // Admission snapshot; temporary composition inputs are owned by VT.
    Resources _requestedResources;

    ccstd::vector<Pass> _passes;
    size_t _activePassCount{0};
#if CC_LANDSCAPE_DEBUG
    bool _lastMorphRangeWarning{false};
#endif
};
} // namespace cc::landscape

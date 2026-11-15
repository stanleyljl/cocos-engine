// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include "base/std/container/unordered_map.h"
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

// Owns terrain state and coordinates resource readiness before committing draws.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, LandscapeRenderer &renderer, float lodQualityScale);
    ~LandscapeSchedule();
    void update(const Vec3 &camera, const Vec3 &origin);
    void beginPasses();
    void addPass(const geometry::Frustum &frustum, bool includeNearby);
    void prepareRenderData(uint32_t frame);
    void endPasses();
    void collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void visitDebugNodes(const geometry::Frustum &frustum, const Vec3 &origin, const std::function<void(const QuadNode &, const LandscapeRegion &)> &visit) const;
    bool isReady() const { return _ready; }
    // Current capacity-admitted requests are ready and draw selection is stable.
    bool isRenderStateStable() const;

private:
    struct Pass {
        std::array<Vec4, 6> planes;
        bool includeNearby{false}; // Include near-camera nodes in morph diagnostics.
    };

    struct Resources {
        // Sources retain collection order; pages are sorted by key. Both are unique.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
        uint32_t mappingCount{0};
    };
    struct PendingNode {
        NodeAddress node;
        VTGridMapping material; // Replacement mapping; the node retains its drawable mapping.
    };
    void prepareResources(uint32_t frame);
    bool prepareRequests();
    void checkMorphRanges();
    void composeNodes();
    bool publishNodes();

    // Build a reservation from ready nodes and pending replacements, then admit requests.
    Resources collectResources() const;
    void schedulePendingNodes(Resources &resources, bool sectorRootsReady);
    void appendResources(Resources &resources, const PendingNode &pending) const;
    bool canPrepare(const Resources &resources, ccstd::unordered_map<uint64_t, ccstd::vector<NodeAddress>> &pageSources) const;
    ccstd::vector<NodeAddress> compositionSources() const;
    void stopLoading();
#if CC_LANDSCAPE_DEBUG
    bool morphRangeTooSmall(const QuadNode &node) const;
#endif

    // Borrowed dependencies outlive the scheduler.
    LandscapeAsset &_asset;
    LandscapeResidency &_residency;
    VTRenderer &_vtRenderer;
    LandscapeRenderer &_renderer;
    std::unique_ptr<Quadtree> _quadtree;
    ccstd::vector<Pass> _passes;
    size_t _activePassCount{0};
    // Camera, committed selection and pass changes invalidate diagnostics.
    bool _needsMorphCheck{true};
#if CC_LANDSCAPE_DEBUG
    bool _lastMorphRangeWarning{false}; // Log only diagnostic state transitions.
#endif
    bool _ready{false};

    ccstd::vector<PendingNode> _pendingNodes;
    // Last committed reservation, retained while a source batch loads. Temporary
    // composition inputs are owned by Residency and are not part of this base set.
    Resources _reservedResources;
    // View/selection changes, cancellation and publication request another admission
    // pass. Defer that pass until the source batch completes; capacity rejection alone
    // does not request a retry.
    bool _requestsDirty{true};
    bool _loadingStopped{false};
    std::optional<uint32_t> _lastUpdateFrame;
};
} // namespace cc::landscape

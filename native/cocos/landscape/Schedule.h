// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <optional>
#include "base/std/container/vector.h"
#include "landscape/Data.h"

namespace cc::landscape {
class LandscapeAsset;
class LandscapeResidency;
class VTRenderer;
class Quadtree;

// Owns pending mappings and resource reservations. Loads sources, composes VT
// pages, then publishes complete mappings; Quadtree owns target and draw selection.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, LandscapeResidency &residency, VTRenderer &vtRenderer, Quadtree &quadtree);
    // Run after updateTarget and before selectRenderables, at most once per frame.
    void prepareResources(uint32_t frame);
    // Current capacity-admitted requests have finished loading and composition.
    bool isSettled() const;

private:
    struct Resources {
        // Sources retain collection order; pages are sorted by key. Both are unique.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
    };
    struct PendingNode {
        NodeAddress node;
        VTGridMapping material; // Replacement mapping; the node retains its drawable mapping.
    };
    // Each frame prepares requests, drives asynchronous loading, composes and publishes.
    bool prepareRequests();
    bool loadNodes();
    void composeNodes();
    bool publishNodes();

    // Build a reservation from ready nodes and pending replacements, then admit requests.
    Resources collectResources(uint32_t &mappingCount) const;
    void admitNodes(Resources &resources, uint32_t mappingCount, bool sectorRootsReady);
    void appendResources(Resources &resources, const PendingNode &pending) const;
    bool canPrepare(const Resources &resources) const;
    ccstd::vector<NodeAddress> compositionSources() const;
    void stopLoading();

    // Borrowed dependencies outlive the scheduler.
    LandscapeAsset &_asset;
    LandscapeResidency &_residency;
    VTRenderer &_vtRenderer;
    Quadtree &_quadtree;

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

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <optional>
#include "landscape/Quadtree.h"
#include "landscape/Data.h"

namespace cc::landscape {
class LandscapeAsset;
class LandscapeResidency;
class VTRenderer;

// Prepare the next local Quadtree change without exposing incomplete resources.
// The tree owns coverage and distance decisions; this module owns reservations,
// uploads and VT preparation. Ready changes can advance repeatedly in one update.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, LandscapeResidency &residency, VTRenderer &vt, Quadtree &quadtree);
    void update(uint32_t frame);
    bool ready() const { return _ready; }

private:
    struct Resources {
        // Persistent inputs and output pages protected until commit. Temporary
        // composition inputs are selected afresh from dirty pages during prepare.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
    };
    struct Transaction {
        Quadtree::Change topology;
        ccstd::vector<Quadtree::Node> nodes;
        Resources resources;
    };

    bool beginInitialChange();
    bool beginNextChange();
    bool admit(Transaction transaction);
    bool prepareResources(bool synchronous);
    void commitChange();

    Resources collectResources(const ccstd::vector<Quadtree::Node> &added = {}) const;
    const Resources &activeResources() const;
    bool canPrepare(const Resources &resources) const;
    ccstd::vector<NodeAddress> compositionSources() const;
    bool resourcesReady() const;
    Quadtree::Node desiredNode(NodeAddress node) const;
    void stopLoading();

    LandscapeAsset &_asset;
    LandscapeResidency &_residency;
    VTRenderer &_vt;
    const LandscapeData &_data;
    VTPageLayout _layout;
    Quadtree &_quadtree;
    std::optional<Transaction> _pending;
    Resources _resources;
    bool _needsPlan{true};
    bool _ready{false};
    bool _failed{false};
    std::optional<uint32_t> _frame;
    uint32_t _remainingPages{0};
};
} // namespace cc::landscape

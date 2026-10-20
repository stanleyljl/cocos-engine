// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <optional>
#include "landscape/LandscapeAsset.h"
#include "landscape/Quadtree.h"
#include "landscape/TilePagePool.h"

namespace cc::landscape {
class VTRenderer;

// Advances CurrentTree toward ExpectedTree one prepared local change at a time.
// Admission protects the displayed tree; preparation loads sources and composes
// material pages; commit publishes the change and releases retired resources.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, TilePagePool &tiles, VTRenderer &vt);
    void update(const ccstd::vector<QuadNode> &geometry, const ccstd::vector<QuadNode> &surface, const Vec3 &localCamera, uint32_t frame, bool frozen);
    bool ready() const { return _ready; }
    const CurrentTree &currentTree() const { return _currentTree; }
    QuadNode geometry(NodeAddress address) const;

private:
    enum class RequestType {
        MERGE,
        SPLIT,
        MATERIAL,
    };
    struct Request {
        NodeAddress node;
        RequestType type;
        uint32_t priority{0};
        bool visible{false};
        float distance{0};
    };
    struct Resources {
        // Persistent inputs and output pages protected until commit. Temporary
        // composition inputs are selected afresh from dirty pages during prepare.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
    };

    void buildRequests();
    bool selectNextChange();
    bool admitChange();
    bool prepareResources(bool synchronous);
    void commitChange();

    Resources collectResources() const;
    bool canPrepare(const Resources &resources) const;
    ccstd::vector<NodeAddress> compositionSources() const;
    bool resourcesReady() const;
    float distance(NodeAddress node) const;
    CurrentTree::Node desiredNode(NodeAddress node) const;
    void stopLoading();

    LandscapeAsset &_asset;
    TilePagePool &_tiles;
    VTRenderer &_vt;
    const LandscapeData &_data;
    VTPageLayout _layout;
    ExpectedTree _expectedTree;
    CurrentTree _currentTree;
    CurrentTree::Change _pending;
    Resources _resources;
    ccstd::vector<Request> _requests;
    size_t _requestIndex{0};
    bool _rebuildRequests{true};
    bool _ready{false};
    bool _failed{false};
    std::optional<uint32_t> _frame;
    uint32_t _remainingPages{0};
};
} // namespace cc::landscape

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <optional>
#include "landscape/LandscapeAsset.h"
#include "landscape/Quadtree.h"
#include "landscape/SourceDataManager.h"

namespace cc::landscape {
class VTRenderer;

// Compare demand with committed coverage, admit one replacement, prepare it,
// then commit geometry and materials together. Only an admitted transaction
// survives across updates; candidate ordering is never cached against an old cut.
class LandscapeSchedule {
public:
    LandscapeSchedule(LandscapeAsset &asset, SourceDataManager &sourceData, VTRenderer &vt);
    void update(ExpectedTree expected, const Vec3 &localCamera, uint32_t frame);
    bool ready() const { return _ready; }
    const CurrentTree &currentTree() const { return _currentTree; }
    QuadNode geometry(NodeAddress address) const;

private:
    struct Resources {
        // Persistent inputs and output pages protected until commit. Temporary
        // composition inputs are selected afresh from dirty pages during prepare.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
    };
    struct Transaction {
        CurrentTree::Change topology;
        ccstd::vector<CurrentTree::Node> nodes;
        Resources resources;
    };

    bool beginInitialChange();
    bool beginNextChange();
    bool admit(Transaction transaction);
    bool prepareResources(bool synchronous);
    void commitChange();

    Resources collectResources(const ccstd::vector<CurrentTree::Node> &added = {}) const;
    const Resources &activeResources() const;
    bool canPrepare(const Resources &resources) const;
    ccstd::vector<NodeAddress> compositionSources() const;
    bool resourcesReady() const;
    float distance(NodeAddress node) const;
    CurrentTree::Node desiredNode(NodeAddress node) const;
    void stopLoading();

    LandscapeAsset &_asset;
    SourceDataManager &_sourceData;
    VTRenderer &_vt;
    const LandscapeData &_data;
    VTPageLayout _layout;
    ExpectedTree _expectedTree;
    CurrentTree _currentTree;
    std::optional<Transaction> _pending;
    Resources _resources;
    Vec3 _camera;
    bool _needsPlan{true};
    bool _ready{false};
    bool _failed{false};
    std::optional<uint32_t> _frame;
    uint32_t _remainingPages{0};
};
} // namespace cc::landscape

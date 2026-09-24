// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <memory>
#include "core/TypedArray.h"

namespace cc::landscape {

// Owns only Landscape's streamed PhysX resources. The existing TerrainShape
// continues to provide actor registration, filtering, events and ray hits.
class LandscapeHeightfield final {
public:
    LandscapeHeightfield();
    ~LandscapeHeightfield();
    LandscapeHeightfield(const LandscapeHeightfield &) = delete;
    LandscapeHeightfield &operator=(const LandscapeHeightfield &) = delete;

    uint32_t create(const Uint16Array &samples, uint32_t resolution, uint32_t wrapperObjectID);
    bool adoptShape();
    // Called AFTER TerrainShape::onDestroy removes the actor/event mappings.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace cc::landscape

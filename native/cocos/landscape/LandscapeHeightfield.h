// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <memory>
#include "core/TypedArray.h"

namespace cc::landscape {

// Owns only Landscape's streamed PhysX resources. The existing terrain/mesh
// wrappers continue to provide actor registration, filtering, events and ray hits.
class LandscapeHeightfield final {
public:
    LandscapeHeightfield();
    ~LandscapeHeightfield();
    LandscapeHeightfield(const LandscapeHeightfield &) = delete;
    LandscapeHeightfield &operator=(const LandscapeHeightfield &) = delete;

    // Returns a PhysX resource ID (including 0), or UINT32_MAX on failure.
    // Hole cells retain the source grid and do not require triangle-mesh cooking.
    uint32_t create(const Uint16Array &samples, const Uint8Array &holes, uint32_t resolution, uint32_t wrapperObjectID);
    // Transfer a streamed mesh out of the JSB wrapper's permanent asset cache.
    bool adoptTriangleMesh(uint32_t objectID, uint32_t wrapperObjectID);
    bool adoptShape();
    // Called AFTER the initialized wrapper removes the actor/event mappings.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace cc::landscape

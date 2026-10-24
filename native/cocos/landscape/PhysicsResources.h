// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <memory>
#include "core/TypedArray.h"

namespace cc::landscape {

// Owns one streamed PhysX heightfield or mesh and its shape, matching TS.
// Handles are native resource/wrapper IDs; Web uses SDK/wrapper objects.
// Existing wrappers retain actor registration, filtering, events and ray hits.
class LandscapePhysicsResources final {
public:
    LandscapePhysicsResources();
    ~LandscapePhysicsResources();
    LandscapePhysicsResources(const LandscapePhysicsResources &) = delete;
    LandscapePhysicsResources &operator=(const LandscapePhysicsResources &) = delete;

    // Creates only a heightfield. Returns its handle (including 0), or UINT32_MAX.
    // Remembers the wrapper for adoption and partial-initialization cleanup.
    // Native supports hole materials; Web callers use meshes for hole patches.
    uint32_t createHeightfield(const Uint16Array &samples, const Uint8Array &holes, uint32_t resolution, uint32_t wrapperObjectID);
    // On success, transfers the mesh's original reference from its asset cache.
    bool adoptTriangleMesh(uint32_t objectID, uint32_t wrapperObjectID);
    // On success, transfers the remembered wrapper's original shape reference.
    // The wrapper must not release it after adoption; no extra reference is added.
    bool adoptShape();
    // Called AFTER the initialized wrapper removes the actor/event mappings.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace cc::landscape

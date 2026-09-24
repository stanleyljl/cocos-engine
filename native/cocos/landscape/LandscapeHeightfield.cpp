// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeHeightfield.h"
#include "base/Macros.h"

#if CC_USE_PHYSICS_PHYSX
    #include "physics/physx/PhysXWorld.h"
    #include "physics/physx/shapes/PhysXShape.h"
    #include "base/std/container/vector.h"
#endif

namespace cc::landscape {

struct LandscapeHeightfield::Impl {
#if CC_USE_PHYSICS_PHYSX
    physx::PxHeightField *heightfield{nullptr};
    physx::PxShape *shape{nullptr};
    uint32_t objectID{0};
    uint32_t uninitializedWrapperID{0};
#endif
};

LandscapeHeightfield::LandscapeHeightfield() : _impl(std::make_unique<Impl>()) {}
LandscapeHeightfield::~LandscapeHeightfield() { destroy(); }

uint32_t LandscapeHeightfield::create(const Uint16Array &source, uint32_t resolution, uint32_t wrapperObjectID) {
#if CC_USE_PHYSICS_PHYSX
    if (_impl->heightfield) return 0;
    _impl->uninitializedWrapperID = wrapperObjectID;
    if (resolution < 2 || resolution > 4097 ||
        source.length() != resolution * resolution) return 0;
    ccstd::vector<physx::PxHeightFieldSample> samples(source.length());
    for (uint32_t x = 0; x < resolution; ++x) {
        for (uint32_t z = 0; z < resolution; ++z) {
            auto &sample = samples[x * resolution + z];
            sample.height = static_cast<int16_t>(static_cast<int32_t>(source[z * resolution + x]) - 32768);
            sample.materialIndex0 = 0;
            sample.materialIndex1 = 0;
            sample.clearTessFlag();
        }
    }
    physx::PxHeightFieldDesc desc;
    desc.nbRows = resolution;
    desc.nbColumns = resolution;
    desc.samples.data = samples.data();
    desc.samples.stride = sizeof(physx::PxHeightFieldSample);
    _impl->heightfield = physics::PhysXWorld::getCooking().createHeightField(
        desc, physics::PhysXWorld::getPhysics().getPhysicsInsertionCallback());
    if (!_impl->heightfield) return 0;
    _impl->objectID = physics::PhysXWorld::getInstance().addPXObject(reinterpret_cast<uintptr_t>(_impl->heightfield));
    return _impl->objectID;
#else
    return 0;
#endif
}

bool LandscapeHeightfield::adoptShape() {
#if CC_USE_PHYSICS_PHYSX
    if (!_impl->heightfield || _impl->shape) return false;
    auto &world = physics::PhysXWorld::getInstance();
    auto *wrapper = reinterpret_cast<physics::PhysXShape *>(world.getWrapperPtrWithObjectID(_impl->uninitializedWrapperID));
    if (!wrapper) return false;
    auto &shape = wrapper->getShape();
    physx::PxHeightFieldGeometry geometry;
    if (!shape.getHeightFieldGeometry(geometry) || geometry.heightField != _impl->heightfield) return false;
    // Adopt the original createShape reference, not a new reference. Legacy
    // TerrainShape does not release it. This owner releases it on tile unload.
    _impl->shape = &shape;
    _impl->uninitializedWrapperID = 0;
    world.removePXObject(_impl->objectID);
    _impl->objectID = 0;
    return true;
#else
    return false;
#endif
}

void LandscapeHeightfield::destroy() {
#if CC_USE_PHYSICS_PHYSX
    if (_impl->uninitializedWrapperID) {
        // Cooking failed before the unchanged TerrainShape could initialize.
        physics::PhysXWorld::getInstance().removeWrapperObject(_impl->uninitializedWrapperID);
        _impl->uninitializedWrapperID = 0;
    }
    if (_impl->shape) {
        _impl->shape->release();
        _impl->shape = nullptr;
    }
    if (_impl->objectID) {
        physics::PhysXWorld::getInstance().removePXObject(_impl->objectID);
        _impl->objectID = 0;
    }
    if (_impl->heightfield) {
        _impl->heightfield->release();
        _impl->heightfield = nullptr;
    }
#endif
}

} // namespace cc::landscape

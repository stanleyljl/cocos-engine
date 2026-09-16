// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cc {
namespace landscape {

// Material coordinates are independent of the geometry quadtree. Level zero
// covers at least one meter; increasing the level doubles the covered area side.
struct VTPageAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};
};

inline uint32_t vtRootLevel(float sectorSize) {
    uint32_t level = 0;
    while (level < 27U && sectorSize / static_cast<float>(1U << (level + 1U)) >= 1.0F) ++level;
    return level;
}

inline float vtPageWorldSize(float sectorSize, uint32_t rootLevel, uint32_t level) {
    return sectorSize / static_cast<float>(1U << (rootLevel - level));
}

// A conservative CPU distance policy, independent of CDLOD distance ranges.
// The near page covers 1 m on a meter-aligned landscape. Cache pressure can
// select a resident ancestor without changing any geometry.
inline uint32_t vtDesiredLevel(float sectorSize, uint32_t rootLevel, float distance) {
    uint32_t level = 0;
    while (level < rootLevel && vtPageWorldSize(sectorSize, rootLevel, level) < distance * 0.5F) ++level;
    return level;
}

// Coordinates are relative to the landscape's minimum XZ, not its center.
// Expand to an ancestor until the ENTIRE (possibly morphed) primitive fits.
inline VTPageAddress vtCoveringPage(float sectorSize, uint32_t rootLevel, uint32_t desiredLevel,
                                   float minX, float minZ, float maxX, float maxZ) {
    VTPageAddress result;
    for (uint32_t level = desiredLevel; level <= rootLevel; ++level) {
        const double size = vtPageWorldSize(sectorSize, rootLevel, level);
        result = {level, static_cast<uint32_t>(std::floor(minX / size + 1e-7)),
                        static_cast<uint32_t>(std::floor(minZ / size + 1e-7))};
        if (maxX <= (result.x + 1.0) * size + size * 1e-7 &&
            maxZ <= (result.z + 1.0) * size + size * 1e-7) break;
    }
    return result;
}

} // namespace landscape
} // namespace cc

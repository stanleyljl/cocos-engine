// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include "landscape/LandscapeConfig.h"
#include "math/Vec4.h"

namespace cc {
namespace landscape {

// Material coordinates are independent of the geometry quadtree. Level zero
// covers at least one meter; increasing the level doubles the covered area side.
struct VTPageAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};

    uint64_t key() const { return makeNodeKey(level, x, z); }
    VTPageAddress parent() const { return {level + 1U, x >> 1U, z >> 1U}; }
};

// Resolved inputs for ONE material page, shared by source lookup, the cache,
// and the compose pass. These are shader records, not VT addresses or slots.
struct VTPageInputs {
    Vec4 region;      // output page: landscape-local XZ origin, world size, unused
    Vec4 splatSource; // input tile: landscape-local XZ origin, world size, array layer
    // 4x4 input tiles: 2x2 interior plus a one-tile ring for filtering gutters.
    std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> normalSources;
};

// Material-page dimensions are derived together; a geometry maxLevel cannot
// accidentally be supplied as the VT root. This class does not own atlas slots.
class VTPageLayout {
public:
    explicit VTPageLayout(float sectorSize = 1.0F) : _sectorSize(sectorSize) {
        assert(std::isfinite(sectorSize) && sectorSize > 0);
        while (_rootLevel < 27U && sectorSize / static_cast<float>(1U << (_rootLevel + 1U)) >= 1.0F) {
            ++_rootLevel;
        }
    }

    uint32_t rootLevel() const { return _rootLevel; }
    float pageSize(uint32_t level) const {
        return level <= _rootLevel ? _sectorSize / static_cast<float>(1U << (_rootLevel - level)) : 0.0F;
    }
    // Distance-based material resolution, independent of geometry LOD.
    uint32_t levelForDistance(float distance) const {
        uint32_t level = 0;
        while (level < _rootLevel && pageSize(level) < distance * 0.5F) {
            ++level;
        }
        return level;
    }
    LandscapeGridXZ pageOrigin(VTPageAddress page) const {
        const float size = pageSize(page.level);
        return {page.x * size, page.z * size};
    }

    // Bounds use minimum-corner-relative meters and must stay within one sector.
    // Include the complete morph trajectory, not just the original grid cell.
    VTPageAddress coveringPage(uint32_t desiredLevel, const LandscapeGridBounds &bounds) const {
        assert(desiredLevel <= _rootLevel);
        for (uint32_t level = desiredLevel; level <= _rootLevel; ++level) {
            const double size = pageSize(level);
            const VTPageAddress page{level,
                static_cast<uint32_t>(std::floor(bounds.min.x / size + 1e-7)),
                static_cast<uint32_t>(std::floor(bounds.min.z / size + 1e-7))};
            if (bounds.max.x <= (page.x + 1.0) * size + size * 1e-7 &&
                bounds.max.z <= (page.z + 1.0) * size + size * 1e-7) {
                return page;
            }
        }
        assert(false && "A VT patch must not cross sector boundaries");
        return {};
    }

private:
    float _sectorSize;
    uint32_t _rootLevel{0};
};

} // namespace landscape
} // namespace cc

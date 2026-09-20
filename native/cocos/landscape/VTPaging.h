// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "base/std/container/unordered_set.h"
#include "landscape/LandscapeConfig.h"

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

// A fixed, bounded reference grid keeps projection height/axis selection independent
// of camera distance and CDLOD morph. Reuse at most a quarter of the source pool.
inline uint32_t cliffReferenceLevel(const LandscapeData &data) {
    uint32_t level = data.minTileLevel;
    while (level < data.maxLevel &&
           static_cast<uint64_t>(data.sectorsX) * data.sectorsZ *
               (1ULL << (2U * (data.maxLevel - level))) > config::PAGE_POOL_LAYERS / 4U) ++level;
    return level;
}

inline float cliffDensityScale(float heightRange, float width, float childScale = 1.0F) {
    const float slope = std::max(heightRange, 0.0F) / std::max(width, 0.001F);
    // At most two extra VT levels. This is a conservative range-based estimate,
    // not a substitute for screen-space feedback; residency still bounds memory.
    // Retain narrow steep features when a coarse node encloses mostly flat
    // terrain. Averaging its high/low range over the coarse width loses them.
    return std::min(4.0F, std::max(childScale, std::sqrt(1.0F + slope * slope)));
}

inline float vtAncestorPriority(float requiredPagePriority, uint32_t ancestorSteps) {
    // Ancestors provide fallback, not additional visible coverage. Recomputing
    // size/distance for them incorrectly promotes bigger, blurrier pages ahead
    // of the fine page actually requested by the visible patch.
    return std::ldexp(requiredPagePriority, -static_cast<int>(std::min(ancestorSteps, 27U)));
}

struct VTPageRequest {
    VTPageAddress page;
    float priority{0.0F};
    uint64_t key{0};
    bool required{false}; // directly requested by a visible patch
};

inline void budgetVTRequests(ccstd::vector<VTPageRequest> &requests, uint32_t rootLevel, size_t capacity) {
    if (capacity == 0U) {
        requests.clear();
        return;
    }
    if (requests.size() > capacity) {
        // Detail-first truncation alone can drop EVERY ancestor of a visible
        // patch. Its only remaining fallback is then a whole-sector root page.
        // Reserve a bounded, view-wide coverage set before spending on detail.
        // All non-root ancestors already exist in the input request set.
        ccstd::unordered_set<uint64_t> coverage;
        const size_t coverageBudget = std::max(size_t{1}, capacity / 2U);
        for (uint32_t bias = 0; bias <= rootLevel; ++bias) {
            coverage.clear();
            for (const auto &request : requests) {
                if (!request.required) continue;
                const auto &page = request.page;
                const auto level = std::min(page.level + bias, rootLevel);
                if (level == rootLevel) continue; // roots have permanent slots
                const auto shift = level - page.level;
                coverage.insert(makeNodeKey(level, page.x >> shift, page.z >> shift));
            }
            if (coverage.size() <= coverageBudget) break;
        }
        float maximumPriority = 0.0F;
        for (const auto &request : requests) maximumPriority = std::max(maximumPriority, request.priority);
        for (auto &request : requests) {
            // Also compose coverage before refinement under the per-frame
            // update budget, so movement does not leave lasting root fallbacks.
            if (coverage.count(request.key) != 0U) request.priority += maximumPriority + 1.0F;
        }
    }
    std::sort(requests.begin(), requests.end(), [](const auto &a, const auto &b) {
        return a.priority != b.priority ? a.priority > b.priority : a.key < b.key;
    });
    if (requests.size() > capacity) requests.resize(capacity);
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

// Skip CPU reconstruction only when both selection and streamed content are
// unchanged. Page publication must wake it even when the camera is stationary.
class LandscapeSyncCache {
public:
    using Positions = std::array<float, 9>; // geometry camera, VT camera, terrain origin

    bool matches(const Positions &positions, uint64_t tileRevision, uint64_t vtRevision,
                 const ccstd::vector<QuadNode> &selected) const {
        return _valid && positions == _positions && tileRevision == _tileRevision &&
            vtRevision == _vtRevision && selected.size() == _selected.size() &&
            std::equal(selected.begin(), selected.end(), _selected.begin(), [](const QuadNode &a, const QuadNode &b) {
                return a.level == b.level && a.ix == b.ix && a.iz == b.iz &&
                    a.minY == b.minY && a.maxY == b.maxY && a.quadrantMask == b.quadrantMask;
            });
    }

    void store(const Positions &positions, uint64_t tileRevision, uint64_t vtRevision,
               const ccstd::vector<QuadNode> &selected) {
        _positions = positions;
        _tileRevision = tileRevision;
        _vtRevision = vtRevision;
        _selected = selected;
        _valid = true;
    }

    void invalidate() { _valid = false; }

private:
    bool _valid{false};
    Positions _positions{};
    uint64_t _tileRevision{0};
    uint64_t _vtRevision{0};
    ccstd::vector<QuadNode> _selected;
};

} // namespace landscape
} // namespace cc

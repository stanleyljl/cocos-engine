// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once
#include <array>
#include <cstdint>
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeData.h"
#include "math/Vec4.h"
#include "math/Vec3.h"

namespace cc::landscape {

// Material coordinates are independent of the geometry quadtree. Level zero
// covers at least one logical unit; increasing the level doubles the covered area side.
struct VTPageAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};

    uint64_t key() const { return makeNodeKey(level, x, z); }
};

// A node owns unique page addresses and a compact index for each grid cell.
// Residency and physical-slot lookup visit pages once, not all 256 cells.
struct VTGridMapping {
    std::array<uint8_t, 256> cells{};
    ccstd::vector<VTPageAddress> pages;
};

// Resolved shader records for one material-page composition.
struct VTPageInputs {
    Vec4 region;      // output page: landscape-local XZ origin, size, unused
    Vec4 splatSource; // input tile: landscape-local XZ origin, size, array layer
    // 4x4 input tiles: 2x2 interior plus a one-tile ring for filtering gutters.
    std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> normalSources;
};

// Material-page dimensions are derived together; a geometry maxLevel cannot
// accidentally be supplied as the VT root. This class does not own atlas slots.
class VTPageLayout {
public:
    explicit VTPageLayout(float sectorSize = 1.0F);

    uint32_t rootLevel() const { return _rootLevel; }
    float pageSize(uint32_t level) const {
        return level <= _rootLevel ? _sectorSize / static_cast<float>(1U << (_rootLevel - level)) : 0.0F;
    }
    // Distance-based material resolution, independent of geometry LOD.
    uint32_t levelForDistance(float distance) const;
    LandscapeRegion pageRegion(VTPageAddress page) const {
        const float size = pageSize(page.level);
        return {page.x * size, page.z * size, size};
    }

    // Select material resolution independently of geometry, retaining the same
    // distance bands and stitched coverage for every cell of the node.
    VTGridMapping gridMapping(LandscapeRegion region, float minY, float maxY,
                         const Vec3 &camera, uint32_t bias) const;

    // Local bounds must stay within one sector.
    // Include the complete stitched footprint, not just the original grid cell.
    VTPageAddress coveringPage(uint32_t desiredLevel, LandscapePoint min, LandscapePoint max) const;

private:
    float _sectorSize;
    uint32_t _rootLevel{0};
};

// Exact-address physical storage. The renderer owns admission and local grid mappings.
// The working set includes BOTH displayed and candidate leaves until commit.
class VirtualTexture {
public:
    enum class State {
        EMPTY,
        DIRTY, // No composed pixels yet.
        READY,
        STALE, // Needs recomposition; previous pixels remain drawable.
    };
    struct Page {
        VTPageAddress address;
        State state{State::EMPTY};
        uint64_t lastReserved{0};
        bool needsComposition() const {
            return state == State::DIRTY || state == State::STALE;
        }
        bool drawable() const {
            return state == State::READY || state == State::STALE;
        }
    };
    bool init(const LandscapeData &data, uint32_t capacity = config::VT_PAGE_COUNT);
    // Reservation inputs are unique addresses, collected by LandscapeRenderer.
    bool canReserve(const ccstd::vector<VTPageAddress> &pages) const;
    bool reserve(const ccstd::vector<VTPageAddress> &pages);
    const ccstd::vector<uint32_t> &activeSlots() const { return _active; }
    const Page &page(uint32_t slot) const { return _pages[slot]; }
    LandscapeRegion region(VTPageAddress address) const { return _layout.pageRegion(address); }
    void publish(const ccstd::vector<uint32_t> &slots);
    void invalidate();
    bool ready() const;
    size_t capacity() const { return _pages.size(); }
    bool needsComposition(VTPageAddress address) const;
    uint64_t revision() const { return _revision; }
    // Exact address only. An absent/unpublished page is an error at draw time.
    int resolve(VTPageAddress address) const;

private:
    int allocate(VTPageAddress address);
    LandscapeData _data;
    VTPageLayout _layout;
    uint64_t _reservation{0}; // Advances per successful reservation, not per rendered frame.
    uint64_t _revision{0};
    ccstd::vector<Page> _pages;
    ccstd::vector<uint32_t> _active;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;
};
} // namespace cc::landscape

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once
#include <array>
#include <cstdint>
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeData.h"
#include "math/Vec4.h"

namespace cc::landscape {

// Material coordinates are independent of the geometry quadtree. Level zero
// covers at least one logical unit; increasing the level doubles the covered area side.
struct VTPageAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};

    uint64_t key() const { return makeNodeKey(level, x, z); }
};

// Resolved inputs for one material page, shared by source lookup, the cache,
// and the compose pass. These are shader records, not VT addresses or slots.
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

    // Reserve every possible stitched position, independently of camera distance
    // and the current edge mask. A neighbor may change LOD while this page stays
    // bound; an odd edge vertex then snaps to the preceding even grid vertex.
    VTPageAddress coveringPatch(uint32_t desiredLevel, LandscapePoint origin, float cellSize,
                                uint32_t x, uint32_t z, uint32_t cells) const;

    // Local bounds must stay within one sector.
    // Include the complete stitched footprint, not just the original grid cell.
    VTPageAddress coveringPage(uint32_t desiredLevel, LandscapePoint min, LandscapePoint max) const;

private:
    float _sectorSize;
    uint32_t _rootLevel{0};
};

// Exact-address physical storage. The renderer owns admission and explicit patch bindings.
// The working set includes BOTH displayed and candidate leaves until commit.
class VirtualTexture {
public:
    enum class State { EMPTY,
                       DIRTY,
                       READY };
    struct Page {
        VTPageAddress address;
        VTPageInputs inputs;
        State state{State::EMPTY};
        uint64_t lastRequested{0};
        bool inputsResolved{false};
        bool published{false};
    };
    bool init(const LandscapeData &data, uint32_t capacity = config::VT_PAGE_COUNT);
    bool canReserve(const ccstd::vector<VTPageAddress> &pages) const;
    bool reserve(const ccstd::vector<VTPageAddress> &pages);
    const ccstd::vector<uint32_t> &activeSlots() const { return _active; }
    const Page &page(uint32_t slot) const { return _pages[slot]; }
    void setInputs(uint32_t slot, const VTPageInputs &inputs);
    void collectUpdates(ccstd::vector<uint32_t> &slots, uint32_t budget) const;
    void publish(const ccstd::vector<uint32_t> &slots);
    void invalidate();
    bool ready() const;
    size_t capacity() const { return _pages.size(); }
    void clearInputs(uint32_t slot) { _pages[slot].inputsResolved = false; }
    bool needsSources(VTPageAddress address) const;
    uint64_t revision() const { return _revision; }
    // Exact address only. An absent/unpublished page is an error at draw time.
    int resolve(VTPageAddress address) const;

private:
    int allocate(VTPageAddress address);
    LandscapeData _data;
    VTPageLayout _layout;
    uint64_t _frame{0}, _revision{0};
    ccstd::vector<Page> _pages;
    ccstd::vector<uint32_t> _active;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;
};
} // namespace cc::landscape

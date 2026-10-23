/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/VTPageProcessing.h"
#include "math/Vec3.h"

namespace cc {
class TextureBase;
}

namespace cc::landscape {
class VTRenderer;

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
    static constexpr uint32_t SIDE = 16;
    static constexpr uint32_t CELL_COUNT = SIDE * SIDE;
    std::array<uint8_t, CELL_COUNT> cells{};
    ccstd::vector<VTPageAddress> pages;
};

// Source addresses for one material-page composition, independent of residency.
struct VTPageSources {
    NodeAddress splat;
    // 2x2 lookup: base precision repeats one tile; far detail uses up to four.
    // Each source includes its own real-neighbor border for the VT gutters.
    std::array<NodeAddress, config::VT_NORMAL_SOURCE_COUNT> normals;
    // Append unique addresses in sampling order, preserving existing entries.
    void appendTo(ccstd::vector<NodeAddress> &output) const;
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

    // Subdivide aligned grid regions according to distance, independently of
    // geometry LOD, and map each cell to a page covering its stitched footprint.
    VTGridMapping buildGridMapping(LandscapeRegion region, float minY, float maxY, const Vec3 &camera) const;

    // Local bounds must stay within one sector.
    // Include the complete stitched footprint, not just the original grid cell.
    VTPageAddress coveringPage(uint32_t desiredLevel, LandscapePoint min, LandscapePoint max) const;

private:
    void mapCells(const LandscapeRegion &firstCell, const Vec3 &camera, float heightDistance, uint32_t left, uint32_t bottom, uint32_t side, VTGridMapping &mapping) const;
    float _sectorSize;
    uint32_t _rootLevel{0};
};

// GPU page table consumed by terrain draws. Each grid owns 256 consecutive
// RG8 entries (physical slot, VT level); allocation and uploads live together.
class VTPageTable {
public:
    VTPageTable();
    ~VTPageTable();
    gfx::Texture *texture() const { return _texture.get(); }
    bool ensureCapacity(uint32_t count);
    uint32_t allocate();
    void release(uint32_t slot);
    bool update(uint32_t slot, const VTGridMapping &mapping, const VTRenderer &pages);
    void upload(ccstd::vector<uint32_t> slots);
    void destroy();

protected:
    // CPU allocation step; runtime callers use ensureCapacity() to update both mirrors.
    void resizeEntries(size_t count);

private:
    struct Entry {
        uint8_t physicalSlot{0};
        uint8_t level{0};
    };
    static_assert(sizeof(Entry) == 2, "VT page table entries must match RG8 texels");
    IntrusivePtr<gfx::Texture> _texture;
    ccstd::vector<Entry> _entries;
    ccstd::vector<uint32_t> _freeSlots;
    uint32_t _size{0};
    uint32_t _count{0};
};

// Owns page residency and the complete VT update sequence. LandscapeSchedule
// admits resource demands; only this coordinator submits work and publishes pages.
class VTRenderer {
public:
    VTRenderer();
    ~VTRenderer();
    VTRenderer(const VTRenderer &) = delete;
    VTRenderer &operator=(const VTRenderer &) = delete;
    bool init(const LandscapeAsset &asset, SourceDataManager &sourceData, const MaterialLibrary &materials, DecalRenderer &decals);
    void destroy();
    uint32_t renderPages(SourceDataManager &sourceData, uint32_t maxPages);

    enum class PageState {
        EMPTY,
        UNCOMPOSED, // No composed pixels yet.
        READY,
        OUTDATED, // Needs recomposition; previous pixels remain drawable.
    };
    struct Page {
        VTPageAddress address;
        PageState state{PageState::EMPTY};
        uint64_t lastReserved{0};
        bool needsComposition() const {
            return state == PageState::UNCOMPOSED || state == PageState::OUTDATED;
        }
        bool drawable() const {
            return state == PageState::READY || state == PageState::OUTDATED;
        }
    };
    // Reservation inputs are unique addresses, collected by LandscapeSchedule.
    bool canReserve(const ccstd::vector<VTPageAddress> &pages) const;
    bool reserve(const ccstd::vector<VTPageAddress> &pages);
    const ccstd::vector<uint32_t> &reservedSlots() const { return _reservedSlots; }
    const Page &page(uint32_t slot) const { return _pages[slot]; }
    LandscapeRegion region(VTPageAddress address) const { return _layout.pageRegion(address); }
    VTPageSources selectSources(VTPageAddress address, bool refineFarDetail) const;
    void invalidate();
    // Invalidate intersecting cached pages, including their filtering gutters.
    void invalidate(LandscapeRegion region);
    bool ready() const;
    size_t capacity() const { return _pages.size(); }
    bool needsComposition(VTPageAddress address) const;
    // Exact address only. An absent/unpublished page is an error at draw time.
    int resolve(VTPageAddress address) const;

    VTPageTable &pageTable() { return _pageTable; }
    const VTPageTable &pageTable() const { return _pageTable; }
    bool getRefineFarDetail() const { return _refineFarDetail; }
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setHeightBlendEnabled(bool enabled);
    void setRefineFarDetail(bool enabled);
    TextureBase *atlas() const;
    gfx::Texture *albedo() const;
    gfx::Texture *normalRoughnessAO() const;
    gfx::Sampler *sampler() const { return _sampler; }

protected:
    // CPU residency operations; runtime callers use init() and renderPages().
    bool initPages(const LandscapeData &data, uint32_t capacity = config::VT_PAGE_COUNT);
    // Mark composition complete only after its GPU commands have been submitted.
    void publish(uint32_t slot);

private:
    bool initResources(const LandscapeAsset &asset, SourceDataManager &sourceData, const MaterialLibrary &materials, DecalRenderer &decals, bool compressed);
    uint32_t sourceLevel(float size) const;
    int allocate(VTPageAddress address);
    LandscapeData _data;
    VTPageLayout _layout;
    uint64_t _reservation{0}; // Advances per successful reservation, not per rendered frame.
    ccstd::vector<Page> _pages;
    ccstd::vector<uint32_t> _reservedSlots;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;

    VTPageTable _pageTable;
    std::unique_ptr<VTPageComposer> _composer;
    std::unique_ptr<VTPageMipGenerator> _mipGenerator;
    std::unique_ptr<VTPageCompressor> _compressor;
    ccstd::vector<VTPageInstance> _batch;
    IntrusivePtr<gfx::CommandBuffer> _commands;
    gfx::Sampler *_sampler{nullptr}; // device-owned
    bool _needsClear{true};
    bool _refineFarDetail{true};
};
} // namespace cc::landscape

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
#include "landscape/Data.h"
#include "landscape/VTPageStages.h"
#include "math/Vec3.h"

namespace cc {
class TextureBase;
}

namespace cc::landscape {
class VTRenderer;

// Scoped to one admission scan; caches immutable inputs, never capacity decisions.
class VTCapacityQuery {
public:
    VTCapacityQuery(const VTRenderer &renderer, const LandscapeResidency &residency);
    bool canPrepare(const ccstd::vector<VTPageAddress> &pages, const ccstd::vector<NodeAddress> &sources);

private:
    const VTRenderer &_renderer;
    const LandscapeResidency &_residency;
    ccstd::unordered_map<uint64_t, ccstd::vector<NodeAddress>> _pageSources;
};

// GPU page table consumed by terrain draws. Each grid owns 256 consecutive
// RG8 entries (physical slot, VT level); allocation and uploads live together.
class VTPageTable {
public:
    VTPageTable();
    ~VTPageTable();
    void destroy();
    bool canFit(uint32_t count) const;
    bool ensureCapacity(uint32_t count);
    uint32_t allocate();
    void release(uint32_t slot);
    bool update(uint32_t slot, const VTGridMapping &mapping, const VTRenderer &pages);
    void upload(ccstd::vector<uint32_t> slots);
    gfx::Texture *texture() const { return _texture.get(); }

protected:
    // CPU allocation step; runtime callers use ensureCapacity() to update both mirrors.
    void resizeEntries(size_t count);

private:
    struct Entry {
        uint8_t physicalSlot{0};
        uint8_t level{0};
    };
    static_assert(sizeof(Entry) == 2, "VT page table entries must match RG8 texels");

    uint32_t requiredSize(uint32_t count) const;

    IntrusivePtr<gfx::Texture> _texture;
    ccstd::vector<Entry> _entries;
    ccstd::vector<uint32_t> _freeSlots;
    uint32_t _size{0};
    uint32_t _count{0};
};

class VTRenderer {
public:
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
        bool needsComposition() const { return state == PageState::UNCOMPOSED || state == PageState::OUTDATED; }
        bool drawable() const { return state == PageState::READY || state == PageState::OUTDATED; }
    };

    VTRenderer();
    ~VTRenderer();
    VTRenderer(const VTRenderer &) = delete;
    VTRenderer &operator=(const VTRenderer &) = delete;
    bool init(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals, uint32_t atlasSize);
    void destroy();

    // Page residency and composition.
    VTCapacityQuery capacityQuery() const { return VTCapacityQuery(*this, *_residency); }
    bool canFit(const ccstd::vector<VTPageAddress> &pages) const;
    void request(VTPageAddress address);
    void release(VTPageAddress address);
    bool update(uint32_t maxPages);
    void invalidate();
    // Invalidate intersecting cached pages, including their filtering gutters.
    void invalidate(LandscapeRegion region);
    bool ready() const;
    bool needsComposition(VTPageAddress address) const;
    // Exact address only. An absent/unpublished page is an error at draw time.
    int resolve(VTPageAddress address) const;
    const ccstd::vector<uint32_t> &reservedSlots() const { return _reservedSlots; }
    const Page &page(uint32_t slot) const { return _pages[slot]; }
    LandscapeRegion region(VTPageAddress address) const { return _layout.pageRegion(address); }
    VTPageSources selectSources(VTPageAddress address, bool refineFarDetail, bool triplanar) const;

    // Composition settings.
    void setGlobalColorMap(Texture2D *texture);
    void setGlobalColorStrength(float strength);
    void setHeightBlendEnabled(bool enabled);
    void setTriplanarEnabled(bool enabled);
    bool getTriplanarEnabled() const { return _triplanarEnabled; }
    void setRefineFarDetail(bool enabled);
    bool getRefineFarDetail() const { return _refineFarDetail; }

    // GPU resources.
    VTPageTable &pageTable() { return _pageTable; }
    const VTPageTable &pageTable() const { return _pageTable; }
    TextureBase *atlas() const;
    gfx::Texture *albedo() const;
    gfx::Texture *normalRoughness() const;
    gfx::Sampler *sampler() const { return _sampler; }

protected:
    // CPU residency operations; runtime callers use init() and update().
    bool initPages(const LandscapeData &data, const ccstd::vector<ccstd::vector<HeightRange>> &heightRanges, uint32_t capacity = config::VT_PAGE_COUNT);
    void publish(uint32_t slot);
    bool applyRequests();

private:
    struct Request {
        VTPageAddress address;
        uint32_t count{0};
    };
    uint32_t renderPages(LandscapeResidency &residency, const ccstd::vector<VTPageAddress> &pages, uint32_t maxPages);
    bool initResources(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals, uint32_t atlasSize, bool compressed);
    int allocate(VTPageAddress address);
    uint32_t sourceLevel(float size) const;
    bool hasCliff(LandscapeRegion region) const;

    LandscapeData _data;
    VTPageLayout _layout;
    const ccstd::vector<ccstd::vector<HeightRange>> *_heightRanges{nullptr}; // Borrowed immutable asset metadata.

    LandscapeResidency *_residency{nullptr}; // Borrowed; initialized before runtime work.
    ccstd::unordered_map<uint64_t, Request> _requests;
    bool _requestsDirty{false};
    // Owns one reference per source until update() reconciles composition demand.
    ccstd::vector<NodeAddress> _sourceRequests;
    uint64_t _reservation{0}; // Advances per successful reservation, not per rendered frame.
    ccstd::vector<Page> _pages;
    ccstd::vector<uint32_t> _reservedSlots;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;

    VTPageTable _pageTable;
    std::unique_ptr<VTPageComposer> _composer;
    std::unique_ptr<VTPageMipGenerator> _mipGenerator;
    std::unique_ptr<VTPageCompressor> _compressor;
    ccstd::vector<VTPageInstance> _batch;
    gfx::Sampler *_sampler{nullptr}; // device-owned
    bool _needsClear{true};

    bool _refineFarDetail{true};
    bool _triplanarEnabled{true};
};
} // namespace cc::landscape

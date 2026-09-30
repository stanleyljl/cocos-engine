// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once
#include "base/std/container/unordered_map.h"
#include "landscape/VTPaging.h"

namespace cc::landscape {
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

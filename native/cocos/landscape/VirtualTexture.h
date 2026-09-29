// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include "base/std/container/unordered_map.h"
#include "landscape/VTPaging.h"

namespace cc::landscape {

// CPU-only VT working set and physical-slot ownership. No GPU or source loading.
// Frame: beginRequests -> request* -> endRequests -> setInputs* -> collectUpdates
// -> GPU submission -> publish. Source resolution NEVER allocates/evicts slots.
class VirtualTexture {
public:
    enum class State { EMPTY, DIRTY, READY };
    struct Page {
        VTPageAddress address;
        VTPageInputs inputs;
        State state{State::EMPTY};
        uint64_t lastRequested{0};
        bool inputsResolved{false};
    };

    bool init(const LandscapeData &data, uint32_t capacity = config::VT_PAGE_COUNT);
    void beginRequests();
    void request(VTPageAddress address, float priority);
    void endRequests();

    // Roots first, followed by coverage/refinement order. Fixed until endRequests.
    const ccstd::vector<uint32_t> &activeSlots() const { return _active; }
    const Page &page(uint32_t slot) const { return _pages[slot]; }
    void setInputs(uint32_t slot, const VTPageInputs &inputs);
    void collectUpdates(ccstd::vector<uint32_t> &slots, uint32_t budget) const;
    // Called only after all mip writes have been submitted on the graphics queue.
    void publish(const ccstd::vector<uint32_t> &slots);
    void invalidate();
    bool ready() const;
    uint64_t revision() const { return _revision; }
    // Finest active READY ancestor, including the sector root; -1 before warmup.
    int resolve(VTPageAddress address) const;

private:
    struct Request {
        VTPageAddress address;
        float priority{0};
        bool direct{false};
        bool coverage{false};
    };
    void selectRequests();
    int allocate(VTPageAddress address);

    LandscapeData _data;
    VTPageLayout _layout;
    uint32_t _rootCount{0};
    uint64_t _frame{0};
    uint64_t _revision{0};
    ccstd::vector<Page> _pages;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;
    ccstd::vector<uint32_t> _active;
    ccstd::vector<Request> _requests;
    ccstd::unordered_map<uint64_t, size_t> _requestIndices;
};

} // namespace cc::landscape

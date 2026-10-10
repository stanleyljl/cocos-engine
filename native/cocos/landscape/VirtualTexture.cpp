// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/VirtualTexture.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include "base/std/container/unordered_set.h"
namespace cc::landscape {

VTPageLayout::VTPageLayout(float sectorSize) : _sectorSize(sectorSize) {
    assert(sectorSize > 0);
    while (_rootLevel < 27U && sectorSize / static_cast<float>(1U << (_rootLevel + 1U)) >= 1.0F) {
        ++_rootLevel;
    }
}

uint32_t VTPageLayout::levelForDistance(float distance) const {
    uint32_t level = 0;
    while (level < _rootLevel && pageSize(level) < distance * 0.5F) {
        ++level;
    }
    return level;
}

VTPageAddress VTPageLayout::coveringPatch(uint32_t desiredLevel, LandscapePoint origin, float cellSize,
                                        uint32_t x, uint32_t z, uint32_t cells) const {
    return coveringPage(desiredLevel,
                        {origin.x + (x - (x & 1U)) * cellSize, origin.z + (z - (z & 1U)) * cellSize},
                         {origin.x + (x + cells) * cellSize, origin.z + (z + cells) * cellSize});
}

VTPageAddress VTPageLayout::coveringPage(uint32_t desiredLevel, LandscapePoint min, LandscapePoint max) const {
    assert(desiredLevel <= _rootLevel);
    for (uint32_t level = desiredLevel; level <= _rootLevel; ++level) {
        const float size = pageSize(level);
        const VTPageAddress page{level,
                                 static_cast<uint32_t>(std::floor(min.x / size + 1e-7F)),
                                 static_cast<uint32_t>(std::floor(min.z / size + 1e-7F))};
        if (max.x <= (page.x + 1.0F) * size + size * 1e-7F &&
            max.z <= (page.z + 1.0F) * size + size * 1e-7F) {
            return page;
        }
    }
    assert(false && "A VT patch must not cross sector boundaries");
    return {};
}

bool VirtualTexture::init(const LandscapeData &data, uint32_t capacity) {
    if (!data.valid() || capacity < static_cast<uint64_t>(data.sectorsX) * data.sectorsZ) {
        return false;
    }
    *this = VirtualTexture{};
    _data = data;
    _layout = VTPageLayout(data.sectorSize);
    _pages.resize(capacity);
    return true;
}
bool VirtualTexture::canReserve(const ccstd::vector<VTPageAddress> &pages) const {
    ccstd::unordered_set<uint64_t> unique;
    for (const auto p : pages) {
        if (p.level > _layout.rootLevel()) {
            return false;
        }
        const uint64_t side = 1ULL << (_layout.rootLevel() - p.level);
        if (p.x >= _data.sectorsX * side || p.z >= _data.sectorsZ * side) {
            return false;
        }
        unique.insert(p.key());
    }
    return unique.size() <= _pages.size();
}
bool VirtualTexture::reserve(const ccstd::vector<VTPageAddress> &pages) {
    if (!canReserve(pages)) {
        return false;
    }
    ++_frame;
    _active.clear();
    for (const auto p : pages) {
        const auto it = _lookup.find(p.key());
        if (it != _lookup.end()) {
            _pages[it->second].lastRequested = _frame;
        }
    }
    ccstd::unordered_set<uint64_t> unique;
    for (const auto p : pages) {
        if (!unique.insert(p.key()).second) {
            continue;
        }
        const auto it = _lookup.find(p.key());
        const int slot = it == _lookup.end() ? allocate(p) : static_cast<int>(it->second);
        assert(slot >= 0);
        _pages[slot].lastRequested = _frame;
        _active.push_back(static_cast<uint32_t>(slot));
    }
    return true;
}
int VirtualTexture::allocate(VTPageAddress address) {
    int slot = -1;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (uint32_t i = 0; i < _pages.size(); ++i) {
        const auto &p = _pages[i];
        if (p.state == State::EMPTY) {
            slot = static_cast<int>(i);
            break;
        }
        if (p.lastRequested != _frame && p.lastRequested < oldest) {
            oldest = p.lastRequested;
            slot = static_cast<int>(i);
        }
    }
    if (slot < 0) {
        return -1;
    }
    auto &p = _pages[slot];
    if (p.state != State::EMPTY) {
        _lookup.erase(p.address.key());
    }
    p = Page{};
    p.address = address;
    p.state = State::DIRTY;
    _lookup[address.key()] = static_cast<uint32_t>(slot);
    return slot;
}
void VirtualTexture::setInputs(uint32_t slot, const VTPageInputs &inputs) {
    auto &p = _pages[slot];
    assert(p.lastRequested == _frame && p.state == State::DIRTY);
    p.inputs = inputs;
    p.inputsResolved = true;
}
void VirtualTexture::collectUpdates(ccstd::vector<uint32_t> &slots, uint32_t budget) const {
    slots.clear();
    for (const auto slot : _active) {
        const auto &p = _pages[slot];
        if (p.state != State::DIRTY || !p.inputsResolved) {
            continue;
        }
        if (slots.size() == budget) {
            break;
        }
        slots.push_back(slot);
    }
}
void VirtualTexture::publish(const ccstd::vector<uint32_t> &slots) {
    for (const auto slot : slots) {
        auto &p = _pages[slot];
        assert(p.inputsResolved && p.lastRequested == _frame);
        p.state = State::READY;
        p.published = true;
    }
    if (!slots.empty()) {
        ++_revision;
    }
}
void VirtualTexture::invalidate() {
    for (auto &p : _pages) {
        if (p.state != State::EMPTY) {
            p.state = State::DIRTY;
            p.inputsResolved = false;
        }
    }
    ++_revision;
}
bool VirtualTexture::ready() const {
    return !_active.empty() && std::all_of(_active.begin(), _active.end(), [this](uint32_t slot) { return _pages[slot].state == State::READY; });
}
bool VirtualTexture::needsSources(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    return it == _lookup.end() || _pages[it->second].state != State::READY;
}
int VirtualTexture::resolve(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    if (it == _lookup.end()) {
        return -1;
    }
    const auto &p = _pages[it->second];
    return p.lastRequested == _frame && p.published ? static_cast<int>(it->second) : -1;
}
} // namespace cc::landscape

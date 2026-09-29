// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/VirtualTexture.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include "base/std/container/unordered_set.h"

namespace cc::landscape {
namespace {
bool same(const Vec4 &a, const Vec4 &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
}

bool VirtualTexture::init(const LandscapeData &data, uint32_t capacity) {
    *this = VirtualTexture{};
    const uint64_t roots = static_cast<uint64_t>(data.sectorsX) * data.sectorsZ;
    if (!data.valid() || capacity == 0 || roots > capacity) {
        return false;
    }
    _data = data;
    _layout = VTPageLayout(data.sectorSize);
    _rootCount = static_cast<uint32_t>(roots);
    _pages.resize(capacity);
    _active.reserve(capacity);
    // Root slots are fixed for the cache lifetime, not eviction candidates.
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            const uint32_t slot = z * data.sectorsX + x;
            auto &p = _pages[slot];
            p.address = {_layout.rootLevel(), x, z};
            p.state = State::DIRTY;
            _lookup.emplace(p.address.key(), slot);
            _active.push_back(slot);
        }
    }
    return true;
}

void VirtualTexture::beginRequests() {
    _requests.clear();
    _requestIndices.clear();
}

void VirtualTexture::request(VTPageAddress address, float priority) {
    const auto root = _layout.rootLevel();
    if (address.level > root || !std::isfinite(priority) || priority < 0) {
        return;
    }
    const uint32_t side = 1U << (root - address.level);
    if (address.x >= static_cast<uint64_t>(_data.sectorsX) * side ||
        address.z >= static_cast<uint64_t>(_data.sectorsZ) * side) {
        return;
    }
    const uint32_t desired = address.level;
    for (; address.level < root; address = address.parent(), priority *= 0.5F) {
        const auto inserted = _requestIndices.emplace(address.key(), _requests.size());
        if (inserted.second) {
            _requests.push_back({address, priority});
        }
        auto &r = _requests[inserted.first->second];
        r.priority = std::max(r.priority, priority);
        r.direct |= address.level == desired;
    }
}

void VirtualTexture::selectRequests() {
    const size_t capacity = _pages.size() - _rootCount;
    if (capacity == 0) {
        _requests.clear();
        return;
    }
    if (_requests.size() > capacity) {
        // Reserve half the dynamic cache for view-wide coverage. A common
        // ancestor bias keeps every visible region represented before refinement.
        const size_t reserve = std::max(size_t{1}, capacity / 2);
        ccstd::unordered_set<uint64_t> coverage;
        for (uint32_t bias = 0; bias <= _layout.rootLevel(); ++bias) {
            coverage.clear();
            for (const auto &r : _requests) {
                if (!r.direct) {
                    continue;
                }
                const auto level = std::min(r.address.level + bias, _layout.rootLevel());
                if (level == _layout.rootLevel()) {
                    continue; // permanent coverage
                }
                const auto shift = level - r.address.level;
                coverage.insert(VTPageAddress{level, r.address.x >> shift, r.address.z >> shift}.key());
            }
            if (coverage.size() <= reserve) {
                break;
            }
        }
        for (auto &r : _requests) {
            r.coverage = coverage.count(r.address.key()) != 0;
        }
    }
    // The same order drives BOTH slot admission and composition. No priority
    // boosting, second dirty-page sort, or separate protected-key plan.
    std::sort(_requests.begin(), _requests.end(), [](const Request &a, const Request &b) {
        if (a.coverage != b.coverage) {
            return a.coverage;
        }
        return a.priority != b.priority ? a.priority > b.priority : a.address.key() < b.address.key();
    });
    if (_requests.size() > capacity) {
        _requests.resize(capacity);
    }
}

void VirtualTexture::endRequests() {
    if (_pages.empty()) {
        return;
    }
    selectRequests();
    _requestIndices.clear();
    ++_frame;
    _active.clear();
    for (auto &p : _pages) {
        p.inputsResolved = false;
    }
    for (uint32_t slot = 0; slot < _rootCount; ++slot) {
        _pages[slot].lastRequested = _frame;
        _active.push_back(slot);
    }
    // Pin the ENTIRE admitted set before allocating any missing page.
    for (const auto &r : _requests) {
        const auto it = _lookup.find(r.address.key());
        if (it != _lookup.end()) {
            _pages[it->second].lastRequested = _frame;
        }
    }
    for (const auto &r : _requests) {
        const auto it = _lookup.find(r.address.key());
        const int slot = it != _lookup.end() ? static_cast<int>(it->second) : allocate(r.address);
        assert(slot >= 0 && "Admitted VT working set must fit the physical cache");
        if (slot < 0) {
            continue;
        }
        _pages[slot].lastRequested = _frame;
        _active.push_back(static_cast<uint32_t>(slot));
    }
    _requests.clear(); // Only the admitted physical slots survive this phase.
}

int VirtualTexture::allocate(VTPageAddress address) {
    int slot = -1;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (uint32_t i = _rootCount; i < _pages.size(); ++i) {
        const auto &p = _pages[i];
        if (p.state == State::EMPTY) {
            slot = static_cast<int>(i);
            break;
        }
        if (p.lastRequested != _frame && p.lastRequested < oldest) {
            slot = static_cast<int>(i);
            oldest = p.lastRequested;
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
    _lookup.emplace(address.key(), static_cast<uint32_t>(slot));
    return slot;
}

void VirtualTexture::setInputs(uint32_t slot, const VTPageInputs &inputs) {
    auto &p = _pages[slot];
    assert(p.state != State::EMPTY && p.lastRequested == _frame);
    if (!same(p.inputs.region, inputs.region) || !same(p.inputs.splatSource, inputs.splatSource) ||
        !std::equal(p.inputs.normalSources.begin(), p.inputs.normalSources.end(), inputs.normalSources.begin(), same)) {
        p.state = State::DIRTY;
    }
    p.inputs = inputs;
    p.inputsResolved = true;
}

void VirtualTexture::collectUpdates(ccstd::vector<uint32_t> &slots, uint32_t budget) const {
    slots.clear();
    for (const uint32_t slot : _active) {
        const auto &p = _pages[slot];
        if (p.state != State::DIRTY || !p.inputsResolved) {
            continue;
        }
        if (slot >= _rootCount) {
            if (budget == 0) {
                break;
            }
            --budget;
        }
        slots.push_back(slot);
    }
}

void VirtualTexture::publish(const ccstd::vector<uint32_t> &slots) {
    for (const uint32_t slot : slots) {
        auto &p = _pages[slot];
        assert(p.inputsResolved && p.lastRequested == _frame && p.state == State::DIRTY);
        p.state = State::READY;
    }
    if (!slots.empty()) {
        ++_revision;
    }
}

void VirtualTexture::invalidate() {
    for (auto &p : _pages) {
        if (p.state != State::EMPTY) {
            p.state = State::DIRTY;
        }
    }
    ++_revision;
}

bool VirtualTexture::ready() const {
    return !_active.empty() && std::all_of(_active.begin(), _active.end(),
        [this](uint32_t slot) { return _pages[slot].state == State::READY; });
}

int VirtualTexture::resolve(VTPageAddress address) const {
    while (address.level <= _layout.rootLevel()) {
        const auto it = _lookup.find(address.key());
        if (it != _lookup.end()) {
            const auto &p = _pages[it->second];
            if (p.lastRequested == _frame && p.state == State::READY) {
                return static_cast<int>(it->second);
            }
        }
        address = address.parent();
    }
    return -1;
}

} // namespace cc::landscape

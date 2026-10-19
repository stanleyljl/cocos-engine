// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/VirtualTexture.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
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

VTGridMapping VTPageLayout::gridMapping(LandscapeRegion region, float minY, float maxY,
                                  const Vec3 &camera, uint32_t bias) const {
    VTGridMapping result;
    std::array<bool, 256> assigned{};
    const float cell = region.size / 16.0F;
    const auto axisDistance = [](float p, float lo, float hi) {
        return std::max({lo - p, p - hi, 0.0F});
    };
    const float dy = axisDistance(camera.y, minY, maxY);
    for (uint32_t z = 0; z < 16; ++z) {
        for (uint32_t x = 0; x < 16; ++x) {
            if (assigned[z * 16 + x]) {
                continue;
            }
            // Evaluate aligned distance bands from eight cells down to one.
            // This controls page resolution only; the geometry is always whole.
            for (uint32_t cells = 8; cells > 0; cells /= 2) {
                const uint32_t left = x / cells * cells;
                const uint32_t bottom = z / cells * cells;
                const float loX = region.x + left * cell;
                const float loZ = region.z + bottom * cell;
                const float size = cells * cell;
                const float dx = axisDistance(camera.x, loX, loX + size);
                const float dz = axisDistance(camera.z, loZ, loZ + size);
                const uint32_t desired = std::min(_rootLevel, levelForDistance(std::sqrt(dx * dx + dy * dy + dz * dz)) + bias);
                if (cells > 1 && pageSize(desired) < size) {
                    continue;
                }
                // Odd edge vertices may snap to the preceding even vertex.
                // Reserve that coverage even before a neighbor changes LOD.
                const auto page = coveringPage(desired,
                    {loX - (left & 1U) * cell, loZ - (bottom & 1U) * cell},
                    {loX + size, loZ + size});
                const auto found = std::find_if(result.pages.begin(), result.pages.end(), [page](const auto &entry) {
                    return entry.key() == page.key();
                });
                const auto index = static_cast<size_t>(found - result.pages.begin());
                assert(index < result.cells.size());
                if (found == result.pages.end()) {
                    result.pages.push_back(page);
                }
                // Fill a whole band once, avoiding repeated distance queries.
                for (uint32_t row = bottom; row < bottom + cells; ++row) {
                    std::fill_n(result.cells.begin() + row * 16 + left, cells, static_cast<uint8_t>(index));
                    std::fill_n(assigned.begin() + row * 16 + left, cells, true);
                }
                break;
            }
        }
    }
    return result;
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
    assert(false && "VT coverage must not cross sector boundaries");
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
    if (pages.size() > _pages.size()) {
        return false;
    }
    for (const auto p : pages) {
        if (p.level > _layout.rootLevel()) {
            return false;
        }
        const uint64_t side = 1ULL << (_layout.rootLevel() - p.level);
        if (p.x >= _data.sectorsX * side || p.z >= _data.sectorsZ * side) {
            return false;
        }
    }
    return true;
}
bool VirtualTexture::reserve(const ccstd::vector<VTPageAddress> &pages) {
    if (!canReserve(pages)) {
        return false;
    }
    ++_reservation;
    // Protect every existing requested page before allocating any replacements.
    _active.clear();
    for (const auto p : pages) {
        const auto it = _lookup.find(p.key());
        if (it != _lookup.end()) {
            _pages[it->second].lastReserved = _reservation;
        }
    }
    for (const auto p : pages) {
        const auto it = _lookup.find(p.key());
        const int slot = it == _lookup.end() ? allocate(p) : static_cast<int>(it->second);
        assert(slot >= 0);
        _pages[slot].lastReserved = _reservation;
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
        if (p.lastReserved != _reservation && p.lastReserved < oldest) {
            oldest = p.lastReserved;
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
void VirtualTexture::publish(const ccstd::vector<uint32_t> &slots) {
    for (const auto slot : slots) {
        auto &p = _pages[slot];
        assert(p.lastReserved == _reservation && p.needsComposition());
        p.state = State::READY;
    }
    if (!slots.empty()) {
        ++_revision;
    }
}
void VirtualTexture::invalidate() {
    for (auto &p : _pages) {
        if (p.state == State::READY) {
            p.state = State::STALE;
        }
    }
    ++_revision;
}
bool VirtualTexture::ready() const {
    return !_active.empty() && std::all_of(_active.begin(), _active.end(), [this](uint32_t slot) { return _pages[slot].state == State::READY; });
}
bool VirtualTexture::needsComposition(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    return it == _lookup.end() || _pages[it->second].state != State::READY;
}
int VirtualTexture::resolve(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    if (it == _lookup.end()) {
        return -1;
    }
    const auto &p = _pages[it->second];
    return p.lastReserved == _reservation && p.drawable() ? static_cast<int>(it->second) : -1;
}
} // namespace cc::landscape

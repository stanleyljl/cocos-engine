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
#include "landscape/VTRenderer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include "base/Log.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderTexture.h"
#include "core/assets/Texture2D.h"
#include "landscape/Asset.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/Residency.h"
#include "renderer/gfx-base/GFXCommandBuffer.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXQueue.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/Pass.h"
#include "scene/RenderWindow.h"

namespace cc::landscape {
VTPageTable::VTPageTable() = default;

VTPageTable::~VTPageTable() = default;

bool VTPageTable::ensureCapacity(uint32_t count) {
    constexpr uint32_t ENTRIES = VTGridMapping::CELL_COUNT;
    uint32_t size = std::max(256U, _size);
    auto *device = Root::getInstance()->getDevice();
    while (static_cast<uint64_t>(size) * size < static_cast<uint64_t>(count) * ENTRIES) {
        size *= 2;
        if (size > device->getCapabilities().maxTextureSize) {
            return false;
        }
    }
    if (size == _size) {
        return true;
    }
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = gfx::Format::RG8;
    info.width = size;
    info.height = size;
    IntrusivePtr<gfx::Texture> texture = device->createTexture(info);
    if (!texture) {
        return false;
    }
    // Linear indices remain stable across growth; the shader uses textureSize.
    resizeEntries(static_cast<size_t>(size) * size);
    gfx::BufferTextureCopy copy;
    copy.texExtent = {size, size, 1};
    const uint8_t *bytes[]{reinterpret_cast<const uint8_t *>(_entries.data())};
    device->copyBuffersToTexture(bytes, texture, &copy, 1);
    _texture = std::move(texture);
    _size = size;
    return true;
}

uint32_t VTPageTable::allocate() {
    if (!_freeSlots.empty()) {
        const uint32_t slot = _freeSlots.back();
        _freeSlots.pop_back();
        return slot;
    }
    CC_ASSERT(static_cast<size_t>(_count + 1) * VTGridMapping::CELL_COUNT <= _entries.size());
    return _count++;
}

void VTPageTable::release(uint32_t slot) {
    CC_ASSERT(slot < _count);
    _freeSlots.push_back(slot);
}

bool VTPageTable::update(uint32_t slot, const VTGridMapping &mapping, const VTRenderer &pages) {
    static_assert(config::VT_PAGE_COUNT <= 256, "RG8 mappings store an 8-bit physical slot");
    CC_ASSERT(slot < _count);
    std::array<uint8_t, VTGridMapping::CELL_COUNT> physicalSlots{};
    CC_ASSERT(mapping.pages.size() <= physicalSlots.size());
    for (size_t i = 0; i < mapping.pages.size(); ++i) {
        const int physical = pages.resolve(mapping.pages[i]);
        CC_ASSERTF(physical >= 0, "[Landscape] exact VT page must be published before drawing");
        physicalSlots[i] = static_cast<uint8_t>(physical);
    }
    std::array<Entry, VTGridMapping::CELL_COUNT> encoded;
    for (size_t i = 0; i < mapping.cells.size(); ++i) {
        const uint8_t page = mapping.cells[i];
        CC_ASSERT(page < mapping.pages.size());
        encoded[i] = {physicalSlots[page], static_cast<uint8_t>(mapping.pages[page].level)};
    }
    const size_t offset = static_cast<size_t>(slot) * encoded.size();
    auto *destination = _entries.data() + offset;
    // A neighbor can dirty only the stitch mask. Avoid uploading an unchanged
    // material table; the CPU mirror also remains valid across texture growth.
    if (std::memcmp(encoded.data(), destination, sizeof(encoded)) == 0) {
        return false;
    }
    std::copy(encoded.begin(), encoded.end(), destination);
    return true;
}

void VTPageTable::upload(ccstd::vector<uint32_t> slots) {
    if (slots.empty()) {
        return;
    }
    std::sort(slots.begin(), slots.end());
    ccstd::vector<gfx::BufferTextureCopy> copies;
    ccstd::vector<const uint8_t *> buffers;
    copies.reserve(slots.size());
    buffers.reserve(slots.size());
    constexpr uint32_t cells = VTGridMapping::CELL_COUNT;
    for (size_t i = 0; i < slots.size(); ++i) {
        CC_ASSERT(slots[i] < _count);
        const uint32_t first = slots[i] * cells;
        uint32_t width = cells;
        // Adjacent tables share one upload, but never cross a texture row.
        while (i + 1 < slots.size() && slots[i + 1] == slots[i] + 1 &&
               slots[i + 1] * cells / _size == first / _size) {
            ++i;
            CC_ASSERT(slots[i] < _count);
            width += cells;
        }
        gfx::BufferTextureCopy copy;
        copy.texOffset = {static_cast<int32_t>(first % _size), static_cast<int32_t>(first / _size), 0};
        copy.texExtent = {width, 1, 1};
        copies.push_back(copy);
        buffers.push_back(reinterpret_cast<const uint8_t *>(_entries.data() + first));
    }
    Root::getInstance()->getDevice()->copyBuffersToTexture(buffers.data(), _texture, copies.data(), static_cast<uint32_t>(copies.size()));
}

void VTPageTable::destroy() {
    _texture = nullptr;
    _entries.clear();
    _freeSlots.clear();
    _size = 0;
    _count = 0;
}

void VTPageTable::resizeEntries(size_t count) {
    CC_ASSERT(static_cast<size_t>(_count) * VTGridMapping::CELL_COUNT <= count);
    _entries.resize(count);
}

VTRenderer::VTRenderer() = default;

VTRenderer::~VTRenderer() {
    destroy();
}

bool VTRenderer::init(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals) {
    if (!residency.valid() || !materials.valid()) {
        return false;
    }
    const bool compressed = VTPageCompressor::supported();
    if (!initResources(asset, residency, materials, decals, compressed)) {
        destroy();
        if (!compressed || !initResources(asset, residency, materials, decals, false)) {
            destroy();
            return false;
        }
        CC_LOG_WARNING("[Landscape] ASTC 4x4 initialization failed; using RGBA8 VT atlas");
    }
    CC_LOG_INFO("[Landscape] VT atlas: %s", _compressor ? "ASTC 4x4" : "RGBA8");
    _batch.reserve(config::VT_PAGE_COUNT);
    return true;
}

void VTRenderer::destroy() {
    _commands = nullptr;
    _compressor.reset();
    _mipGenerator.reset();
    _composer.reset();
    _pageTable.destroy();
    _pages.clear();
    _reservedSlots.clear();
    _lookup.clear();
    _reservation = 0;
    _batch.clear();
    _sampler = nullptr;
    _needsClear = true;
    _refineFarDetail = true;
}

uint32_t VTRenderer::renderPages(LandscapeResidency &residency, const ccstd::vector<VTPageAddress> &pages, uint32_t maxPages) {
    if (maxPages == 0) {
        return 0;
    }
    // The scheduler supplies pages of loaded nodes, including invalidated ready
    // pages. Compose only when every shared page input is resident as well.
    _batch.clear();
    for (const auto address : pages) {
        const uint32_t slot = _lookup.at(address.key());
        const auto &page = _pages[slot];
        CC_ASSERT(page.lastReserved == _reservation);
        if (!page.needsComposition()) {
            continue;
        }
        VTPageInstance instance;
        const auto inputs = selectSources(page.address, getRefineFarDetail());
        if (!_composer->prepareInputs(slot, region(page.address), inputs, residency, instance)) {
            continue;
        }
        _batch.push_back(instance);
        if (_batch.size() == maxPages) {
            break;
        }
    }
    if (_batch.empty()) {
        return 0;
    }
    _composer->uploadInputs();

    _commands->begin();
    if (_compressor) {
        _compressor->begin(*_commands, _needsClear);
        // Finish composition, mips and compression before reusing the scratch page.
        for (const auto &instance : _batch) {
            _composer->record(*_commands, &instance, 1, _needsClear);
            _mipGenerator->record(*_commands, *_composer, 1, _needsClear);
            _needsClear = false;
            _compressor->record(*_commands, instance.physicalSlot());
        }
        _compressor->end(*_commands);
    } else {
        const auto count = static_cast<uint32_t>(_batch.size());
        _composer->record(*_commands, _batch.data(), count, _needsClear);
        _mipGenerator->record(*_commands, *_composer, count, _needsClear);
        _needsClear = false;
    }
    _commands->end();

    // Submit every stage on the same queue before making these pages drawable.
    auto *device = Root::getInstance()->getDevice();
    gfx::CommandBuffer *command = _commands.get();
    device->flushCommands(&command, 1);
    device->getQueue()->submit(&command, 1);
#if CC_DEBUG && CC_LANDSCAPE_DEBUG
    const auto recomposed = std::count_if(_batch.begin(), _batch.end(), [this](const VTPageInstance &instance) {
        return page(instance.physicalSlot()).state == PageState::OUTDATED;
    });
    if (recomposed > 0) {
        CC_LOG_INFO("[Landscape] VT recomposed: %zu stale pages submitted (%s)",
                    static_cast<size_t>(recomposed), _compressor ? "ASTC 4x4" : "RGBA8");
    }
#endif
    for (const auto &instance : _batch) {
        publish(instance.physicalSlot());
    }
    return static_cast<uint32_t>(_batch.size());
}

bool VTRenderer::needsComposition(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    return it == _lookup.end() || _pages[it->second].state != PageState::READY;
}

bool VTRenderer::canReserve(const ccstd::vector<VTPageAddress> &pages) const {
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

bool VTRenderer::reserve(const ccstd::vector<VTPageAddress> &pages) {
    if (!canReserve(pages)) {
        return false;
    }
    ++_reservation;
    // First protect resident pages and remember their slots before any eviction.
    constexpr uint32_t MISSING = std::numeric_limits<uint32_t>::max();
    _reservedSlots.clear();
    for (const auto address : pages) {
        const auto found = _lookup.find(address.key());
        if (found == _lookup.end()) {
            _reservedSlots.push_back(MISSING);
            continue;
        }
        const auto slot = found->second;
        _pages[slot].lastReserved = _reservation;
        _reservedSlots.push_back(slot);
    }
    // Fill only the missing slots, preserving demand order without another lookup.
    for (size_t i = 0; i < pages.size(); ++i) {
        if (_reservedSlots[i] != MISSING) {
            continue;
        }
        const int slot = allocate(pages[i]);
        assert(slot >= 0);
        _pages[slot].lastReserved = _reservation;
        _reservedSlots[i] = static_cast<uint32_t>(slot);
    }
    return true;
}

VTPageSources VTRenderer::selectSources(VTPageAddress page, bool refineFarDetail) const {
    const float size = _layout.pageSize(page.level);
    VTPageSources result;
    result.splat = _data.nodeAtClamped(sourceLevel(size), {(page.x + 0.5F) * size, (page.z + 0.5F) * size});
    // Base precision uses one tile for the page interior. Far detail matches
    // source height density to the output page, using up to 2x2 interior tiles.
    const float target = size * std::max(0.5F, static_cast<float>(config::SOURCE_TILE_RESOLUTION - 1) / config::VT_PAGE_INTERIOR);
    const bool detailed = refineFarDetail && page.level >= config::VT_NORMAL_DETAIL_MIN_LEVEL;
    const uint32_t level = detailed ? sourceLevel(target) : result.splat.level;
    // Select only the four interior quadrants. Source padding supplies the
    // gutters, so neither precision reserves additional neighboring tiles.
    std::array<uint32_t, 2> columns;
    std::array<uint32_t, 2> rows;
    for (uint32_t i = 0; i < 2; ++i) {
        const auto source = _data.nodeAtClamped(level, {(page.x + 0.25F + i * 0.5F) * size, (page.z + 0.25F + i * 0.5F) * size});
        columns[i] = source.x;
        rows[i] = source.z;
    }
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t col = 0; col < 2; ++col) {
            result.normals[row * 2 + col] = {level, columns[col], rows[row]};
        }
    }
    return result;
}

void VTRenderer::invalidate() {
    for (auto &p : _pages) {
        if (p.state == PageState::READY) {
            p.state = PageState::OUTDATED;
        }
    }
}

void VTRenderer::invalidate(LandscapeRegion changed) {
    for (auto &page : _pages) {
        if (page.state != PageState::READY) {
            continue;
        }
        const auto bounds = region(page.address);
        const float border = bounds.size * static_cast<float>(config::VT_PAGE_BORDER) / config::VT_PAGE_INTERIOR;
        if (changed.x > bounds.x + bounds.size + border || changed.z > bounds.z + bounds.size + border ||
            changed.x + changed.size < bounds.x - border || changed.z + changed.size < bounds.z - border) {
            continue;
        }
        page.state = PageState::OUTDATED;
    }
}

bool VTRenderer::ready() const {
    return !_reservedSlots.empty() && std::all_of(_reservedSlots.begin(), _reservedSlots.end(), [this](uint32_t slot) { return _pages[slot].state == PageState::READY; });
}

int VTRenderer::resolve(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    if (it == _lookup.end()) {
        return -1;
    }
    const auto &p = _pages[it->second];
    return p.lastReserved == _reservation && p.drawable() ? static_cast<int>(it->second) : -1;
}

void VTRenderer::setGlobalColorStrength(float strength) {
    if (_composer->setGlobalColorStrength(strength)) {
        invalidate();
    }
#if CC_DEBUG && CC_LANDSCAPE_DEBUG
    auto *pass = _composer->material()->getPasses()->front().get();
    const auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("globalColorParams")));
    const auto binding = scene::Pass::getBindingFromHandle(pass->getHandle("globalColorMap"));
    const auto *texture = pass->getDescriptorSet()->getTexture(binding);
    const auto stale = std::count_if(reservedSlots().begin(), reservedSlots().end(), [this](uint32_t slot) {
        return page(slot).state == PageState::OUTDATED;
    });
    CC_LOG_INFO("[Landscape] Global color strength: requested=%.2f effective=%.2f map=%ux%u stale=%zu/%zu",
                strength, params.z, texture ? texture->getWidth() : 0U, texture ? texture->getHeight() : 0U,
                static_cast<size_t>(stale), reservedSlots().size());
#endif
}

void VTRenderer::setGlobalColorMap(Texture2D *texture) {
    _composer->setGlobalColorMap(texture);
    invalidate();
}

void VTRenderer::setHeightBlendEnabled(bool enabled) {
    if (_composer->setHeightBlendEnabled(enabled)) {
        invalidate();
    }
}

void VTRenderer::setRefineFarDetail(bool enabled) {
    if (_refineFarDetail == enabled) {
        return;
    }
    _refineFarDetail = enabled;
    for (auto &page : _pages) {
        if (page.address.level >= config::VT_NORMAL_DETAIL_MIN_LEVEL && page.state == PageState::READY) {
            page.state = PageState::OUTDATED;
        }
    }
}

TextureBase *VTRenderer::atlas() const {
    return _compressor ? static_cast<TextureBase *>(_compressor->atlas()) : _composer->target();
}

gfx::Texture *VTRenderer::albedo() const {
    return _compressor ? _compressor->albedo() : _composer->target()->getWindow()->getColorTexture(0);
}

gfx::Texture *VTRenderer::normalRoughnessAO() const {
    return _compressor ? _compressor->normalRoughnessAO() : _composer->target()->getWindow()->getColorTexture(1);
}

bool VTRenderer::initPages(const LandscapeData &data, uint32_t capacity) {
    if (capacity < static_cast<uint64_t>(data.sectorsX) * data.sectorsZ) {
        return false;
    }
    _pages.clear();
    _reservedSlots.clear();
    _lookup.clear();
    _reservation = 0;
    _data = data;
    _layout = VTPageLayout(data.sectorSize);
    _pages.resize(capacity);
    return true;
}

void VTRenderer::publish(uint32_t slot) {
    auto &page = _pages[slot];
    assert(page.lastReserved == _reservation && page.needsComposition());
    page.state = PageState::READY;
}

bool VTRenderer::initResources(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals, bool compressed) {
    _composer = std::make_unique<VTPageComposer>();
    _mipGenerator = std::make_unique<VTPageMipGenerator>();
    if (!initPages(asset.data()) || !_pageTable.ensureCapacity(1) ||
        !_composer->init(asset, residency, materials, decals, compressed) || !_mipGenerator->init(*_composer, compressed)) {
        return false;
    }
    if (compressed) {
        _compressor = std::make_unique<VTPageCompressor>();
        if (!_compressor->init(Root::getInstance()->getDevice(), *_composer->target())) {
            return false;
        }
    }
    auto *device = Root::getInstance()->getDevice();
    gfx::SamplerInfo sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = gfx::Filter::LINEAR;
    sampler.maxAnisotropy = config::VT_MAX_ANISOTROPY;
    sampler.addressU = sampler.addressV = sampler.addressW = gfx::Address::CLAMP;
    _sampler = device->getSampler(sampler);
    _commands = device->createCommandBuffer({device->getQueue(), gfx::CommandBufferType::PRIMARY});
    return _commands != nullptr;
}

uint32_t VTRenderer::sourceLevel(float size) const {
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxLevel && _data.nodeSize(level) < size) {
        ++level;
    }
    return level;
}

int VTRenderer::allocate(VTPageAddress address) {
    int slot = -1;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (uint32_t i = 0; i < _pages.size(); ++i) {
        const auto &p = _pages[i];
        if (p.state == PageState::EMPTY) {
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
    if (p.state != PageState::EMPTY) {
        _lookup.erase(p.address.key());
    }
    p = Page{};
    p.address = address;
    p.state = PageState::UNCOMPOSED;
    _lookup[address.key()] = static_cast<uint32_t>(slot);
    return slot;
}

} // namespace cc::landscape

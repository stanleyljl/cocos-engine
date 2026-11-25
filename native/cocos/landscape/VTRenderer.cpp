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
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/RenderWindow.h"

namespace cc::landscape {
VTCapacityQuery::VTCapacityQuery(const VTRenderer &renderer, const LandscapeResidency &residency)
: _renderer(renderer)
, _residency(residency) {}

bool VTCapacityQuery::canPrepare(const ccstd::vector<VTPageAddress> &pages, const ccstd::vector<NodeAddress> &sources, const ResourceRequirements &retained) {
    const auto sharedSources = std::count_if(sources.begin(), sources.end(), [&](NodeAddress source) {
        return retained.sources.count(source) != 0;
    });
    const auto sharedPages = std::count_if(pages.begin(), pages.end(), [&](VTPageAddress page) {
        return retained.pages.count(page) != 0;
    });
    const auto retainedSourceCount = retained.sources.size() - sharedSources;
    const auto retainedPageCount = retained.pages.size() - sharedPages;
    if (!_residency.canFit(sources, retainedSourceCount) || !_renderer.canFit(pages, retainedPageCount)) {
        return false;
    }
    const auto sourceCount = retainedSourceCount + sources.size();
    const auto containsSource = [&](NodeAddress source) {
        return retained.sources.count(source) != 0 || std::any_of(sources.begin(), sources.end(), [&](NodeAddress candidate) {
            return candidate.key() == source.key();
        });
    };
    const auto canCompose = [&](VTPageAddress page) {
        const auto [entry, inserted] = _pageSources.try_emplace(page.key());
        if (inserted) {
            _renderer.selectSources(page, true, true).appendTo(entry->second);
        }
        const auto additional = std::count_if(entry->second.begin(), entry->second.end(), [&](NodeAddress source) {
            return !containsSource(source);
        });
        return sourceCount + additional <= _residency.layerCount();
    };
    // New sources can also exhaust composition headroom for retained pages.
    for (const auto page : retained.pages) {
        if (!canCompose(page)) {
            return false;
        }
    }
    for (const auto page : pages) {
        if (retained.pages.count(page) == 0 && !canCompose(page)) {
            return false;
        }
    }
    return true;
}

VTPageTable::VTPageTable() = default;

VTPageTable::~VTPageTable() = default;

void VTPageTable::destroy() {
    _texture = nullptr;
    _entries.clear();
    _freeSlots.clear();
    _size = 0;
    _count = 0;
}

bool VTPageTable::canFit(uint32_t count) const {
    return requiredSize(count) != 0;
}

bool VTPageTable::ensureCapacity(uint32_t count) {
    const uint32_t size = requiredSize(count);
    if (size == 0) {
        return false;
    }
    if (size == _size) {
        return true;
    }
    auto *device = Root::getInstance()->getDevice();
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

uint32_t VTPageTable::requiredSize(uint32_t count) const {
    constexpr uint32_t ENTRIES = VTGridMapping::CELL_COUNT;
    uint32_t size = std::max(256U, _size);
    auto *device = Root::getInstance()->getDevice();
    while (static_cast<uint64_t>(size) * size < static_cast<uint64_t>(count) * ENTRIES) {
        size *= 2;
        if (size > device->getCapabilities().maxTextureSize) {
            return 0;
        }
    }
    return size <= device->getCapabilities().maxTextureSize ? size : 0;
}

void VTPageTable::resizeEntries(size_t count) {
    CC_ASSERT(static_cast<size_t>(_count) * VTGridMapping::CELL_COUNT <= count);
    _entries.resize(count);
}

VTRenderer::VTRenderer() = default;

VTRenderer::~VTRenderer() {
    destroy();
}

bool VTRenderer::init(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals, uint32_t atlasSize) {
    if (!residency.valid() || !materials.valid()) {
        return false;
    }
    const bool compressed = VTPageCompressor::supported();
    if (!initResources(asset, residency, materials, decals, atlasSize, compressed)) {
        destroy();
        if (!compressed || !initResources(asset, residency, materials, decals, atlasSize, false)) {
            destroy();
            return false;
        }
        CC_LOG_WARNING("[Landscape] ASTC 4x4 initialization failed; using RGBA8 VT atlas");
    }
    _residency = &residency;
    if (!validateRootCapacity()) {
        CC_LOG_ERROR("[Landscape] Device capacity cannot compose sector roots");
        destroy();
        return false;
    }
#if CC_LANDSCAPE_DEBUG
    CC_LOG_INFO("[Landscape] VT atlas: %ux%u %s pages=%zu", atlasSize, atlasSize, _compressor ? "ASTC 4x4" : "RGBA8", _pages.size());
#endif
    _batch.reserve(_pages.size());
    return true;
}

bool VTRenderer::validateRootCapacity() const {
    ccstd::vector<VTPageAddress> rootPages;
    ccstd::vector<NodeAddress> rootSources;
    for (uint32_t z = 0; z < _data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < _data.sectorsX; ++x) {
            rootPages.push_back({_layout.rootLevel(), x, z});
            rootSources.push_back({_data.maxTileLevel, x, z});
        }
    }
    return capacityQuery().canPrepare(rootPages, rootSources);
}

void VTRenderer::destroy() {
    for (const auto source : _sourceRequests) {
        _residency->release(source);
    }
    _sourceRequests.clear();
    _requests.clear();
    _requestsDirty = false;
    _residency = nullptr;
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

bool VTRenderer::canFit(const ccstd::vector<VTPageAddress> &pages, size_t retainedCount) const {
    if (pages.size() + retainedCount > _pages.size()) {
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

void VTRenderer::request(VTPageAddress address) {
    const auto [entry, inserted] = _requests.try_emplace(address.key(), Request{address, 0});
    ++entry->second.count;
    _requestsDirty |= inserted;
}

void VTRenderer::release(VTPageAddress address) {
    const auto entry = _requests.find(address.key());
    CC_ASSERT(entry != _requests.end());
    if (--entry->second.count == 0) {
        _requests.erase(entry);
        _requestsDirty = true;
    }
}

bool VTRenderer::update(uint32_t budget) {
    if (!applyRequests()) {
        return false;
    }
    ccstd::vector<VTPageAddress> pages;

    // Reconcile before Residency.update(), so retained inputs never lose their slots.
    for (const auto source : _sourceRequests) {
        _residency->release(source);
    }
    _sourceRequests.clear();
    ccstd::unordered_set<uint64_t> keys;
    for (const auto slot : _reservedSlots) {
        const auto page = _pages[slot].address;
        if (!needsComposition(page)) {
            continue;
        }
        ccstd::vector<NodeAddress> inputs;
        selectSources(page, _refineFarDetail, _triplanarEnabled).appendTo(inputs);
        if (!_residency->canRequest(inputs)) {
            continue;
        }
        pages.push_back(page);
        for (const auto source : inputs) {
            if (keys.insert(source.key()).second) {
                _residency->request(source);
                _sourceRequests.push_back(source);
            }
        }
    }
    // Keep this batch pinned through Residency.update() and GPU submission.
    renderPages(*_residency, pages, budget);
    return true;
}

bool VTRenderer::applyRequests() {
    if (!_requestsDirty) {
        return true;
    }
    ccstd::vector<VTPageAddress> pages;
    for (const auto &entry : _requests) {
        pages.push_back(entry.second.address);
    }
    std::sort(pages.begin(), pages.end(), [](VTPageAddress a, VTPageAddress b) {
        return a.key() < b.key();
    });
    if (!canFit(pages)) {
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
    _requestsDirty = false;
    return true;
}

uint32_t VTRenderer::renderPages(LandscapeResidency &residency, const ccstd::vector<VTPageAddress> &pages, uint32_t maxPages) {
    if (maxPages == 0) {
        return 0;
    }
    // Compose each demanded page once, only when all of its inputs are resident.
    _batch.clear();
    for (const auto address : pages) {
        const uint32_t slot = _lookup.at(address.key());
        const auto &page = _pages[slot];
        CC_ASSERT(page.lastReserved == _reservation);
        if (!page.needsComposition()) {
            continue;
        }
        VTPageInstance instance;
        const auto inputs = selectSources(page.address, getRefineFarDetail(), getTriplanarEnabled());
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

    auto &commands = *Root::getInstance()->getDevice()->getCommandBuffer();
    if (_compressor) {
        _compressor->begin(commands, _needsClear);
        // Finish composition, mips and compression before reusing the scratch page.
        for (const auto &instance : _batch) {
            _composer->record(commands, &instance, 1, _needsClear);
            _mipGenerator->record(commands, *_composer, 1, _needsClear);
            _needsClear = false;
            _compressor->record(commands, instance.physicalSlot());
        }
        _compressor->end(commands);
    } else {
        // Group whole pages by shader. Each group keeps the instance buffer alive
        // through its mip draws before the next group overwrites it.
        const auto split = std::partition(_batch.begin(), _batch.end(), [](const VTPageInstance &instance) {
            return instance.pageInfo.z == 0.0F;
        });
        const auto planarCount = static_cast<uint32_t>(split - _batch.begin());
        const std::array<uint32_t, 3> offsets{0U, planarCount, static_cast<uint32_t>(_batch.size())};
        for (size_t i = 0; i < 2; ++i) {
            const uint32_t count = offsets[i + 1] - offsets[i];
            if (count == 0) {
                continue;
            }
            _composer->record(commands, _batch.data() + offsets[i], count, _needsClear);
            _mipGenerator->record(commands, *_composer, count, _needsClear);
            _needsClear = false;
        }
    }
    for (const auto &instance : _batch) {
        publish(instance.physicalSlot());
    }
    return static_cast<uint32_t>(_batch.size());
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
    return !_requestsDirty && !_reservedSlots.empty() && std::all_of(_reservedSlots.begin(), _reservedSlots.end(), [this](uint32_t slot) { return _pages[slot].state == PageState::READY; });
}

bool VTRenderer::needsComposition(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    return it == _lookup.end() || _pages[it->second].state != PageState::READY;
}

int VTRenderer::resolve(VTPageAddress address) const {
    const auto it = _lookup.find(address.key());
    if (it == _lookup.end()) {
        return -1;
    }
    const auto &p = _pages[it->second];
    return p.lastReserved == _reservation && p.drawable() ? static_cast<int>(it->second) : -1;
}

VTPageSources VTRenderer::selectSources(VTPageAddress page, bool refineFarDetail, bool triplanar) const {
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
    result.triplanar = triplanar && hasCliff(region(page));
    return result;
}

void VTRenderer::setGlobalColorMap(Texture2D *texture) {
    _composer->setGlobalColorMap(texture);
    invalidate();
}

void VTRenderer::setGlobalColorStrength(float strength) {
    if (_composer->setGlobalColorStrength(strength)) {
        invalidate();
    }
}

void VTRenderer::setHeightBlendEnabled(bool enabled) {
    if (_composer->setHeightBlendEnabled(enabled)) {
        invalidate();
    }
}

void VTRenderer::setTriplanarEnabled(bool enabled) {
    if (enabled == _triplanarEnabled) {
        return;
    }
    _triplanarEnabled = enabled;
    invalidate();
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
    return _compressor ? _compressor->albedo() : _composer->albedo();
}

gfx::Texture *VTRenderer::normalRoughness() const {
    return _compressor ? _compressor->normalRoughness() : _composer->normal();
}

bool VTRenderer::initPages(const LandscapeAsset &asset, uint32_t capacity) {
    const auto &data = asset.data();
    if (capacity < static_cast<uint64_t>(data.sectorsX) * data.sectorsZ) {
        return false;
    }
    _pages.clear();
    _reservedSlots.clear();
    _lookup.clear();
    _reservation = 0;
    _data = data;
    _asset = &asset;
    _layout = VTPageLayout(data.sectorSize);
    _pages.resize(capacity);
    return true;
}

void VTRenderer::publish(uint32_t slot) {
    auto &page = _pages[slot];
    assert(page.lastReserved == _reservation && page.needsComposition());
    page.state = PageState::READY;
}

bool VTRenderer::initResources(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals, uint32_t atlasSize, bool compressed) {
    _composer = std::make_unique<VTPageComposer>();
    _mipGenerator = std::make_unique<VTPageMipGenerator>();
    const uint32_t side = atlasSize / config::VT_PAGE_RES;
    if (!initPages(asset, side * side) || !_pageTable.ensureCapacity(1) ||
        !_composer->init(asset, residency, materials, decals, atlasSize, compressed) || !_mipGenerator->init(*_composer, compressed)) {
        return false;
    }
    if (compressed) {
        _compressor = std::make_unique<VTPageCompressor>();
        if (!_compressor->init(Root::getInstance()->getDevice(), *_composer->target(), atlasSize)) {
            return false;
        }
    }
    auto *device = Root::getInstance()->getDevice();
    gfx::SamplerInfo sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = gfx::Filter::LINEAR;
    sampler.maxAnisotropy = config::VT_MAX_ANISOTROPY;
    sampler.addressU = sampler.addressV = sampler.addressW = gfx::Address::CLAMP;
    _sampler = device->getSampler(sampler);
    return true;
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

uint32_t VTRenderer::sourceLevel(float size) const {
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxTileLevel && _data.nodeSize(level) < size) {
        ++level;
    }
    return level;
}

bool VTRenderer::hasCliff(LandscapeRegion region) const {
    // Estimate slope from each region's height span and size, including gutters.
    // This coarse classification can miss cliffs narrower than the region.
    const float border = region.size * config::VT_PAGE_BORDER / config::VT_PAGE_INTERIOR;
    uint32_t level = 0;
    while (level < _data.maxTileLevel && _data.nodeSize(level) < region.size + 2 * border) {
        ++level;
    }
    const auto first = _data.nodeAtClamped(level, {region.x - border, region.z - border});
    const auto last = _data.nodeAtClamped(level, {region.x + region.size + border, region.z + region.size + border});
    const float heightThreshold = _data.nodeSize(level) * std::sqrt(1.0F / (config::TRIPLANAR_START_NORMAL_Y * config::TRIPLANAR_START_NORMAL_Y) - 1.0F);
    const auto &ranges = _asset->heightRanges()[level];
    for (uint32_t z = first.z; z <= last.z; ++z) {
        for (uint32_t x = first.x; x <= last.x; ++x) {
            const auto &range = ranges[static_cast<size_t>(z) * _data.nodesX(level) + x];
            if (range.maxY - range.minY > heightThreshold) {
                return true;
            }
        }
    }
    return false;
}


} // namespace cc::landscape

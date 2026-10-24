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

#include "landscape/Residency.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "base/Log.h"
#include "base/Macros.h"
#include "landscape/Asset.h"
#include "landscape/Config.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDef.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"

namespace cc {
namespace landscape {

LandscapeResidency::LandscapeResidency() = default;

LandscapeResidency::~LandscapeResidency() {
    destroy();
}

bool LandscapeResidency::init(gfx::Device *device, LandscapeAsset *asset, uint32_t layerCount, bool heightUnorm) {
    CC_ASSERT(_device == nullptr);
    if (device == nullptr || asset == nullptr || !asset->valid() || layerCount == 0) {
        return false;
    }
    if (!initSlots(asset->data(), layerCount)) {
        return false;
    }
    // Check capabilities before createTexture reaches the validator assertion.
    // Normal arrays require linearly filtered RG8 sampling.
    const auto filtered = gfx::FormatFeature::SAMPLED_TEXTURE | gfx::FormatFeature::LINEAR_FILTER;
    if (!hasAllFlags(device->getFormatFeatures(gfx::Format::RG8), filtered) ||
        !hasAllFlags(device->getFormatFeatures(gfx::Format::R16UI), gfx::FormatFeature::SAMPLED_TEXTURE)) {
        CC_LOG_ERROR("[Landscape] device requires filtered RG8 and sampled R16UI textures");
        return false;
    }
    _device = device;
    _asset = asset;
    _heightUnorm = heightUnorm;
    if (!initTextures()) {
        destroy();
        return false;
    }
    initSamplers();

    return true;
}

void LandscapeResidency::destroy() {
    _data = {};
    _capacity = 0;
    _slots.clear();
    _working.clear();
    _heightUnorm = false;
    _inFlight.clear();
    _asset = nullptr;
    _heightArray = nullptr;
    _splatArray = nullptr;
    _normalArray = nullptr;
    _heightSampler = nullptr;
    _splatSampler = nullptr;
    _device = nullptr;
}

bool LandscapeResidency::canReserve(const ccstd::vector<NodeAddress> &sources) const {
    if (sources.size() > _capacity) {
        return false;
    }
    for (const auto source : sources) {
        if (!validSource(source)) {
            return false;
        }
    }
    return true;
}

bool LandscapeResidency::reserve(const ccstd::vector<NodeAddress> &sources) {
    if (!canReserve(sources)) {
        return false;
    }
    // Build the replacement before changing live ownership. Existing sources
    // retain their layer and readiness; new ones receive free layers.
    ccstd::unordered_map<uint64_t, Slot> next;
    ccstd::vector<bool> used(_capacity, false);
    for (const auto source : sources) {
        const auto previous = _slots.find(source.key());
        const Slot slot = previous == _slots.end() ? Slot{_capacity, false} : previous->second;
        if (!next.emplace(source.key(), slot).second) {
            return false;
        }
        if (slot.layer < _capacity) {
            used[slot.layer] = true;
        }
    }
    uint32_t layer = 0;
    for (const auto source : sources) {
        auto &slot = next.at(source.key());
        if (slot.layer < _capacity) {
            continue;
        }
        while (used[layer]) {
            ++layer;
        }
        slot.layer = layer;
        used[layer] = true;
    }
    _slots = std::move(next);
    _working = sources;
    return true;
}

bool LandscapeResidency::loadReserved(bool synchronous) {
    if (_asset->loadingFailed()) {
        return false;
    }
    constexpr size_t MAX_IN_FLIGHT = 8;
    // Decode into the upload representation on the worker, avoiding a second
    // per-texel conversion on the main thread for hardware-filtered heights.
    const auto heightFormat = _heightUnorm ? gfx::Format::R16UI : gfx::Format::RG8;
    for (const auto source : _working) {
        auto &slot = _slots.at(source.key());
        if (slot.ready || _inFlight.count(source.key())) {
            continue;
        }
        if (synchronous) {
            LandscapeAsset::TileData tile;
            if (!_asset->loadTileSet(source.level, source.x, source.z, heightFormat, tile)) {
                return false;
            }
            uploadTile(tile.key, tile.height.data(), tile.splat.data(), tile.normal.data());
            continue;
        }
        if (_inFlight.size() >= MAX_IN_FLIGHT) {
            break;
        }
        if (!_asset->requestTile(source.level, source.x, source.z, heightFormat)) {
            return false;
        }
        _inFlight.insert(source.key());
    }
    return !_asset->loadingFailed();
}

void LandscapeResidency::update(uint32_t budget) {
    LandscapeAsset::TileData tile;
    for (uint32_t count = 0; count < budget && _asset->takeReadyTile(tile); ++count) {
        _inFlight.erase(tile.key);
        uploadTile(tile.key, tile.height.data(), tile.splat.data(), tile.normal.data());
    }
}

int LandscapeResidency::resident(NodeAddress source) const {
    const auto it = _slots.find(source.key());
    return it != _slots.end() && it->second.ready ? static_cast<int>(it->second.layer) : -1;
}

LandscapeResidency::Tile LandscapeResidency::resolve(NodeAddress node) const {
    const uint32_t level = std::max(node.level, _data.minTileLevel);
    const NodeAddress source{level, node.x >> (level - node.level), node.z >> (level - node.level)};
    return {source, resident(source)};
}

Vec4 LandscapeResidency::shaderParams(const Tile &tile) const {
    const auto region = _data.nodeRegion(tile.address);
    return {region.x, region.z, region.size, static_cast<float>(tile.layer)};
}

bool LandscapeResidency::ready() const {
    return std::all_of(_slots.begin(), _slots.end(), [](const auto &e) { return e.second.ready; });
}

bool LandscapeResidency::initSlots(const LandscapeData &data, uint32_t layerCount) {
    CC_ASSERT(_device == nullptr && _slots.empty() && _inFlight.empty());
    const size_t rootCount = static_cast<size_t>(data.sectorsX) * data.sectorsZ;
    if (!data.valid() || layerCount == 0) {
        return false;
    }
    if (rootCount > layerCount) {
        CC_LOG_WARNING("[Landscape] source data manager has %u layers but needs %zu permanent root tiles", layerCount, rootCount);
        return false;
    }
    _data = data;
    _capacity = layerCount;
    return true;
}

int LandscapeResidency::reservedLayer(NodeAddress source) const {
    const auto it = _slots.find(source.key());
    return it != _slots.end() ? static_cast<int>(it->second.layer) : -1;
}

void LandscapeResidency::publish(uint64_t key) {
    _slots.at(key).ready = true;
}

bool LandscapeResidency::validSource(NodeAddress source) const {
    return source.level >= _data.minTileLevel && source.level <= _data.maxLevel &&
           source.x < _data.nodesX(source.level) && source.z < _data.nodesZ(source.level);
}

bool LandscapeResidency::initTextures() {
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D_ARRAY;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = _heightUnorm ? gfx::Format::R16_UNORM : gfx::Format::RG8;
    info.width = config::SOURCE_TILE_RESOLUTION;
    info.height = config::SOURCE_TILE_RESOLUTION;
    info.layerCount = _capacity;
    info.levelCount = 1;
    _heightArray = _device->createTexture(info);
    info.format = gfx::Format::R16UI;
    _splatArray = _device->createTexture(info);
    info.format = gfx::Format::RG8;
    info.width = config::SOURCE_NORMAL_RESOLUTION;
    info.height = config::SOURCE_NORMAL_RESOLUTION;
    _normalArray = _device->createTexture(info);
    if (!valid()) {
        CC_LOG_WARNING("[Landscape] failed to create height/splat/normal arrays");
        return false;
    }
    return true;
}

void LandscapeResidency::initSamplers() {
    gfx::SamplerInfo si;
    // R16_UNORM heights and normals use hardware filtering. RG8 heights are
    // decoded before interpolation with texelFetch, which ignores the sampler.
    si.minFilter = gfx::Filter::LINEAR;
    si.magFilter = gfx::Filter::LINEAR;
    si.mipFilter = gfx::Filter::NONE;
    si.addressU = gfx::Address::CLAMP;
    si.addressV = gfx::Address::CLAMP;
    si.addressW = gfx::Address::CLAMP;
    _heightSampler = _device->getSampler(si);
    si.minFilter = gfx::Filter::POINT;
    si.magFilter = gfx::Filter::POINT;
    _splatSampler = _device->getSampler(si);
}

void LandscapeResidency::uploadTile(uint64_t key, const uint8_t *height, const uint8_t *splat, const uint8_t *normal) {
    const auto it = _slots.find(key);
    if (it == _slots.end() || it->second.ready) {
        return;
    }
    auto &slot = it->second;
    uploadLayer(_heightArray, slot.layer, height);
    uploadLayer(_splatArray, slot.layer, splat);
    uploadLayer(_normalArray, slot.layer, normal);
    publish(key);
}

void LandscapeResidency::uploadLayer(gfx::Texture *array, uint32_t layer, const uint8_t *data) const {
    if (array == nullptr || data == nullptr) {
        return;
    }
    gfx::BufferTextureCopy region;
    region.texExtent.width = array->getWidth();
    region.texExtent.height = array->getHeight();
    region.texExtent.depth = 1;
    region.texSubres.mipLevel = 0;
    region.texSubres.baseArrayLayer = layer;
    region.texSubres.layerCount = 1;
    const uint8_t *buffers[1]{data};
    _device->copyBuffersToTexture(buffers, array, &region, 1);
}

} // namespace landscape
} // namespace cc

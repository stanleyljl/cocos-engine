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

#include "landscape/TilePagePool.h"
#include "landscape/VirtualTexture.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "base/Log.h"
#include "base/Macros.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/LandscapeConfig.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDef.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"

namespace cc {
namespace landscape {

TilePagePool::TilePagePool() = default;

TilePagePool::~TilePagePool() {
    destroy();
}

bool TilePagePool::supportsHeightUnorm(gfx::Device *device) {
    return device != nullptr && hasAllFlags(device->getFormatFeatures(gfx::Format::R16_UNORM),
                                            gfx::FormatFeature::SAMPLED_TEXTURE | gfx::FormatFeature::LINEAR_FILTER);
}

bool TilePagePool::init(gfx::Device *device, LandscapeAsset *asset, uint32_t layerCount) {
    CC_ASSERT(_device == nullptr);
    if (device == nullptr || asset == nullptr || !asset->valid() || layerCount == 0) {
        return false;
    }
    const auto &data = asset->data();
    const size_t rootCount = static_cast<size_t>(data.sectorsX) * data.sectorsZ;
    if (rootCount > layerCount) {
        CC_LOG_WARNING("[Landscape] page pool has %u layers but needs %zu permanent root pages", layerCount, rootCount);
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
    _tileRes = data.tileResolution;
    _layerCount = layerCount;
    _reservations.data = data;
    _reservations.capacity = layerCount;
    _heightUnorm = supportsHeightUnorm(device);
    if (!initTextures()) {
        destroy();
        return false;
    }
    initSamplers();
#if CC_LANDSCAPE_DEBUG
    CC_LOG_INFO("[Landscape] height format: %s (2 bytes/texel)",
                _heightUnorm ? "R16_UNORM, hardware filtering" : "RG8, manual interpolation");
    CC_LOG_INFO("[Landscape] %zu sector roots; RG8 XZ normals %ux%ux%u", rootCount, _tileRes, _tileRes, _layerCount);
#endif
    return true;
}

bool TilePagePool::initTextures() {
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D_ARRAY;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = _heightUnorm ? gfx::Format::R16_UNORM : gfx::Format::RG8;
    info.width = _tileRes;
    info.height = _tileRes;
    info.layerCount = _layerCount;
    info.levelCount = 1;
    _heightArray = _device->createTexture(info);
    info.format = gfx::Format::R16UI;
    _splatArray = _device->createTexture(info);
    info.format = gfx::Format::RG8;
    _normalArray = _device->createTexture(info);
    if (!valid()) {
        CC_LOG_WARNING("[Landscape] failed to create height/splat/normal arrays");
        return false;
    }
    return true;
}

void TilePagePool::initSamplers() {
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

void TilePagePool::uploadLayer(gfx::Texture *array, uint32_t layer, const uint8_t *data) const {
    if (array == nullptr || data == nullptr) {
        return;
    }
    gfx::BufferTextureCopy region;
    region.texExtent.width = _tileRes;
    region.texExtent.height = _tileRes;
    region.texExtent.depth = 1;
    region.texSubres.mipLevel = 0;
    region.texSubres.baseArrayLayer = layer;
    region.texSubres.layerCount = 1;
    const uint8_t *buffers[1]{data};
    _device->copyBuffersToTexture(buffers, array, &region, 1);
}

bool TilePagePool::canReserve(const ccstd::vector<NodeAddress> &sources) const {
    return _reservations.canReserve(sources);
}
bool TilePagePool::reserve(const ccstd::vector<NodeAddress> &sources) {
    return _reservations.reserve(sources);
}
int TilePagePool::resident(NodeAddress source) const {
    return _reservations.resident(source);
}
bool TilePagePool::ready() const {
    return std::all_of(_reservations.slots.begin(), _reservations.slots.end(), [](const auto &e) { return e.second.ready; });
}
bool TilePagePool::loadReserved(bool synchronous) {
    if (_asset->loadingFailed()) {
        return false;
    }
    constexpr size_t MAX_IN_FLIGHT = 8;
    // Decode into the upload representation on the worker, avoiding a second
    // per-texel conversion on the main thread for hardware-filtered heights.
    const auto heightFormat = _heightUnorm ? gfx::Format::R16UI : gfx::Format::RG8;
    for (const auto source : _reservations.working) {
        auto &slot = _reservations.slots.at(source.key());
        if (slot.ready || _inFlight.count(source.key())) {
            continue;
        }
        if (synchronous) {
            LandscapeAsset::TileData tile;
            if (!_asset->loadTileSet(source.level, source.x, source.z, heightFormat, tile)) {
                return false;
            }
            uploadLayer(_heightArray, slot.layer, tile.height.data());
            uploadLayer(_splatArray, slot.layer, tile.splat.data());
            uploadLayer(_normalArray, slot.layer, tile.normal.data());
            slot.ready = true;
            ++_updateRevision;
        } else {
            if (_inFlight.size() >= MAX_IN_FLIGHT) {
                break;
            }
            if (!_asset->requestTile(source.level, source.x, source.z, heightFormat)) {
                return false;
            }
            _inFlight.insert(source.key());
        }
    }
    return !_asset->loadingFailed();
}
void TilePagePool::update(uint32_t budget) {
    LandscapeAsset::TileData tile;
    for (uint32_t count = 0; count < budget && _asset->takeReadyTile(tile); ++count) {
        _inFlight.erase(tile.key);
        const auto it = _reservations.slots.find(tile.key);
        if (it == _reservations.slots.end() || it->second.ready) {
            continue;
        }
        auto &slot = it->second;
        uploadLayer(_heightArray, slot.layer, tile.height.data());
        uploadLayer(_splatArray, slot.layer, tile.splat.data());
        uploadLayer(_normalArray, slot.layer, tile.normal.data());
        slot.ready = true;
        ++_updateRevision;
    }
}
void TilePagePool::destroy() {
    _reservations.slots.clear();
    _reservations.working.clear();
    _inFlight.clear();
    _asset = nullptr;
    _heightArray = nullptr;
    _splatArray = nullptr;
    _normalArray = nullptr;
    _heightSampler = nullptr;
    _splatSampler = nullptr;
    _device = nullptr;
}
TilePageResolver::TilePageResolver(TilePagePool &pool, const LandscapeData &data)
: _pool(pool), _data(data), _vtLayout(data.sectorSize) {}
TilePageResolver::Tile TilePageResolver::resolve(NodeAddress node) const {
    const uint32_t level = std::max(node.level, _data.minTileLevel);
    const NodeAddress source{level, node.x >> (level - node.level), node.z >> (level - node.level)};
    return {source, _pool.resident(source)};
}
Vec4 TilePageResolver::shaderParams(const Tile &tile) const {
    const auto region = _data.nodeRegion(tile.address);
    return {region.x, region.z, region.size, static_cast<float>(tile.layer)};
}
uint32_t TilePageResolver::sourceLevelForSize(float size) const {
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxLevel && _data.nodeSize(level) < size) {
        ++level;
    }
    return level;
}
NodeAddress TilePageResolver::splatSource(VTPageAddress page) const {
    const float size = _vtLayout.pageSize(page.level);
    return _data.nodeAtClamped(sourceLevelForSize(size), {(page.x + 0.5F) * size, (page.z + 0.5F) * size});
}
std::array<NodeAddress, config::VT_NORMAL_SOURCE_COUNT> TilePageResolver::normalSources(VTPageAddress page, bool bake) const {
    std::array<NodeAddress, config::VT_NORMAL_SOURCE_COUNT> result;
    if (!bake) {
        result.fill(splatSource(page));
        return result;
    }
    const float size = _vtLayout.pageSize(page.level);
    const float target = size * std::max(0.5F, static_cast<float>(_data.tileResolution - 1) / config::VT_PAGE_INTERIOR);
    const uint32_t level = sourceLevelForSize(target);
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t col = 0; col < 4; ++col) {
            result[row * 4 + col] = _data.nodeAtClamped(level, {(page.x - 0.25F + col * 0.5F) * size, (page.z - 0.25F + row * 0.5F) * size});
        }
    }
    return result;
}
void TilePageResolver::pageSources(VTPageAddress page, bool bake, ccstd::vector<NodeAddress> &output) const {
    output.push_back(splatSource(page));
    const auto normals = normalSources(page, bake);
    output.insert(output.end(), normals.begin(), normals.end());
}
bool TilePageResolver::resolvePage(VTPageAddress page, bool bake, VTPageInputs &output) const {
    const auto splat = resolve(splatSource(page));
    if (splat.layer < 0) {
        return false;
    }
    const auto region = _vtLayout.pageRegion(page);
    output.region = {region.x, region.z, region.size, 0};
    output.splatSource = shaderParams(splat);
    const auto normals = normalSources(page, bake);
    for (size_t i = 0; i < normals.size(); ++i) {
        const auto tile = resolve(normals[i]);
        if (tile.layer < 0) {
            return false;
        }
        output.normalSources[i] = shaderParams(tile);
    }
    return true;
}
} // namespace landscape
} // namespace cc

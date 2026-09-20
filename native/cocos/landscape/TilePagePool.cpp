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
    // Height and normal arrays both require linearly filtered RG8 sampling.
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

    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D_ARRAY;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = gfx::Format::RG8;
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
        destroy();
        return false;
    }

    gfx::SamplerInfo si;
    // RG8 unpacking is a linear combination of both normalized channels, so
    // hardware filtering the packed channels is equivalent to filtering the
    // reconstructed 16-bit height.
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

    // Fill all three arrays with root data before any model can render.
    // Roots deliberately stay out of the LRU, even when outside the view.
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            LandscapeAsset::TileData tile;
            if (!_asset->loadRootTile(x, z, tile)) {
                CC_LOG_WARNING("[Landscape] cannot initialize height/splat/normal root L%u (%u,%u)", data.maxLevel, x, z);
                destroy();
                return false;
            }
            const uint32_t layer = z * data.sectorsX + x;
            uploadLayer(_heightArray, layer, tile.height.data());
            uploadLayer(_splatArray, layer, tile.splat.data());
            uploadLayer(_normalArray, layer, tile.normal.data());
            _resident[tile.key] = layer;
        }
    }
    _freeLayers.reserve(_layerCount);
    for (uint32_t layer = _layerCount; layer > rootCount; --layer) {
        _freeLayers.push_back(layer - 1);
    }
#if CC_LANDSCAPE_DEBUG
    CC_LOG_INFO("[Landscape] %zu root height/splat/normal pages resident; RG8 XZ normals %ux%ux%u", rootCount, _tileRes, _tileRes, _layerCount);
#endif
    return true;
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

void TilePagePool::beginFrame() {
    _inUse.clear();
}

int TilePagePool::query(uint32_t level, uint32_t x, uint32_t z) {
    const uint64_t key = makeNodeKey(level, x, z);
    if (!valid()) {
        return -1;
    }
    _inUse.insert(key);
    const auto it = _resident.find(key);
    if (it != _resident.end()) {
        touchLRU(key);
        return static_cast<int>(it->second);
    }
    // Not resident yet: ask the asset to decode it asynchronously. Return -1
    // so the caller can fall back to a resident ancestor tile.
    if (_asset != nullptr) {
        _asset->requestTile(level, x, z);
    }
    return -1;
}

int TilePagePool::peekResident(uint64_t key) {
    if (!valid()) {
        return -1;
    }
    const auto it = _resident.find(key);
    if (it == _resident.end()) {
        return -1;
    }
    // Used as a streaming fallback this frame: protect it from eviction and keep
    // it warm in the LRU so it stays available until the real tile arrives.
    _inUse.insert(key);
    touchLRU(key);
    return static_cast<int>(it->second);
}

void TilePagePool::update(uint32_t maxUploads) {
    if (!valid()) {
        return;
    }
    // The asset owns worker threads and decoded data. Consume at most the
    // upload budget here, without doing any file or PNG work on this class.
    uint32_t done = 0;
    if (_asset != nullptr) {
        uint64_t failedKey = 0;
        while (_asset->takeFailedTile(failedKey)) {
            ++_updateRevision; // Let a stationary renderer retry failed sources.
        }
    }
    LandscapeAsset::TileData rt;
    while (_asset != nullptr && done < maxUploads && _asset->takeReadyTile(rt)) {
        ++_updateRevision;
        if (_resident.find(rt.key) != _resident.end()) {
            continue; // already uploaded via an earlier duplicate
        }
        const int layer = acquireLayer();
        if (layer < 0) {
            // Every resident tile is needed this frame: the visible set exceeds
            // the pool. Drop the decode (it re-requests when queried) and warn once.
            if (!_warnedFull) {
                _warnedFull = true;
                CC_LOG_WARNING("[Landscape] height/splat page pool full (%u layers); using resident ancestors. Increase PAGE_POOL_LAYERS.", _layerCount);
            }
            continue;
        }
        uploadLayer(_heightArray, static_cast<uint32_t>(layer), rt.height.data());
        uploadLayer(_splatArray, static_cast<uint32_t>(layer), rt.splat.data());
        uploadLayer(_normalArray, static_cast<uint32_t>(layer), rt.normal.data());
        // Publish only after all three textures occupy the same layer.
        _resident[rt.key] = static_cast<uint32_t>(layer);
        touchLRU(rt.key);
        ++done;
    }
}

void TilePagePool::touchLRU(uint64_t key) {
    if ((key >> NODE_KEY_LEVEL_SHIFT) == _asset->data().maxLevel) {
        return; // roots are permanent and must never become eviction candidates
    }
    const auto it = _lruIter.find(key);
    if (it != _lruIter.end()) {
        // Move the existing entry without allocating; its iterator stays valid.
        _lru.splice(_lru.end(), _lru, it->second);
        return;
    }
    _lru.push_back(key); // most-recently-used at the back
    _lruIter[key] = std::prev(_lru.end());
}

int TilePagePool::acquireLayer() {
    if (!_freeLayers.empty()) {
        const uint32_t layer = _freeLayers.back();
        _freeLayers.pop_back();
        return static_cast<int>(layer);
    }
    // Evict the least-recently-used tile that is NOT needed this frame (front of
    // the LRU list is oldest). Tiles requested this frame are protected.
    for (auto it = _lru.begin(); it != _lru.end(); ++it) {
        const uint64_t victim = *it;
        if (_inUse.find(victim) != _inUse.end()) {
            continue;
        }
        const auto res = _resident.find(victim);
        const uint32_t layer = res->second;
        _resident.erase(res);
        _lruIter.erase(victim);
        _lru.erase(it);
        return static_cast<int>(layer);
    }
    return -1; // nothing evictable (all resident tiles are in use this frame)
}

void TilePagePool::destroy() {
    _resident.clear();
    _lru.clear();
    _lruIter.clear();
    _freeLayers.clear();
    _inUse.clear();
    _asset = nullptr;
    _heightArray = nullptr;
    _splatArray = nullptr;
    _normalArray = nullptr;
    _heightSampler = nullptr;
    _splatSampler = nullptr;
    _device = nullptr;
    _warnedFull = false;
}

} // namespace landscape
} // namespace cc

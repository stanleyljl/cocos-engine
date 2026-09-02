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
#include "base/ThreadPool.h"
#include "platform/FileUtils.h"
#include "platform/Image.h"
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

bool TilePagePool::init(gfx::Device *device, gfx::Format format, uint32_t tileRes, uint32_t layerCount) {
    if (device == nullptr || tileRes == 0 || layerCount == 0) {
        return false;
    }
    const uint32_t formatIndex = static_cast<uint32_t>(format);
    if (formatIndex >= static_cast<uint32_t>(gfx::Format::COUNT)) {
        CC_LOG_WARNING("[Landscape] TilePagePool: invalid tile format %u", formatIndex);
        return false;
    }
    const uint32_t bytesPerTexel = gfx::GFX_FORMAT_INFOS[formatIndex].size;
    if (bytesPerTexel == 0) {
        CC_LOG_WARNING("[Landscape] TilePagePool: unsupported tile format %u (0 bytes/texel)", static_cast<uint32_t>(format));
        return false;
    }
    _device = device;
    _format = format;
    _bytesPerTexel = bytesPerTexel;
    _tileRes = tileRes;
    _layerCount = layerCount;

    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D_ARRAY;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = format;
    info.width = _tileRes;
    info.height = _tileRes;
    info.layerCount = _layerCount;
    info.levelCount = 1;
    _array = _device->createTexture(info);
    if (_array == nullptr) {
        CC_LOG_WARNING("[Landscape] TilePagePool: failed to create TEX2D_ARRAY (format %u, %ux%ux%u)",
                       static_cast<uint32_t>(format), _tileRes, _tileRes, _layerCount);
        _device = nullptr;
        return false;
    }

    gfx::SamplerInfo si;
    si.minFilter = gfx::Filter::POINT;
    si.magFilter = gfx::Filter::POINT;
    si.mipFilter = gfx::Filter::NONE;
    si.addressU = gfx::Address::CLAMP;
    si.addressV = gfx::Address::CLAMP;
    si.addressW = gfx::Address::CLAMP;
    _sampler = _device->getSampler(si);

    // Layer 0 = flat fallback (0). Layers 1..N-1 start free.
    uploadZeroLayer(FLAT_LAYER);
    _freeLayers.clear();
    _freeLayers.reserve(_layerCount);
    for (uint32_t layer = _layerCount; layer > 1; --layer) {
        _freeLayers.push_back(layer - 1); // pushes N-1 .. 1 (pop from back gives 1 first)
    }
    _resident.clear();
    _lru.clear();
    _lruIter.clear();
    _pending.clear();
    _inUse.clear();
    _stage.clear();
    _async = std::make_shared<AsyncState>();
    _warnedFull = false;
    return true;
}

void TilePagePool::uploadZeroLayer(uint32_t layer) const {
    if (_array == nullptr) {
        return;
    }
    const uint32_t bytes = _tileRes * _tileRes * _bytesPerTexel;
    ccstd::vector<uint8_t> zeros(bytes, 0);
    uploadLayer(layer, zeros.data());
}

void TilePagePool::uploadLayer(uint32_t layer, const uint8_t *data) const {
    if (_array == nullptr || data == nullptr) {
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
    _device->copyBuffersToTexture(buffers, _array, &region, 1);
}

void TilePagePool::beginFrame() {
    _inUse.clear();
}

int TilePagePool::query(uint64_t key, const ccstd::string &tilePath) {
    if (_array == nullptr) {
        return -1;
    }
    _inUse.insert(key);
    const auto it = _resident.find(key);
    if (it != _resident.end()) {
        touchLRU(key);
        return static_cast<int>(it->second);
    }
    // Not resident yet: kick off an async load. Return -1 so the caller can fall
    // back to a resident ancestor tile (see LandscapeRenderer::resolvePage).
    requestLoad(key, tilePath);
    return -1;
}

int TilePagePool::peekResident(uint64_t key) {
    if (_array == nullptr) {
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

void TilePagePool::requestLoad(uint64_t key, const ccstd::string &tilePath) {
    if (_pending.find(key) != _pending.end()) {
        return; // already in flight (dedup)
    }
    _pending.insert(key);

    auto async = _async; // shared_ptr copy keeps the queue alive past pool teardown
    const uint32_t tileRes = _tileRes;
    const gfx::Format format = _format;
    const uint32_t bytesPerTexel = _bytesPerTexel;
    LegacyThreadPool::getDefaultThreadPool()->pushTask(
        [async, key, tilePath, tileRes, format, bytesPerTexel](int /*threadId*/) {
            auto reportFailure = [async, key]() {
                std::lock_guard<std::mutex> lock(async->mutex);
                async->failed.push_back(key);
            };
            if (async->cancelled.load()) {
                return;
            }
            const Data data = FileUtils::getInstance()->getDataFromFile(tilePath);
            if (data.isNull()) {
                CC_LOG_WARNING("[Landscape] failed to load tile: %s", tilePath.c_str());
                reportFailure();
                return;
            }
            IntrusivePtr<Image> image = ccnew Image();
            if (!image->initWithImageData(data.getBytes(), data.getSize())) {
                reportFailure();
                return;
            }
            if (image->getRenderFormat() != format ||
                static_cast<uint32_t>(image->getWidth()) != tileRes ||
                static_cast<uint32_t>(image->getHeight()) != tileRes) {
                reportFailure();
                return;
            }
            ReadyTile rt;
            rt.key = key;
            const uint8_t *src = image->getData();
            rt.data.assign(src, src + static_cast<size_t>(tileRes) * tileRes * bytesPerTexel);
            {
                std::lock_guard<std::mutex> lock(async->mutex);
                async->ready.push_back(std::move(rt));
            }
        },
        LegacyThreadPool::TaskType::IO);
}

void TilePagePool::update(uint32_t maxUploads) {
    if (_array == nullptr) {
        return;
    }
    // Move any worker-decoded tiles into the main-thread upload stage.
    {
        std::lock_guard<std::mutex> lock(_async->mutex);
        if (!_async->failed.empty()) {
            for (const uint64_t key : _async->failed) {
                _pending.erase(key);
            }
            _async->failed.clear();
        }
        if (!_async->ready.empty()) {
            for (auto &rt : _async->ready) {
                _stage.push_back(std::move(rt));
            }
            _async->ready.clear();
        }
    }
    // Upload up to the per-frame budget; the rest waits for the next frame.
    uint32_t done = 0;
    while (done < maxUploads && !_stage.empty()) {
        ReadyTile rt = std::move(_stage.front());
        _stage.pop_front();
        _pending.erase(rt.key);
        if (_resident.find(rt.key) != _resident.end()) {
            continue; // already uploaded via an earlier duplicate
        }
        const int layer = acquireLayer();
        if (layer < 0) {
            // Every resident tile is needed this frame: the visible set exceeds
            // the pool. Drop the decode (it re-requests when queried) and warn once.
            if (!_warnedFull) {
                _warnedFull = true;
                CC_LOG_WARNING("[Landscape] height page pool full (%u layers); some tiles render flat. Increase PAGE_POOL_LAYERS.", _layerCount);
            }
            continue;
        }
        uploadLayer(static_cast<uint32_t>(layer), rt.data.data());
        _resident[rt.key] = static_cast<uint32_t>(layer);
        touchLRU(rt.key);
        ++done;
    }
}

void TilePagePool::touchLRU(uint64_t key) {
    const auto it = _lruIter.find(key);
    if (it != _lruIter.end()) {
        _lru.erase(it->second);
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
    if (_async) {
        // In-flight IO tasks hold their own shared_ptr to _async; flag them to
        // bail early, then drop our reference. Their decoded results (if any)
        // land in the now-orphaned queue and are freed with the last task.
        _async->cancelled.store(true);
    }
    _resident.clear();
    _lru.clear();
    _lruIter.clear();
    _freeLayers.clear();
    _pending.clear();
    _inUse.clear();
    _stage.clear();
    _async.reset();
    _array = nullptr; // IntrusivePtr releases the gfx texture
    _sampler = nullptr;
    _device = nullptr;
    _warnedFull = false;
}

} // namespace landscape
} // namespace cc

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

#pragma once

#include <list>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"

namespace cc {
namespace gfx {
class Device;
class Texture;
class Sampler;
} // namespace gfx

namespace landscape {

class LandscapeAsset;

/**
 * One LRU cache for paired height (RG8) and splat (R16UI) texture arrays.
 * A node has one request, readiness state and array layer for both textures.
 * Both uploads finish before residency is published; eviction replaces both.
 * Sector roots are loaded during init and never enter the eviction LRU.
 * They remain resident until destroy, providing a real terrain fallback.
 */
class TilePagePool {
public:
    TilePagePool();
    ~TilePagePool();

    bool init(gfx::Device *device, LandscapeAsset *asset, uint32_t layerCount);
    void destroy();
    inline bool valid() const { return _heightArray != nullptr && _splatArray != nullptr; }

    // Marks the start of a frame: clears the "in use this frame" set that
    // protects visible tiles from LRU eviction.
    void beginFrame();
    // Non-blocking: returns the tile's resident layer (touched for LRU) or -1
    // while it streams in. Requests a decode from the LandscapeAsset on first
    // miss. Callers use a resident ancestor tile as the fallback for a -1.
    int query(uint32_t level, uint32_t x, uint32_t z);
    // Returns a resident tile's layer without triggering a load, or -1 if not
    // resident. Touches LRU + marks in-use so a fallback ancestor is protected.
    int peekResident(uint64_t key);
    // Main thread, once per frame: consumes up to `maxUploads` decoded tiles
    // from the LandscapeAsset and uploads them to the GPU (LRU-evicting when
    // full).
    void update(uint32_t maxUploads);

    inline gfx::Texture *heightArray() const { return _heightArray; }
    inline gfx::Texture *splatArray() const { return _splatArray; }
    inline gfx::Sampler *heightSampler() const { return _heightSampler; }
    inline gfx::Sampler *splatSampler() const { return _splatSampler; }
    inline uint32_t layerCount() const { return _layerCount; }

private:
    void uploadLayer(gfx::Texture *array, uint32_t layer, const uint8_t *data) const;
    void touchLRU(uint64_t key);
    int acquireLayer(); // free layer, else evict LRU non-in-use; -1 if none

    gfx::Device *_device{nullptr};     // weak; owned by Root
    IntrusivePtr<LandscapeAsset> _asset;
    IntrusivePtr<gfx::Texture> _heightArray;
    IntrusivePtr<gfx::Texture> _splatArray;
    gfx::Sampler *_heightSampler{nullptr}; // cached by device
    gfx::Sampler *_splatSampler{nullptr};
    uint32_t _tileRes{129};
    uint32_t _layerCount{0};

    ccstd::unordered_map<uint64_t, uint32_t> _resident; // node key -> layer
    std::list<uint64_t> _lru;                           // front = LRU (oldest), back = MRU
    ccstd::unordered_map<uint64_t, std::list<uint64_t>::iterator> _lruIter;
    ccstd::vector<uint32_t> _freeLayers;
    ccstd::unordered_set<uint64_t> _inUse;   // requested this frame (evict-protected)
    bool _warnedFull{false};
};

} // namespace landscape
} // namespace cc

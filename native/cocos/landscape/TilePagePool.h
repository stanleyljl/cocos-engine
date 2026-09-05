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
#include "base/std/container/string.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"

namespace cc {
namespace gfx {
class Device;
class Texture;
class Sampler;
enum class Format : uint32_t; // opaque; full definition only needed in the .cpp
} // namespace gfx

namespace landscape {

class LandscapeAsset;

/**
 * Fixed-capacity LRU page cache for per-Node tiles, backed by one TEX2D_ARRAY
 * texture. Active height tiles use RG8; R16UI remains available for raw tile
 * data in the material/VT pipeline. Each resident tile occupies one layer,
 * selected by per-instance shader attributes.
 *
 * query() is non-blocking: a resident tile returns its layer and touches LRU;
 * a missing tile requests asynchronous decoding and returns -1 so the caller
 * can choose a fallback. update() runs on the main
 * thread each frame and uploads a budgeted number of decoded tiles to the GPU,
 * evicting the least-recently-used tile (never one used this frame) when the
 * pool is full. Layer 0 is the flat (all-zero) fallback.
 */
class TilePagePool {
public:
    static constexpr int FLAT_LAYER = 0;

    TilePagePool();
    ~TilePagePool();

    // `format` is the GPU pixel format of the tiles (bytes/texel derived from it).
    bool init(gfx::Device *device, gfx::Format format, uint32_t tileRes, uint32_t layerCount);
    void setAsset(LandscapeAsset *asset);
    void destroy();
    inline bool valid() const { return _array != nullptr; }

    // Marks the start of a frame: clears the "in use this frame" set that
    // protects visible tiles from LRU eviction.
    void beginFrame();
    // Non-blocking: returns the tile's resident layer (touched for LRU) or -1
    // while it streams in. Requests a decode from the LandscapeAsset on first
    // miss. Callers use a resident ancestor tile as the fallback for a -1.
    int query(uint64_t key, const ccstd::string &tilePath);
    // Returns a resident tile's layer without triggering a load, or -1 if not
    // resident. Touches LRU + marks in-use so a fallback ancestor is protected.
    int peekResident(uint64_t key);
    // Main thread, once per frame: consumes up to `maxUploads` decoded tiles
    // from the LandscapeAsset and uploads them to the GPU (LRU-evicting when
    // full).
    void update(uint32_t maxUploads);

    inline gfx::Texture *array() const { return _array; }
    inline gfx::Sampler *sampler() const { return _sampler; }
    inline uint32_t layerCount() const { return _layerCount; }

private:
    void uploadZeroLayer(uint32_t layer) const;
    void uploadLayer(uint32_t layer, const uint8_t *data) const;
    void touchLRU(uint64_t key);
    int acquireLayer(); // free layer, else evict LRU non-in-use; -1 if none

    gfx::Device *_device{nullptr};     // weak; owned by Root
    IntrusivePtr<LandscapeAsset> _asset;
    IntrusivePtr<gfx::Texture> _array; // TEX2D_ARRAY of `_format`
    gfx::Sampler *_sampler{nullptr};   // weak; cached by device
    gfx::Format _format{};             // tile pixel format (set in init)
    uint32_t _bytesPerTexel{0};        // derived from _format
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

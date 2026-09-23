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

#include <array>
#include <list>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/VTPaging.h"
#include "math/Vec4.h"

namespace cc {
namespace gfx {
class Device;
class Texture;
class Sampler;
} // namespace gfx

namespace landscape {

class LandscapeAsset;

/**
 * One LRU cache for height (RG8), splat (R16UI) and normal XZ (RG8) arrays.
 * A node has one request, readiness state and array layer for all textures.
 * All uploads finish before residency is published; eviction replaces all.
 * Sector roots are loaded during init and never enter the eviction LRU.
 * They remain resident until destroy, providing a real terrain fallback.
 */
class TilePagePool {
public:
    TilePagePool();
    ~TilePagePool();

    bool init(gfx::Device *device, LandscapeAsset *asset, uint32_t layerCount);
    void destroy();
    inline bool valid() const { return _heightArray != nullptr && _splatArray != nullptr && _normalArray != nullptr; }

    // Marks the start of a frame: clears the "in use this frame" set that
    // protects visible tiles from LRU eviction. Synchronous warmup collects
    // misses without queuing workers, then loads them via loadRequestedTiles().
    void beginFrame(bool synchronous = false);
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
    // Initial warmup only: decode/upload missing protected tiles one at a time.
    // Fails immediately on insufficient capacity or a source decode error.
    bool loadRequestedTiles();
    // Exact source requests must be resident; ancestor fallbacks do not count.
    bool requestsReady() const;
    // Changes when async work completes (including failed/dropped loads).
    uint64_t updateRevision() const { return _updateRevision; }

    inline gfx::Texture *heightArray() const { return _heightArray; }
    inline gfx::Texture *splatArray() const { return _splatArray; }
    inline gfx::Texture *normalArray() const { return _normalArray; }
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
    IntrusivePtr<gfx::Texture> _normalArray;
    gfx::Sampler *_heightSampler{nullptr}; // cached by device
    gfx::Sampler *_splatSampler{nullptr};
    uint32_t _tileRes{129};
    uint32_t _layerCount{0};

    ccstd::unordered_map<uint64_t, uint32_t> _resident; // node key -> layer
    std::list<uint64_t> _lru;                           // front = LRU (oldest), back = MRU
    ccstd::unordered_map<uint64_t, std::list<uint64_t>::iterator> _lruIter;
    ccstd::vector<uint32_t> _freeLayers;
    ccstd::unordered_set<uint64_t> _inUse;   // requested this frame (evict-protected)
    ccstd::unordered_set<uint64_t> _missingRequests; // exact query() misses, excluding resident fallbacks
    bool _warnedFull{false};
    bool _synchronous{false};
    uint64_t _updateRevision{0};
};

// Resolves source coordinates and resident fallbacks. The pool owns storage;
// this resolver only caches lookups within one protected residency interval.
// Geometry, splat composition and normal baking all use the same tile identity.
class TilePageResolver {
public:
    struct Tile {
        uint32_t level{0};
        uint32_t x{0};
        uint32_t z{0};
        int layer{-1};
    };
    TilePageResolver(TilePagePool &pool, const LandscapeData &data);

    // Reset source protection and the lookup cache together, before resolving
    // any geometry or VT inputs. Cached lookups do not re-protect their tiles.
    void beginFrame(bool synchronous = false);
    void protectGeometrySources(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
    // Clear after uploads change residency, so finer sources can be discovered.
    void invalidate() { _cache.clear(); }
    Tile resolve(const QuadNode &node);
    Tile normalParent(const QuadNode &node, const Tile &tile);
    Vec4 params(const Tile &tile) const;
    VTPageInputs resolvePage(const VTPageAddress &page, bool bakeNormals);

private:
    uint32_t sourceLevelForWorldSize(float size) const;
    Tile resolveSplat(const VTPageAddress &page);
    std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> resolveNormals(const VTPageAddress &page, bool bakeNormals);
    uint32_t resolveNormalNeighborhood(const VTPageAddress &page, float pageSize, uint32_t level,
                                      std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> &sources);
    TilePagePool &_pool; // owner destroys the resolver before the pool
    LandscapeData _data;
    uint32_t _vtRootLevel;
    ccstd::unordered_map<uint64_t, Tile> _cache;
};

} // namespace landscape
} // namespace cc

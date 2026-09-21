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
#include "landscape/VTPaging.h"

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
    _missingRequests.clear();
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
    _missingRequests.insert(key);
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

bool TilePagePool::requestsReady() const {
    return std::all_of(_missingRequests.begin(), _missingRequests.end(), [this](uint64_t key) {
        return _resident.count(key) != 0;
    });
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
    _missingRequests.clear();
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

TilePageResolver::TilePageResolver(TilePagePool &pool, const LandscapeData &data)
: _pool(pool), _data(data), _vtRootLevel(vtRootLevel(data.sectorSize)) {}

void TilePageResolver::beginFrame() {
    _pool.beginFrame();
    invalidate();
}

void TilePageResolver::protectGeometrySources(const ccstd::vector<QuadNode> &geometryNodes,
                                             const ccstd::vector<QuadNode> &surfaceNodes) {
    for (const auto &node : geometryNodes) resolve(node);
    // Shadow vertices need heights only. Parent normals are a color-pass input.
    for (const auto &node : surfaceNodes) {
        normalParent(node, resolve(node));
    }
}

Vec4 TilePageResolver::params(const Tile &tile) const {
    const float size = computeNodeSize(_data.sectorSize, _data.maxLevel, tile.level);
    return Vec4{tile.x * size - _data.worldWidth() * 0.5F,
                tile.z * size - _data.worldDepth() * 0.5F, size, static_cast<float>(tile.layer)};
}

uint32_t TilePageResolver::sourceLevelForWorldSize(float size) const {
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxLevel && computeNodeSize(_data.sectorSize, _data.maxLevel, level) < size) ++level;
    return level;
}

TilePageResolver::Tile TilePageResolver::resolveSplat(const VTPageAddress &page) {
    // A material page can be finer than every height/splat tile. Choose the
    // containing source independently of the selected geometry node's LOD.
    const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
    const uint32_t level = sourceLevelForWorldSize(size);
    const float sourceSize = computeNodeSize(_data.sectorSize, _data.maxLevel, level);
    return resolve(QuadNode{level, static_cast<uint32_t>((page.x + 0.5F) * size / sourceSize),
                            static_cast<uint32_t>((page.z + 0.5F) * size / sourceSize)});
}

std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> TilePageResolver::resolveNormals(const VTPageAddress &page, bool bakeNormals) {
    std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> sources;
    if (!bakeNormals) {
        sources.fill(params(resolveSplat(page)));
        return sources;
    }
    // Match normal spacing to VT texels, independently of geometry LOD. A
    // 129-sample tile covers half a 256-texel page. The outer ring supplies
    // real neighboring normals for gutters instead of clamping at page edges.
    const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
    const float target = size * std::max(0.5F,
        static_cast<float>(_data.tileResolution - 1U) / config::VT_PAGE_INTERIOR);
    uint32_t level = sourceLevelForWorldSize(target);
    for (;;) {
        const uint32_t residentLevel = resolveNormalNeighborhood(page, size, level, sources);
        // Publish one resolution for the entire page. Fine sources remain
        // requested; when all arrive, acquirePage invalidates this cached page.
        if (residentLevel == level) return sources;
        level = residentLevel;
    }
}

uint32_t TilePageResolver::resolveNormalNeighborhood(
    const VTPageAddress &page, float pageSize, uint32_t level,
    std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> &sources) {
    constexpr uint32_t SIDE = 4;
    static_assert(SIDE * SIDE == config::VT_NORMAL_SOURCE_COUNT, "Normal source table is a 4x4 neighborhood");
    const float sourceSize = computeNodeSize(_data.sectorSize, _data.maxLevel, level);
    const uint32_t countX = _data.sectorsX << (_data.maxLevel - level);
    const uint32_t countZ = _data.sectorsZ << (_data.maxLevel - level);
    uint32_t coarsestResidentLevel = level;
    for (uint32_t row = 0; row < SIDE; ++row) {
        for (uint32_t column = 0; column < SIDE; ++column) {
            // Probe centers at -1/4, 1/4, 3/4, 5/4 of the page on each axis:
            // two interior sources and one neighbor beyond each edge.
            const float x = (page.x - 0.25F + column * 0.5F) * pageSize;
            const float z = (page.z - 0.25F + row * 0.5F) * pageSize;
            const uint32_t tileX = static_cast<uint32_t>(std::clamp(std::floor(x / sourceSize), 0.0F, static_cast<float>(countX - 1U)));
            const uint32_t tileZ = static_cast<uint32_t>(std::clamp(std::floor(z / sourceSize), 0.0F, static_cast<float>(countZ - 1U)));
            const auto tile = resolve(QuadNode{level, tileX, tileZ});
            sources[row * SIDE + column] = params(tile);
            coarsestResidentLevel = std::max(coarsestResidentLevel, tile.level);
        }
    }
    return coarsestResidentLevel;
}

TilePageResolver::Tile TilePageResolver::normalParent(const QuadNode &node, const Tile &tile) {
    // Finer geometry already shares the finest source normal map. A streaming
    // fallback likewise must not morph towards an extra-coarse level again.
    if (node.level != tile.level || node.level >= _data.maxLevel) return tile;
    return resolve(QuadNode{node.level + 1U, node.ix >> 1U, node.iz >> 1U});
}

TilePageResolver::Tile TilePageResolver::resolve(const QuadNode &node) {
    Tile tile;
    tile.level = std::max(node.level, _data.minTileLevel);
    const uint32_t shift = tile.level - node.level;
    tile.x = node.ix >> shift;
    tile.z = node.iz >> shift;
    const uint64_t key = makeNodeKey(tile.level, tile.x, tile.z);
    const auto cached = _cache.find(key);
    if (cached != _cache.end()) return cached->second;
    // Only the desired tile starts an asynchronous request. Ancestors are
    // queried without loading and remain protected while used as fallbacks.
    tile.layer = _pool.query(tile.level, tile.x, tile.z);
    while (tile.layer < 0 && tile.level < _data.maxLevel) {
        ++tile.level;
        tile.x >>= 1U;
        tile.z >>= 1U;
        tile.layer = _pool.peekResident(makeNodeKey(tile.level, tile.x, tile.z));
    }
    _cache.emplace(key, tile);
    return tile; // initialization guarantees a resident root for every sector
}

VTPageInputs TilePageResolver::resolvePage(const VTPageAddress &page, bool bakeNormals) {
    const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
    const Vec4 region{page.x * size - _data.worldWidth() * 0.5F,
                      page.z * size - _data.worldDepth() * 0.5F, size, 0.0F};
    return {region, params(resolveSplat(page)), resolveNormals(page, bakeNormals)};
}

} // namespace landscape
} // namespace cc

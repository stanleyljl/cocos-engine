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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "base/std/container/string.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "base/RefCounted.h"
#include "landscape/LandscapeConfig.h"

namespace cc {
namespace gfx {
enum class Format : uint32_t;
}
namespace landscape {

class LandscapeAsset : public RefCounted {
public:
    struct MaterialLayer {
        uint32_t id{0};
        ccstd::string name;
        ccstd::string albedoHeight;
        ccstd::string normalRoughnessAO;
        float detailHeightScale{1.0F};
        float detailHeightBias{0.0F};
        float pixelsPerMeter{128.0F}; // source texture pixels per landscape-local meter
    };

    struct DecalLayer {
        ccstd::string albedoMask;
        ccstd::string normalRoughnessAO;
        ccstd::string height;
        float heightScale{0.0F};
        bool modulateColor{false}; // preserve underlying terrain color for imprints
        int32_t detailMaterial0{-1}; // optional tiled material-library pair, RVT-only
        int32_t detailMaterial1{-1}; // normalRoughnessAO.B stores pair blend weight
    };
    // Optional slope-based material override, authored by the terrain asset.
    struct CliffMaterial {
        int32_t layer{-1}; // negative retains painted splat materials
        float maxNormalY{0.0F};
        float globalColorInfluence{1.0F}; // multiplier of the terrain-wide strength
    };
    struct Decal {
        float x{0}; // minimum corner in landscape-local XZ
        float z{0};
        float size{1};
        uint32_t layer{0};
        float nearDistance{12};
        float farDistance{40};
    };

    struct TileData {
        uint64_t key{0};
        ccstd::vector<uint8_t> height;
        ccstd::vector<uint8_t> splat;
        ccstd::vector<uint8_t> normal; // linear RG8 terrain-local XZ; shader reconstructs +Y
    };

    LandscapeAsset();
    ~LandscapeAsset() override;

    bool load(const ccstd::string &dataDir);
    // Synchronously loads sector root height/splat/normal tiles before rendering.
    bool loadRootTile(uint32_t x, uint32_t z, TileData &tile) const;
    // Call on the main thread: resolves absolute file paths before scheduling
    // background I/O, bypassing FileUtils' unsynchronized relative-path cache.
    // Only complete height/splat/normal sets are available through takeReadyTile().
    bool requestTile(uint32_t level, uint32_t x, uint32_t z);
    bool takeReadyTile(TileData &tile);
    bool takeFailedTile(uint64_t &key);
    bool getHeightRange(uint32_t level, uint32_t globalX, uint32_t globalZ,
                        float &minY, float &maxY) const;
    float getSurfaceStretch(uint32_t level, uint32_t globalX, uint32_t globalZ) const;
    // Decodes uint16 height/splat PNGs as RG8/R16UI, or RGB PNGs as linear RGB8.
    static bool loadTile(const ccstd::string &path, gfx::Format format, uint32_t tileResolution,
                         ccstd::vector<uint8_t> &data);

    const ccstd::vector<DecalLayer> &decalLayers() const { return _decalLayers; }
    const ccstd::vector<Decal> &decals() const { return _decals; }
    uint32_t decalResolution() const { return _decalResolution; }
    const LandscapeData &data() const { return _data; }
    const ccstd::string &dataDir() const { return _dataDir; }
    bool valid() const { return _data.valid(); }
    const ccstd::vector<MaterialLayer> &materialLayers() const { return _materialLayers; }
    uint32_t materialResolution() const { return _materialResolution; }
    const CliffMaterial &cliffMaterial() const { return _cliffMaterial; }

private:
    struct Manifest;
    struct AsyncState {
        std::mutex mutex;
        std::deque<TileData> ready;
        std::deque<uint64_t> failed;
        std::atomic<bool> cancelled{false};
    };

    struct HeightRange {
        float minY{0.0F};
        float maxY{0.0F};
        float surfaceStretch{1.0F}; // maximum local stretch, propagated from finer nodes
    };

    void resetAsyncState();
    static bool decodeTileSet(const ccstd::string &heightPath, const ccstd::string &splatPath, const ccstd::string &normalPath,
                               uint32_t resolution, TileData &tile);
    ccstd::string resolveFile(const ccstd::string &logicalPath) const;

    ccstd::string _dataDir;
    ccstd::unordered_map<ccstd::string, ccstd::string> _files;
    LandscapeData _data;
    ccstd::vector<MaterialLayer> _materialLayers;
    uint32_t _materialResolution{0};
    CliffMaterial _cliffMaterial;
    ccstd::vector<DecalLayer> _decalLayers;
    ccstd::vector<Decal> _decals;
    uint32_t _decalResolution{1};
    ccstd::vector<size_t> _levelOffsets;
    ccstd::vector<HeightRange> _heightRanges;
    size_t _nodesPerSector{0U};
    ccstd::unordered_set<uint64_t> _pendingTiles;
    std::shared_ptr<AsyncState> _async;
};

} // namespace landscape
} // namespace cc

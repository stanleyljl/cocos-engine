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
    struct TileData {
        uint64_t key{0};
        ccstd::vector<uint8_t> data;
    };

    LandscapeAsset();
    ~LandscapeAsset() override;

    bool load(const ccstd::string &dataDir);
    // Requests a tile without blocking the caller. The decoded tile is made
    // available through takeReadyTile() on the main thread.
    bool requestTile(uint64_t key, const ccstd::string &tilePath,
                     gfx::Format format, uint32_t tileResolution);
    bool takeReadyTile(TileData &tile);
    bool takeFailedTile(uint64_t &key);
    bool getHeightRange(uint32_t level, uint32_t globalX, uint32_t globalZ,
                        float &minY, float &maxY) const;
    // Decodes uint16 PNG samples as RG8 heights or raw R16UI tile data.
    static bool loadTile(const ccstd::string &path, gfx::Format format, uint32_t tileResolution,
                         ccstd::vector<uint8_t> &data);

    const LandscapeData &data() const { return _data; }
    const ccstd::string &dataDir() const { return _dataDir; }
    bool valid() const { return _data.valid(); }

private:
    struct AsyncState {
        std::mutex mutex;
        std::deque<TileData> ready;
        std::deque<uint64_t> failed;
        std::atomic<bool> cancelled{false};
    };

    struct HeightRange {
        float minY{0.0F};
        float maxY{0.0F};
    };

    void resetAsyncState();
    size_t rangeOffset(uint32_t level, uint32_t globalX, uint32_t globalZ) const;

    ccstd::string _dataDir;
    LandscapeData _data;
    ccstd::vector<size_t> _levelOffsets;
    ccstd::vector<HeightRange> _heightRanges;
    size_t _nodesPerSector{0U};
    ccstd::unordered_set<uint64_t> _pendingTiles;
    std::shared_ptr<AsyncState> _async;
};

} // namespace landscape
} // namespace cc

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

#include <cstdint>

namespace cc {
namespace landscape {
namespace config {

constexpr int       VERTS_PER_NODE_SIDE = 17;
constexpr float     VISIBILITY_DISTANCE_IN_SECTORS = 6.0F;
constexpr float     LOD_DISTANCE_RATIO = 2.0F;
constexpr float     MORPH_START_RATIO = 0.70F;
constexpr uint32_t  MAX_LOD_LEVELS = 8;
constexpr uint8_t   ALL_QUADRANTS = 0x0FU;

// Active height-page cache configuration.
constexpr uint32_t PAGE_POOL_LAYERS = 1024;
constexpr uint32_t PAGE_UPLOAD_BUDGET = 8;
// Retained for the material library and virtual texture implementation.
constexpr uint32_t MATERIAL_LIBRARY_MAX = 32;

constexpr uint32_t VT_ATLAS_SIZE = 4096;
constexpr uint32_t VT_PAGE_RES = 128;
constexpr uint32_t VT_PAGE_BORDER = 4;
constexpr uint32_t VT_PAGE_INTERIOR = VT_PAGE_RES - 2 * VT_PAGE_BORDER;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
static_assert(VT_PAGE_RES > 2 * VT_PAGE_BORDER && VT_ATLAS_SIZE % VT_PAGE_RES == 0,
              "VT pages must fit the atlas and leave a non-empty interior");

} // namespace config

struct LandscapeData {
    uint32_t sectorsX{0};
    uint32_t sectorsZ{0};
    uint32_t maxLevel{0};
    uint32_t minTileLevel{0};
    uint32_t tileResolution{0};
    float sectorSize{0.0F};
    float heightScale{0.0F};
    float heightBias{0.0F};

    float worldWidth() const { return sectorSize * static_cast<float>(sectorsX); }
    float worldDepth() const { return sectorSize * static_cast<float>(sectorsZ); }
    float minHeight() const { return heightBias; }
    float maxHeight() const { return heightBias + heightScale; }
    bool valid() const {
        return sectorsX > 0U && sectorsZ > 0U && maxLevel < config::MAX_LOD_LEVELS &&
               minTileLevel <= maxLevel && tileResolution > 1U && sectorSize > 0.0F &&
               heightScale > 0.0F;
    }
};

struct QuadNode {
    uint32_t level{0};
    uint32_t ix{0};
    uint32_t iz{0};
    float minY{0.0F};
    float maxY{0.0F};
    uint8_t quadrantMask{config::ALL_QUADRANTS};
};

constexpr uint32_t NODE_KEY_COORD_BITS = 28U;
constexpr uint64_t NODE_KEY_COORD_MASK = (1ULL << NODE_KEY_COORD_BITS) - 1ULL;
constexpr uint32_t NODE_KEY_LEVEL_SHIFT = NODE_KEY_COORD_BITS * 2U;

inline uint64_t makeNodeKey(uint32_t level, uint32_t ix, uint32_t iz) {
    return (static_cast<uint64_t>(level) << NODE_KEY_LEVEL_SHIFT) |
           ((static_cast<uint64_t>(ix) & NODE_KEY_COORD_MASK) << NODE_KEY_COORD_BITS) |
           (static_cast<uint64_t>(iz) & NODE_KEY_COORD_MASK);
}

inline uint64_t makeNodeKey(const QuadNode &node) {
    return makeNodeKey(node.level, node.ix, node.iz);
}

} // namespace landscape
} // namespace cc

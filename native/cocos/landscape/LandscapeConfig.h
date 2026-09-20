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

#include <cstddef>
#include <cstdint>
#include <limits>

#include "base/std/container/vector.h"

// Landscape diagnostics are independent of the engine's global debug mode.
// Override with CC_LANDSCAPE_DEBUG=0 to remove informational logs and LOD stats.
// Errors, operational warnings and visual debug controls remain available.
#ifndef CC_LANDSCAPE_DEBUG
    #define CC_LANDSCAPE_DEBUG 1
#endif

namespace cc {
namespace landscape {
namespace config {

constexpr int       VERTS_PER_NODE_SIDE = 17;
constexpr float     LOD_DISTANCE_RATIO = 2.0F;
constexpr float     MORPH_START_RATIO = 0.70F;
// L0-L8: maximum sector size = 4096 m at 1 m finest-grid spacing
// (16 cells per node side * 2^8). Keep the shader/importer limits in sync.
constexpr uint32_t  MAX_LOD_LEVELS = 9;
constexpr uint8_t   ALL_QUADRANTS = 0x0FU;

// Active height-page cache configuration.
constexpr uint32_t PAGE_POOL_LAYERS = 1024;
constexpr uint32_t PAGE_UPLOAD_BUDGET = 8;
// Retained for the material library and virtual texture implementation.
constexpr uint32_t MATERIAL_LIBRARY_MAX = 32;
constexpr uint32_t DECAL_LIBRARY_MAX = 8;
constexpr uint32_t DECAL_INSTANCE_MAX = 128;

// 256 usable texels + filtering gutters. Keep 16 x 16 physical slots.
constexpr uint32_t VT_PAGE_INTERIOR = 256;
constexpr uint32_t VT_NORMAL_SOURCE_COUNT = 16; // 2x2 interior sources plus a one-source gutter ring.
constexpr uint32_t VT_PAGE_BORDER = 4;
constexpr uint32_t VT_MIP_LEVELS = 3; // interiors 256/128/64, borders 4/2/1
constexpr uint32_t VT_PAGE_RES = VT_PAGE_INTERIOR + 2 * VT_PAGE_BORDER;
constexpr uint32_t VT_ATLAS_SIZE = VT_PAGE_RES * 16;
constexpr uint32_t VT_PAGE_UPDATE_BUDGET = 8;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
static_assert(VT_PAGE_RES % (1U << (VT_MIP_LEVELS - 1)) == 0 &&
              VT_PAGE_BORDER % (1U << (VT_MIP_LEVELS - 1)) == 0,
              "Every VT mip requires aligned slots and whole-texel gutters");
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
    bool valid() const;
};


// Fills per-level offsets (L0 first) and returns the total node count per sector.
// Invalid maxLevel values clear the offsets and return zero.
size_t computeSectorNodeLayout(uint32_t maxLevel, ccstd::vector<size_t> &levelOffsets);
// Invalid levels return zero. L0 is finest; maxLevel is the sector root.
uint32_t computeNodesPerSide(uint32_t maxLevel, uint32_t level);
float computeNodeSize(float sectorSize, uint32_t maxLevel, uint32_t level);

constexpr size_t INVALID_NODE_INDEX = std::numeric_limits<size_t>::max();
// Offsets/count must come from computeSectorNodeLayout(data.maxLevel).
// The layout is sector-major, then level-major, then row-major (Z, X).
// Invalid coordinates/levels return INVALID_NODE_INDEX.
size_t nodeRangeIndex(const LandscapeData &data, const ccstd::vector<size_t> &levelOffsets,
                      size_t nodesPerSector, uint32_t sectorX, uint32_t sectorZ,
                      uint32_t level, uint32_t localX, uint32_t localZ);
size_t globalNodeRangeIndex(const LandscapeData &data, const ccstd::vector<size_t> &levelOffsets,
                            size_t nodesPerSector, uint32_t level, uint32_t globalX, uint32_t globalZ);

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

uint64_t makeNodeKey(uint32_t level, uint32_t ix, uint32_t iz);
uint64_t makeNodeKey(const QuadNode &node);
uint64_t makeQuadrantKey(const QuadNode &node, uint32_t quadrant);

} // namespace landscape
} // namespace cc

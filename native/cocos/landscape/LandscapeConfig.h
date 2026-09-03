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

// These values intentionally match cdlod_opengl/cdlod.h.
constexpr uint32_t DEMO_HM_SIZE = 2048;
constexpr float SECTOR_SIZE = 4096.0F;
constexpr uint32_t DEMO_SECTORS = 3;
constexpr float WORLD_SIZE = SECTOR_SIZE * static_cast<float>(DEMO_SECTORS);
constexpr float HEIGHT_SCALE = 500.0F;
constexpr float HEIGHT_BIAS = 0.0F;
constexpr uint32_t DEMO_LOD_COUNT = 6;
constexpr uint32_t MAX_LEVEL = DEMO_LOD_COUNT - 1U;
constexpr int VERTS_PER_GRID_SIDE = 17;
constexpr int GRIDS_PER_NODE_SIDE = 1;
constexpr int GRIDS_PER_NODE = GRIDS_PER_NODE_SIDE * GRIDS_PER_NODE_SIDE;

constexpr float VIS_SAFETY = 1.05F;
constexpr float MORPH_RATIO = 0.66F;
constexpr uint32_t MAX_LOD_LEVELS = 8;
constexpr float TERRAIN_MIN_Y = HEIGHT_BIAS;
constexpr float TERRAIN_MAX_Y = HEIGHT_BIAS + HEIGHT_SCALE;

// Constants retained for the dormant page/material/VT files.
constexpr uint32_t TILE_RESOLUTION = 129;
constexpr uint32_t PAGE_POOL_LAYERS = 512;
constexpr uint32_t PAGE_UPLOAD_BUDGET = 6;
constexpr uint32_t MATERIAL_LIBRARY_MAX = 32;
constexpr uint32_t VT_ATLAS_SIZE = 4096;
constexpr uint32_t VT_PAGE_RES = 128;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
constexpr uint32_t VT_COMPOSE_LAYER = 1U << 19;

} // namespace config

struct QuadNode {
    uint32_t level{0};
    uint32_t ix{0};
    uint32_t iz{0};
    float minY{config::TERRAIN_MIN_Y};
    float maxY{config::TERRAIN_MAX_Y};
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

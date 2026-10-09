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

// Landscape diagnostics are independent of the engine's global debug mode.
// Override with CC_LANDSCAPE_DEBUG=0 to remove informational logs and LOD stats.
// Errors, operational warnings and visual debug controls remain available.
#ifndef CC_LANDSCAPE_DEBUG
    #define CC_LANDSCAPE_DEBUG 1
#endif

namespace cc {
namespace landscape {
namespace config {

constexpr int VERTS_PER_NODE_SIDE = 17;
constexpr float LOD_DISTANCE_RATIO = 2.0F;
constexpr float MORPH_START_RATIO = 0.70F;
constexpr uint32_t SOURCE_TILE_RESOLUTION = 129;
// L0-L8: maximum sector size = 4096 m at 1 m finest-grid spacing
// (16 cells per node side * 2^8). Keep the shader/importer limits in sync.
constexpr uint32_t MAX_LOD_LEVELS = 9;
constexpr uint8_t ALL_QUADRANTS = 0x0FU;

// Active height-page cache configuration.
constexpr uint32_t PAGE_POOL_LAYERS = 1024;
constexpr uint32_t PAGE_UPLOAD_BUDGET = 8;
// Material and decal capacities shared by asset validation and shaders.
constexpr uint32_t MATERIAL_LIBRARY_MAX = 32;
constexpr uint32_t DECAL_LIBRARY_MAX = 32;
constexpr uint32_t DECAL_INSTANCE_MAX = 512;
// The compose block has one vec4 per material/instance/layer plus eight parameters.
static_assert((MATERIAL_LIBRARY_MAX + DECAL_INSTANCE_MAX + DECAL_LIBRARY_MAX + 8) * 16 <= 16384,
              "VT compose constants must fit a 16 KB uniform block");
static_assert(DECAL_INSTANCE_MAX % 2 == 0, "Decal index pairs must fill RGBA8 texels");

// 256 usable texels per meter at the finest page. Keep 16 x 16 physical slots.
constexpr uint32_t VT_PAGE_INTERIOR = 256;
constexpr uint32_t VT_NORMAL_SOURCE_COUNT = 16; // 2x2 interior sources plus a one-source gutter ring.
constexpr uint32_t VT_PAGE_BORDER = 8;
constexpr uint32_t VT_MIP_LEVELS = 3; // interiors 256/128/64, borders 8/4/2
constexpr uint32_t VT_MAX_ANISOTROPY = 4;
// Base-level texel width of the coarsest mip. Reserve this much filter support
// when constraining a fragment's footprint to its physical atlas slot.
constexpr uint32_t VT_FILTER_MARGIN = 1U << (VT_MIP_LEVELS - 1U);
constexpr uint32_t VT_PAGE_RES = VT_PAGE_INTERIOR + 2 * VT_PAGE_BORDER;
constexpr uint32_t VT_ATLAS_SIZE = VT_PAGE_RES * 16;
// Shared by all resource transactions and camera passes in one frame.
constexpr uint32_t VT_PAGE_UPDATE_BUDGET = 8;
constexpr uint32_t STREAMING_STEP_BUDGET = 8;
constexpr uint32_t STREAMING_ADMISSION_BUDGET = 16;
constexpr double   STREAMING_TIME_BUDGET_MS = 2.0;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
static_assert(VT_PAGE_RES % (1U << (VT_MIP_LEVELS - 1)) == 0 &&
                  VT_PAGE_BORDER % (1U << (VT_MIP_LEVELS - 1)) == 0,
              "Every VT mip requires aligned slots and whole-texel gutters");
static_assert(VT_PAGE_RES > 2 * VT_PAGE_BORDER && VT_ATLAS_SIZE % VT_PAGE_RES == 0,
              "VT pages must fit the atlas and leave a non-empty interior");
static_assert(VT_PAGE_BORDER > VT_FILTER_MARGIN, "VT gutters must leave room for anisotropic footprints");

} // namespace config

} // namespace landscape
} // namespace cc

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
// Override with CC_LANDSCAPE_DEBUG=0 to remove informational logs, LOD stats and morph diagnostics.
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
// Slope threshold in degrees from horizontal, evaluated on the fixed sector source.
constexpr float CLIFF_ANGLE = 60.0F;
static_assert(CLIFF_ANGLE >= 0.0F && CLIFF_ANGLE <= 90.0F, "Cliff angle must be between 0 and 90 degrees");
constexpr uint32_t SOURCE_TILE_RESOLUTION = 129;
// Normal interiors share the height grid; real neighboring samples cover VT gutters.
constexpr uint32_t SOURCE_NORMAL_BORDER = 8;
constexpr uint32_t SOURCE_NORMAL_RESOLUTION = SOURCE_TILE_RESOLUTION + 2 * SOURCE_NORMAL_BORDER;
// L0-L8: maximum sector size = 4096 m at 1 m finest-grid spacing
// (16 cells per node side * 2^8). Keep the shader/importer limits in sync.
constexpr uint32_t MAX_LOD_LEVELS = 9;
// Total sectors (X * Z), not the count along either axis. Keep creation tools,
// the importer and the TypeScript height-layout validator in sync.
constexpr uint32_t MAX_SECTOR_COUNT = 64;
constexpr uint8_t ALL_QUADRANTS = 0x0FU;

// Active height-page cache configuration.
constexpr uint32_t PAGE_POOL_LAYERS = 1024;
constexpr uint32_t PAGE_UPLOAD_BUDGET = 8;
// Material and decal capacities shared by asset validation and shaders.
constexpr uint32_t MATERIAL_LIBRARY_MAX = 32;
constexpr uint32_t DECAL_LIBRARY_MAX = 32;
constexpr uint32_t DECAL_INSTANCE_MAX = 512;
// The compose block has one vec4 per material/instance/layer plus six parameters.
static_assert((MATERIAL_LIBRARY_MAX + DECAL_INSTANCE_MAX + DECAL_LIBRARY_MAX + 6) * 16 <= 16384,
              "VT compose constants must fit a 16 KB uniform block");
static_assert(DECAL_INSTANCE_MAX % 2 == 0, "Decal index pairs must fill RGBA8 texels");

// 256 usable texels per logical unit at the finest page. Up to 16 x 16 physical slots.
constexpr uint32_t VT_PAGE_INTERIOR = 256;
constexpr uint32_t VT_NORMAL_SOURCE_COUNT = 4; // 2x2 interiors; padded sources also cover filtering gutters.
constexpr uint32_t VT_NORMAL_DETAIL_MIN_LEVEL = 6;
constexpr uint32_t VT_PAGE_BORDER = 8;
constexpr uint32_t VT_MIP_LEVELS = 3; // interiors 256/128/64, borders 8/4/2
constexpr uint32_t VT_MAX_ANISOTROPY = 4;
// Base-level texel width of the coarsest mip. Reserve this much filter support
// when constraining a fragment's footprint to its physical atlas slot.
constexpr uint32_t VT_FILTER_MARGIN = 1U << (VT_MIP_LEVELS - 1U);
constexpr uint32_t VT_PAGE_RES = VT_PAGE_INTERIOR + 2 * VT_PAGE_BORDER;
// Maximum atlas dimensions; Renderer selects the actual capacity for the device.
constexpr uint32_t VT_ATLAS_SIZE = VT_PAGE_RES * 16;
// Shared by all resource transactions and camera passes in one frame.
constexpr uint32_t VT_PAGE_UPDATE_BUDGET = 8;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
static_assert(VT_PAGE_COUNT <= 256, "RG8 mappings store an 8-bit physical slot");
static_assert(SOURCE_NORMAL_BORDER * VT_PAGE_INTERIOR >= 2 * (SOURCE_TILE_RESOLUTION - 1) * VT_PAGE_BORDER,
              "Padded normals must cover VT gutters at half-page source precision");
static_assert(MAX_SECTOR_COUNT <= VT_PAGE_COUNT, "VT must hold every sector root");
// Root geometry/splat uses one source per sector; baked root normals use at
// most four, shared with neighboring root pages.
static_assert(MAX_SECTOR_COUNT * 5U <= PAGE_POOL_LAYERS,
              "Source pool must hold root coverage and its composition inputs");
static_assert(VT_PAGE_RES % (1U << (VT_MIP_LEVELS - 1)) == 0 &&
                  VT_PAGE_BORDER % (1U << (VT_MIP_LEVELS - 1)) == 0,
              "Every VT mip requires aligned slots and whole-texel gutters");
static_assert(VT_PAGE_RES > 2 * VT_PAGE_BORDER && VT_ATLAS_SIZE % VT_PAGE_RES == 0,
              "VT pages must fit the atlas and leave a non-empty interior");
static_assert(VT_PAGE_BORDER > VT_FILTER_MARGIN, "VT gutters must leave room for anisotropic footprints");

} // namespace config

} // namespace landscape
} // namespace cc

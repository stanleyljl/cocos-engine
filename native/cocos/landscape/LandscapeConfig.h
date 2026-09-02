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

// Basic landscape defaults. The active renderer currently uses only the plane
// size; the remaining constants keep the dormant landscape helpers buildable.
constexpr float SECTOR_SIZE = 4096.0F;
constexpr uint32_t DEMO_SECTORS = 3;
constexpr float WORLD_SIZE = SECTOR_SIZE * static_cast<float>(DEMO_SECTORS);

// Kept for GridMesh.cpp, which remains available but is intentionally dormant.
constexpr int VERTS_PER_GRID_SIDE = 17;
constexpr int GRIDS_PER_NODE_SIDE = 1;
constexpr int GRIDS_PER_NODE = GRIDS_PER_NODE_SIDE * GRIDS_PER_NODE_SIDE;

// Kept for the dormant material and virtual-texture implementations.
constexpr uint32_t MATERIAL_LIBRARY_MAX = 32;
constexpr uint32_t VT_ATLAS_SIZE = 4096;
constexpr uint32_t VT_PAGE_RES = 128;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
constexpr uint32_t VT_COMPOSE_LAYER = 1U << 19;

} // namespace config
} // namespace landscape
} // namespace cc

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

// 512 usable texels per meter at the finest page. Keep 16 x 16 physical slots.
constexpr uint32_t VT_PAGE_INTERIOR = 512;
constexpr uint32_t VT_NORMAL_SOURCE_COUNT = 16; // 2x2 interior sources plus a one-source gutter ring.
constexpr uint32_t VT_PAGE_BORDER = 8;
constexpr uint32_t VT_MIP_LEVELS = 3; // interiors 512/256/128, borders 8/4/2
constexpr uint32_t VT_MAX_ANISOTROPY = 4;
// Base-level texel width of the coarsest mip. Reserve this much filter support
// when constraining a fragment's footprint to its physical atlas slot.
constexpr uint32_t VT_FILTER_MARGIN = 1U << (VT_MIP_LEVELS - 1U);
constexpr uint32_t VT_PAGE_RES = VT_PAGE_INTERIOR + 2 * VT_PAGE_BORDER;
constexpr uint32_t VT_ATLAS_SIZE = VT_PAGE_RES * 16;
// Four times as many texels per page: retain the previous per-frame pixel budget.
constexpr uint32_t VT_PAGE_UPDATE_BUDGET = 2;
constexpr uint32_t VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
constexpr uint32_t VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;
static_assert(VT_PAGE_RES % (1U << (VT_MIP_LEVELS - 1)) == 0 &&
              VT_PAGE_BORDER % (1U << (VT_MIP_LEVELS - 1)) == 0,
              "Every VT mip requires aligned slots and whole-texel gutters");
static_assert(VT_PAGE_RES > 2 * VT_PAGE_BORDER && VT_ATLAS_SIZE % VT_PAGE_RES == 0,
              "VT pages must fit the atlas and leave a non-empty interior");
static_assert(VT_PAGE_BORDER > VT_FILTER_MARGIN, "VT gutters must leave room for anisotropic footprints");

} // namespace config

// Meters relative to the landscape node's center (translation-only placement).
struct LandscapeLocalXZ {
    double x{0};
    double z{0};
};

// Meters from the landscape's minimum XZ corner, before division into cells.
// A separate type prevents accidentally using center-relative coordinates.
struct LandscapeGridXZ {
    double x{0};
    double z{0};
};

struct LandscapeGridBounds {
    LandscapeGridXZ min;
    LandscapeGridXZ max;
};

// Global geometry/source-tile coordinates at a geometry LOD, NOT a VT level.
struct NodeAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};

    uint64_t key() const;
    static NodeAddress fromKey(uint64_t key);
    // targetLevel must be an ancestor (>= level), within geometry LOD limits.
    NodeAddress ancestor(uint32_t targetLevel) const;
};

// Square in landscape-local meters. Convert to Vec4 only at the shader boundary.
struct LandscapeLocalRegion {
    float x{0};
    float z{0};
    float size{0};
};

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

    // L0 is finest; maxLevel is the sector root. Invalid levels return zero.
    uint32_t nodesPerSectorSide(uint32_t level) const;
    float nodeSize(uint32_t level) const;
    uint32_t nodesX(uint32_t level) const { return sectorsX * nodesPerSectorSide(level); }
    uint32_t nodesZ(uint32_t level) const { return sectorsZ * nodesPerSectorSide(level); }

    LandscapeGridXZ localToGrid(LandscapeLocalXZ local) const;
    LandscapeLocalXZ gridToLocal(LandscapeGridXZ grid) const;
    // Sampling includes the outermost boundary. It belongs to the last cell.
    bool containsGridPoint(LandscapeGridXZ grid) const;
    // Requires valid dimensions, a valid level and finite grid coordinates.
    // Explicitly clamps gutter probes and inclusive outer-edge samples.
    NodeAddress nodeAtGridClamped(uint32_t level, LandscapeGridXZ grid) const;
    LandscapeLocalRegion regionAtGrid(LandscapeGridXZ origin, float size) const;
    LandscapeLocalRegion nodeRegion(NodeAddress address) const;
    NodeAddress nodeInSector(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                             uint32_t localX, uint32_t localZ) const;
};

// Flat storage for node metadata: sectors in row-major (Z, X) order, then
// levels from L0 to the root, then nodes in row-major (Z, X) order per level.
// Owns a snapshot of the dimensions and offsets; callers cannot mix layouts.
class NodeRangeLayout {
public:
    static constexpr size_t INVALID_INDEX = std::numeric_limits<size_t>::max();

    NodeRangeLayout() = default;
    // Zero sectors or an invalid maxLevel produce an empty layout.
    explicit NodeRangeLayout(const LandscapeData &data);

    size_t nodeCount() const { return static_cast<size_t>(_sectorsX) * _sectorsZ * _nodesPerSector; }

    // Both queries return an index into the same flat storage, or INVALID_INDEX.
    // localX/localZ are relative to the given sector at the requested level.
    size_t sectorNodeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                           uint32_t localX, uint32_t localZ) const;
    // globalX/globalZ span all sectors at the requested level.
    size_t globalNodeIndex(uint32_t level, uint32_t globalX, uint32_t globalZ) const;

private:
    struct Level {
        uint32_t nodesPerSide{0};
        size_t offset{0};
    };
    std::array<Level, config::MAX_LOD_LEVELS> _levels{};
    uint32_t _levelCount{0};
    uint32_t _sectorsX{0};
    uint32_t _sectorsZ{0};
    size_t _nodesPerSector{0};
};

struct QuadNode {
    // Always global node coordinates, including Quadtree's selection output.
    uint32_t level{0};
    uint32_t ix{0};
    uint32_t iz{0};
    float minY{0.0F};
    float maxY{0.0F};
    uint8_t quadrantMask{config::ALL_QUADRANTS};
    NodeAddress address() const { return {level, ix, iz}; }
};

constexpr uint32_t NODE_KEY_COORD_BITS = 28U;
constexpr uint64_t NODE_KEY_COORD_MASK = (1ULL << NODE_KEY_COORD_BITS) - 1ULL;
constexpr uint32_t NODE_KEY_LEVEL_SHIFT = NODE_KEY_COORD_BITS * 2U;

// Conservative surface-stretch estimate shared by metadata aggregation and
// material demand. Preserve steep child features, capped at two extra VT levels.
float cliffDensityScale(float heightRange, float width, float childScale = 1.0F);

uint64_t makeNodeKey(uint32_t level, uint32_t ix, uint32_t iz);
uint64_t makeNodeKey(const QuadNode &node);
// Deduplicate resource requests after independent pass selections; preserve all quadrants.
void mergeNodeSelections(ccstd::vector<QuadNode> &nodes);
// Inputs must be sorted/deduplicated by mergeNodeSelections; output must not alias them.
// Keep geometry quadrants not needed by any color pass, in linear time.
void collectShadowOnlyNodes(const ccstd::vector<QuadNode> &geometryNodes,
                            const ccstd::vector<QuadNode> &surfaceNodes, ccstd::vector<QuadNode> &output);

} // namespace landscape
} // namespace cc

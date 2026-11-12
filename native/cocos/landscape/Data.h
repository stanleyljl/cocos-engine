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
#include <functional>

#include "base/std/container/vector.h"
#include "landscape/Config.h"
#include "math/Vec4.h"

namespace cc {
class Vec3;

namespace landscape {

// Local XZ position. The landscape node is at the minimum XZ corner;
// world position = node world position + local position (translation only).
struct LandscapePoint {
    float x{0};
    float z{0};
};

// Local square: minimum XZ corner and side length.
struct LandscapeRegion {
    float x{0};
    float z{0};
    float size{0};
};

// Grid instance parameters shared by terrain and displaced decals.
struct LandscapeGridParams {
    Vec4 grid; // landscape-local XZ origin, size, geometry LOD
    Vec4 tile; // source tile XZ origin, size, texture array layer
    Vec4 mapping; // Local table slot, packed 2-bit neighbor relations per edge, unused ZW.
};

// Node bounds in landscape-local height units. Stored per level in global Z/X order.
struct HeightRange {
    float minY{0.0F};
    float maxY{0.0F};
};

// CPU data only. Normals retain padded source RGB8 XYZ, unlike the GPU's RG8 XZ.
struct LandscapeQueryTile {
    ccstd::vector<uint8_t> height;
    ccstd::vector<uint8_t> normal;
    ccstd::vector<uint8_t> splat;
};

// Completion is a loader contract shared by Asset and Query. Native workers
// pass success separately; the tile owns its decoded bytes until publication.
using LandscapeQueryCompletion = std::function<void(LandscapeQueryTile, bool)>;

// Integer node indices across all sectors at a geometry/source LOD.
struct NodeAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};

    uint64_t key() const;
    static NodeAddress fromKey(uint64_t key);
    // targetLevel must be an ancestor (>= level), within geometry LOD limits.
    NodeAddress ancestor(uint32_t targetLevel) const;
};

// Shared key encoding for geometry nodes, source tiles and VT pages.
uint64_t makeNodeKey(uint32_t level, uint32_t ix, uint32_t iz);

struct QuadNode {
    // Global geometry coordinates; management branches above sector roots never draw.
    uint32_t level{0};
    uint32_t ix{0};
    uint32_t iz{0};
    float minY{0.0F};
    float maxY{0.0F};
    NodeAddress address() const { return {level, ix, iz}; }
};

// Material coordinates are independent of the geometry quadtree. Level zero
// is the finest page; increasing the level doubles the covered area side.
struct VTPageAddress {
    uint32_t level{0};
    uint32_t x{0};
    uint32_t z{0};

    uint64_t key() const { return makeNodeKey(level, x, z); }
};

// A node owns unique page addresses and a compact index for each grid cell.
// Residency and physical-slot lookup visit pages once, not all 256 cells.
struct VTGridMapping {
    static constexpr uint32_t SIDE_BITS = 4;
    static constexpr uint32_t SIDE = 1U << SIDE_BITS;
    static constexpr uint32_t CELL_COUNT = SIDE * SIDE;
    std::array<uint8_t, CELL_COUNT> cells{};
    ccstd::vector<VTPageAddress> pages;
};

// Source addresses for one material-page composition, independent of residency.
struct VTPageSources {
    NodeAddress splat;
    // 2x2 lookup: base precision repeats one tile; far detail uses up to four.
    // Each source includes its own real-neighbor border for the VT gutters.
    std::array<NodeAddress, config::VT_NORMAL_SOURCE_COUNT> normals;
    // Side projections sample the padded height data of the splat source.
    bool triplanar{false};
    // Append unique addresses in sampling order, preserving existing entries.
    void appendTo(ccstd::vector<NodeAddress> &output) const;
};

// Shared component and renderer settings; contains no runtime resources.
struct LandscapeDebugData {
    bool wireframe{false};
    bool lodColor{false};
    bool showRanges{false};
    bool showBox{false};
    bool unlit{false};
    bool showVTAtlas{false};
    bool heightBlendEnabled{true};
    bool refineFarDetail{true};
    bool decal3DEnabled{true};
    bool triplanarEnabled{true};
    bool physicsWireframe{false};
};

struct LandscapeData {
    uint32_t sectorsX{0};
    uint32_t sectorsZ{0};
    uint32_t maxTileLevel{0}; // Coarsest source tile level; also the sector root level.
    uint32_t minTileLevel{0}; // Finest source tile level; finer geometry nodes sample ancestor tiles.
    float sectorSize{0.0F}; // Positive power of two.
    float heightScale{0.0F};
    float heightBias{0.0F};

    float width() const { return sectorSize * static_cast<float>(sectorsX); }
    float depth() const { return sectorSize * static_cast<float>(sectorsZ); }
    float minHeight() const { return heightBias; }
    float maxHeight() const { return heightBias + heightScale; }
    bool valid() const;

    // L0 is finest; maxTileLevel is the sector root. Invalid levels return zero.
    uint32_t nodesPerSectorSide(uint32_t level) const;
    float nodeSize(uint32_t level) const;
    uint32_t nodesX(uint32_t level) const { return sectorsX * nodesPerSectorSide(level); }
    uint32_t nodesZ(uint32_t level) const { return sectorsZ * nodesPerSectorSide(level); }

    // Sampling includes the outermost boundary. It belongs to the last cell.
    bool contains(LandscapePoint position) const;
    // Requires valid dimensions and a valid level.
    // Explicitly clamps gutter probes and inclusive outer-edge samples.
    NodeAddress nodeAtClamped(uint32_t level, LandscapePoint position) const;
    LandscapeRegion nodeRegion(NodeAddress address) const;
};

// Material-page dimensions are derived together; a geometry maxTileLevel cannot
// accidentally be supplied as the VT root. This class does not own atlas slots.
class VTPageLayout {
public:
    explicit VTPageLayout(float sectorSize = 1.0F);

    uint32_t rootLevel() const { return _rootLevel; }
    float pageSize(uint32_t level) const { return level <= _rootLevel ? _sectorSize / static_cast<float>(1U << (_rootLevel - level)) : 0.0F; }
    LandscapeRegion pageRegion(VTPageAddress page) const {
        const float size = pageSize(page.level);
        return {page.x * size, page.z * size, size};
    }
    // Distance-based material resolution, independent of geometry LOD.
    uint32_t levelForDistance(float distance) const;
    // Subdivide a geometry node by distance; integer cell coordinates determine
    // the containing page, including the complete morph/stitch trajectory.
    VTGridMapping buildGridMapping(const LandscapeData &data, NodeAddress node, float minY, float maxY, const Vec3 &camera) const;

private:
    struct GridMappingContext;
    void mapGridBlock(const GridMappingContext &context, uint32_t left, uint32_t bottom, uint32_t blockLevel, VTGridMapping &result) const;

    float _sectorSize;
    uint32_t _rootLevel{0};
};

} // namespace landscape
} // namespace cc

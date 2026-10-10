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

#include "landscape/LandscapeConfig.h"

namespace cc {
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

struct LandscapeData {
    uint32_t sectorsX{0};
    uint32_t sectorsZ{0};
    uint32_t maxLevel{0};
    uint32_t minTileLevel{0};
    uint32_t tileResolution{0};
    float sectorSize{0.0F};
    float heightScale{0.0F};
    float heightBias{0.0F};

    float width() const { return sectorSize * static_cast<float>(sectorsX); }
    float depth() const { return sectorSize * static_cast<float>(sectorsZ); }
    float minHeight() const { return heightBias; }
    float maxHeight() const { return heightBias + heightScale; }
    bool valid() const;

    // L0 is finest; maxLevel is the sector root. Invalid levels return zero.
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
    // Convert sector-relative node indices to indices across all sectors.
    NodeAddress nodeInSector(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                             uint32_t localX, uint32_t localZ) const;
};

// Index layout for flat node metadata storage: sectors in row-major (Z, X) order, then
// levels from L0 to the root, then nodes in row-major (Z, X) order per level.
// Owns a snapshot of the dimensions and offsets; callers cannot mix layouts.
class NodeIndexLayout {
public:
    static constexpr size_t INVALID_INDEX = std::numeric_limits<size_t>::max();

    NodeIndexLayout() = default;
    // Zero sectors or an invalid maxLevel produce an empty layout.
    explicit NodeIndexLayout(const LandscapeData &data);

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

} // namespace landscape
} // namespace cc

/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

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

#include "landscape/Data.h"
#include "math/Vec3.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace cc {
namespace landscape {
namespace {

constexpr uint32_t NODE_KEY_COORD_BITS = 28U;
constexpr uint64_t NODE_KEY_COORD_MASK = (1ULL << NODE_KEY_COORD_BITS) - 1ULL;
constexpr uint32_t NODE_KEY_LEVEL_SHIFT = NODE_KEY_COORD_BITS * 2U;

} // namespace

uint64_t makeNodeKey(uint32_t level, uint32_t ix, uint32_t iz) {
    return (static_cast<uint64_t>(level) << NODE_KEY_LEVEL_SHIFT) |
           ((static_cast<uint64_t>(ix) & NODE_KEY_COORD_MASK) << NODE_KEY_COORD_BITS) |
           (static_cast<uint64_t>(iz) & NODE_KEY_COORD_MASK);
}

uint64_t NodeAddress::key() const { return makeNodeKey(level, x, z); }

NodeAddress NodeAddress::fromKey(uint64_t key) {
    return {static_cast<uint32_t>(key >> NODE_KEY_LEVEL_SHIFT),
            static_cast<uint32_t>((key >> NODE_KEY_COORD_BITS) & NODE_KEY_COORD_MASK),
            static_cast<uint32_t>(key & NODE_KEY_COORD_MASK)};
}

NodeAddress NodeAddress::ancestor(uint32_t targetLevel) const {
    assert(targetLevel >= level && targetLevel < config::MAX_LOD_LEVELS);
    const uint32_t shift = targetLevel - level;
    return {targetLevel, x >> shift, z >> shift};
}

void VTPageSources::appendTo(ccstd::vector<NodeAddress> &output) const {
    // Return each input once, in sampling order. Base-precision normal entries
    // share one tile, which may also provide the splat data.
    const auto append = [&output](NodeAddress source) {
        const auto key = source.key();
        const auto duplicate = std::find_if(output.begin(), output.end(), [key](NodeAddress entry) {
            return entry.key() == key;
        });
        if (duplicate == output.end()) {
            output.push_back(source);
        }
    };
    append(splat);
    for (const auto source : normals) {
        append(source);
    }
}

bool LandscapeData::valid() const {
    return sectorsX > 0U && sectorsZ > 0U && maxLevel < config::MAX_LOD_LEVELS &&
           static_cast<uint64_t>(sectorsX) * sectorsZ <= config::MAX_SECTOR_COUNT &&
           minTileLevel <= maxLevel && sectorSize > 0.0F &&
           heightScale > 0.0F;
}

uint32_t LandscapeData::nodesPerSectorSide(uint32_t level) const {
    return maxLevel < config::MAX_LOD_LEVELS && level <= maxLevel
        ? 1U << (maxLevel - level) : 0U;
}

float LandscapeData::nodeSize(uint32_t level) const {
    const uint32_t side = nodesPerSectorSide(level);
    return side != 0U ? sectorSize / static_cast<float>(side) : 0.0F;
}

bool LandscapeData::contains(LandscapePoint position) const {
    return position.x >= 0 && position.z >= 0 && position.x <= width() && position.z <= depth();
}

NodeAddress LandscapeData::nodeAtClamped(uint32_t level, LandscapePoint position) const {
    assert(nodesX(level) > 0 && nodesZ(level) > 0 && sectorSize > 0);
    const float size = nodeSize(level);
    return {level,
        static_cast<uint32_t>(std::clamp(std::floor(position.x / size), 0.0F, float(nodesX(level) - 1U))),
        static_cast<uint32_t>(std::clamp(std::floor(position.z / size), 0.0F, float(nodesZ(level) - 1U)))};
}

LandscapeRegion LandscapeData::nodeRegion(NodeAddress address) const {
    const float size = nodeSize(address.level);
    return {address.x * size, address.z * size, size};
}

NodeIndexLayout::NodeIndexLayout(const LandscapeData &data) {
    if (data.sectorsX == 0U || data.sectorsZ == 0U || data.maxLevel >= config::MAX_LOD_LEVELS ||
        static_cast<uint64_t>(data.sectorsX) * data.sectorsZ > config::MAX_SECTOR_COUNT) {
        return;
    }
    _sectorsX = data.sectorsX;
    _sectorsZ = data.sectorsZ;
    _levelCount = data.maxLevel + 1U;
    for (uint32_t level = 0; level < _levelCount; ++level) {
        const uint32_t side = data.nodesPerSectorSide(level);
        _levels[level] = Level{side, _nodesPerSector};
        _nodesPerSector += static_cast<size_t>(side) * side;
    }
}

size_t NodeIndexLayout::sectorNodeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level, uint32_t localX, uint32_t localZ) const {
    if (level >= _levelCount || sectorX >= _sectorsX || sectorZ >= _sectorsZ) {
        return INVALID_INDEX;
    }
    const auto &layout = _levels[level];
    if (localX >= layout.nodesPerSide || localZ >= layout.nodesPerSide) {
        return INVALID_INDEX;
    }
    const size_t sectorIndex = static_cast<size_t>(sectorZ) * _sectorsX + sectorX;
    return sectorIndex * _nodesPerSector + layout.offset + static_cast<size_t>(localZ) * layout.nodesPerSide + localX;
}

size_t NodeIndexLayout::globalNodeIndex(uint32_t level, uint32_t globalX, uint32_t globalZ) const {
    if (level >= _levelCount) {
        return INVALID_INDEX;
    }
    const uint32_t side = _levels[level].nodesPerSide;
    return sectorNodeIndex(globalX / side, globalZ / side, level, globalX % side, globalZ % side);
}

VTPageLayout::VTPageLayout(float sectorSize)
: _sectorSize(sectorSize) {
    assert(sectorSize > 0);
    _rootLevel = static_cast<uint32_t>(std::clamp(std::ilogb(sectorSize), 0, 27));
}

uint32_t VTPageLayout::levelForDistance(float distance) const {
    // Start with the sector page and refine while the next level meets the
    // requested size. Most mapping queries are for distant, coarse pages.
    uint32_t level = _rootLevel;
    float nextSize = _sectorSize * 0.5F;
    const float target = distance * 0.5F;
    while (level > 0 && nextSize >= target) {
        --level;
        nextSize *= 0.5F;
    }
    return level;
}

VTGridMapping VTPageLayout::buildGridMapping(LandscapeRegion region, float minY, float maxY, const Vec3 &camera) const {
    VTGridMapping result;
    const float heightDistance = std::max({minY - camera.y, camera.y - maxY, 0.0F});
    const LandscapeRegion firstCell{region.x, region.z, region.size / static_cast<float>(VTGridMapping::SIDE)};
    constexpr uint32_t half = VTGridMapping::SIDE / 2;
    // Four aligned quadrants; only subdivide where material resolution needs it.
    for (uint32_t z = 0; z < VTGridMapping::SIDE; z += half) {
        for (uint32_t x = 0; x < VTGridMapping::SIDE; x += half) {
            mapCells(firstCell, camera, heightDistance, x, z, half, result);
        }
    }
    return result;
}

VTPageAddress VTPageLayout::coveringPage(uint32_t desiredLevel, LandscapePoint min, LandscapePoint max) const {
    assert(desiredLevel <= _rootLevel);
    for (uint32_t level = desiredLevel; level <= _rootLevel; ++level) {
        const float size = pageSize(level);
        const VTPageAddress page{level,
                                 static_cast<uint32_t>(std::floor(min.x / size + 1e-7F)),
                                 static_cast<uint32_t>(std::floor(min.z / size + 1e-7F))};
        if (max.x <= (page.x + 1.0F) * size + size * 1e-7F &&
            max.z <= (page.z + 1.0F) * size + size * 1e-7F) {
            return page;
        }
    }
    assert(false && "VT coverage must not cross sector boundaries");
    return {};
}

void VTPageLayout::mapCells(const LandscapeRegion &firstCell, const Vec3 &camera, float heightDistance, uint32_t left, uint32_t bottom, uint32_t side, VTGridMapping &mapping) const {
    const float cellSize = firstCell.size;
    const float x = firstCell.x + left * cellSize;
    const float z = firstCell.z + bottom * cellSize;
    const float size = side * cellSize;
    const float dx = std::max({x - camera.x, camera.x - (x + size), 0.0F});
    const float dz = std::max({z - camera.z, camera.z - (z + size), 0.0F});
    const uint32_t desiredLevel = levelForDistance(std::sqrt(dx * dx + heightDistance * heightDistance + dz * dz));
    if (side > 1 && pageSize(desiredLevel) < size) {
        const uint32_t half = side / 2;
        for (uint32_t row = bottom; row < bottom + side; row += half) {
            for (uint32_t column = left; column < left + side; column += half) {
                mapCells(firstCell, camera, heightDistance, column, row, half, mapping);
            }
        }
        return;
    }
    // Odd vertices collapse to the preceding even vertex during morph/stitching.
    // One page must cover the whole trajectory, including the cell's far edge.
    const auto page = coveringPage(desiredLevel,
                                  {x - (left & 1U) * cellSize, z - (bottom & 1U) * cellSize},
                                  {x + size, z + size});
    const auto found = std::find_if(mapping.pages.begin(), mapping.pages.end(), [page](const auto &entry) {
        return entry.key() == page.key();
    });
    const size_t index = static_cast<size_t>(found - mapping.pages.begin());
    assert(index < VTGridMapping::CELL_COUNT);
    if (found == mapping.pages.end()) {
        mapping.pages.push_back(page);
    }
    for (uint32_t row = bottom; row < bottom + side; ++row) {
        std::fill_n(mapping.cells.begin() + row * VTGridMapping::SIDE + left, side, static_cast<uint8_t>(index));
    }
}

} // namespace landscape
} // namespace cc

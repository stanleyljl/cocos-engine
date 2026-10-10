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

#include "landscape/LandscapeData.h"
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

bool LandscapeData::valid() const {
    return sectorsX > 0U && sectorsZ > 0U && maxLevel < config::MAX_LOD_LEVELS &&
           minTileLevel <= maxLevel && tileResolution > 1U && sectorSize > 0.0F &&
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

NodeAddress LandscapeData::nodeInSector(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                                       uint32_t localX, uint32_t localZ) const {
    const uint32_t side = nodesPerSectorSide(level);
    assert(sectorX < sectorsX && sectorZ < sectorsZ && localX < side && localZ < side);
    return {level, sectorX * side + localX, sectorZ * side + localZ};
}

NodeIndexLayout::NodeIndexLayout(const LandscapeData &data) {
    if (data.sectorsX == 0U || data.sectorsZ == 0U || data.maxLevel >= config::MAX_LOD_LEVELS) {
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

size_t NodeIndexLayout::sectorNodeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                                       uint32_t localX, uint32_t localZ) const {
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

} // namespace landscape
} // namespace cc

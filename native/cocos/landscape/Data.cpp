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

NodeAddress NodeAddress::child(uint32_t quadrant) const {
    assert(level > 0 && quadrant < 4);
    return {level - 1, x * 2 + (quadrant & 1U), z * 2 + (quadrant >> 1U)};
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
    return sectorsX > 0U && sectorsZ > 0U && maxTileLevel < config::MAX_LOD_LEVELS &&
           static_cast<uint64_t>(sectorsX) * sectorsZ <= config::MAX_SECTOR_COUNT &&
           minTileLevel <= maxTileLevel && sectorSize > 0.0F &&
           std::scalbn(1.0F, std::ilogb(sectorSize)) == sectorSize &&
           heightScale > 0.0F;
}

uint32_t LandscapeData::nodesPerSectorSide(uint32_t level) const {
    return maxTileLevel < config::MAX_LOD_LEVELS && level <= maxTileLevel
        ? 1U << (maxTileLevel - level) : 0U;
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

VTPageLayout::VTPageLayout(float sectorSize)
: _sectorSize(sectorSize) {
    assert(sectorSize > 0 && std::scalbn(1.0F, std::ilogb(sectorSize)) == sectorSize);
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

// Immutable inputs shared by one synchronous mapping traversal; never retained.
struct VTPageLayout::GridMappingContext {
    NodeAddress node;
    LandscapeRegion region;
    float cellSize;
    // VT-relative level; negative when cells are smaller than an L0 page.
    int32_t cellLevel;
    const Vec3 &camera;
    float heightDistance;
};

VTGridMapping VTPageLayout::buildGridMapping(const LandscapeData &data, NodeAddress node, float minY, float maxY, const Vec3 &camera) const {
    assert(data.sectorSize == _sectorSize && node.level <= data.maxTileLevel);
    assert(node.x < data.nodesX(node.level) && node.z < data.nodesZ(node.level));
    VTGridMapping result;
    const auto region = data.nodeRegion(node);
    const GridMappingContext context{
        node,
        region,
        region.size / static_cast<float>(VTGridMapping::SIDE),
        static_cast<int32_t>(_rootLevel) - static_cast<int32_t>(data.maxTileLevel - node.level + VTGridMapping::SIDE_BITS),
        camera,
        std::max({minY - camera.y, camera.y - maxY, 0.0F})};
    // Keep the four initial quadrants and traversal order used for page admission.
    constexpr uint32_t half = VTGridMapping::SIDE / 2;
    for (uint32_t z = 0; z < VTGridMapping::SIDE; z += half) {
        for (uint32_t x = 0; x < VTGridMapping::SIDE; x += half) {
            mapGridBlock(context, x, z, VTGridMapping::SIDE_BITS - 1, result);
        }
    }
    return result;
}

void VTPageLayout::mapGridBlock(const GridMappingContext &context, uint32_t left, uint32_t bottom, uint32_t blockLevel, VTGridMapping &result) const {
    const uint32_t side = 1U << blockLevel;
    const float x = context.region.x + left * context.cellSize;
    const float z = context.region.z + bottom * context.cellSize;
    const float size = side * context.cellSize;
    const float dx = std::max({x - context.camera.x, context.camera.x - (x + size), 0.0F});
    const float dz = std::max({z - context.camera.z, context.camera.z - (z + size), 0.0F});
    const uint32_t desiredLevel = levelForDistance(std::sqrt(dx * dx + context.heightDistance * context.heightDistance + dz * dz));
    if (blockLevel > 0 && pageSize(desiredLevel) < size) {
        const uint32_t half = side / 2;
        for (uint32_t row = bottom; row < bottom + side; row += half) {
            for (uint32_t column = left; column < left + side; column += half) {
                mapGridBlock(context, column, row, blockLevel - 1, result);
            }
        }
        return;
    }
    // Aligned blocks contain every morph target. An odd single cell needs
    // its 2x2 parent because odd vertices move to the preceding even vertex.
    const int32_t coverageLevel = context.cellLevel + static_cast<int32_t>(blockLevel) + (blockLevel == 0 && ((left | bottom) & 1U) ? 1 : 0);
    const uint32_t level = static_cast<uint32_t>(std::max(static_cast<int32_t>(desiredLevel), coverageLevel));
    const uint32_t shift = static_cast<uint32_t>(static_cast<int32_t>(level) - context.cellLevel);
    const VTPageAddress page{level, (context.node.x * VTGridMapping::SIDE + left) >> shift, (context.node.z * VTGridMapping::SIDE + bottom) >> shift};
    const auto found = std::find_if(result.pages.begin(), result.pages.end(), [page](const auto &entry) {
        return entry.key() == page.key();
    });
    const size_t index = static_cast<size_t>(found - result.pages.begin());
    assert(index < VTGridMapping::CELL_COUNT);
    if (found == result.pages.end()) {
        result.pages.push_back(page);
    }
    for (uint32_t row = bottom; row < bottom + side; ++row) {
        std::fill_n(result.cells.begin() + row * VTGridMapping::SIDE + left, side, static_cast<uint8_t>(index));
    }
}

} // namespace landscape
} // namespace cc

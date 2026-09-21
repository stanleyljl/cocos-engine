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

#include "landscape/LandscapeConfig.h"
#include <algorithm>

namespace cc {
namespace landscape {

size_t computeSectorNodeLayout(uint32_t maxLevel, ccstd::vector<size_t> &levelOffsets) {
    if (maxLevel >= config::MAX_LOD_LEVELS) {
        levelOffsets.clear();
        return 0U;
    }
    levelOffsets.resize(static_cast<size_t>(maxLevel) + 1U);
    size_t nodesPerSector = 0U;
    for (uint32_t level = 0; level <= maxLevel; ++level) {
        levelOffsets[level] = nodesPerSector;
        const size_t side = computeNodesPerSide(maxLevel, level);
        nodesPerSector += side * side;
    }
    return nodesPerSector;
}

bool LandscapeData::valid() const {
    return sectorsX > 0U && sectorsZ > 0U && maxLevel < config::MAX_LOD_LEVELS &&
           minTileLevel <= maxLevel && tileResolution > 1U && sectorSize > 0.0F &&
           heightScale > 0.0F;
}

uint32_t computeNodesPerSide(uint32_t maxLevel, uint32_t level) {
    return maxLevel < config::MAX_LOD_LEVELS && level <= maxLevel
        ? 1U << (maxLevel - level) : 0U;
}

float computeNodeSize(float sectorSize, uint32_t maxLevel, uint32_t level) {
    const uint32_t side = computeNodesPerSide(maxLevel, level);
    return side != 0U ? sectorSize / static_cast<float>(side) : 0.0F;
}

size_t nodeRangeIndex(const LandscapeData &data, const ccstd::vector<size_t> &levelOffsets,
                      size_t nodesPerSector, uint32_t sectorX, uint32_t sectorZ,
                      uint32_t level, uint32_t localX, uint32_t localZ) {
    const uint32_t side = computeNodesPerSide(data.maxLevel, level);
    if (side == 0U || level >= levelOffsets.size() || nodesPerSector == 0U ||
        sectorX >= data.sectorsX || sectorZ >= data.sectorsZ || localX >= side || localZ >= side) {
        return INVALID_NODE_INDEX;
    }
    const size_t sectorIndex = static_cast<size_t>(sectorZ) * data.sectorsX + sectorX;
    return sectorIndex * nodesPerSector + levelOffsets[level] + static_cast<size_t>(localZ) * side + localX;
}

size_t globalNodeRangeIndex(const LandscapeData &data, const ccstd::vector<size_t> &levelOffsets,
                            size_t nodesPerSector, uint32_t level, uint32_t globalX, uint32_t globalZ) {
    const uint32_t side = computeNodesPerSide(data.maxLevel, level);
    if (side == 0U) return INVALID_NODE_INDEX;
    return nodeRangeIndex(data, levelOffsets, nodesPerSector, globalX / side, globalZ / side,
                          level, globalX % side, globalZ % side);
}

uint64_t makeNodeKey(uint32_t level, uint32_t ix, uint32_t iz) {
    return (static_cast<uint64_t>(level) << NODE_KEY_LEVEL_SHIFT) |
           ((static_cast<uint64_t>(ix) & NODE_KEY_COORD_MASK) << NODE_KEY_COORD_BITS) |
           (static_cast<uint64_t>(iz) & NODE_KEY_COORD_MASK);
}

uint64_t makeNodeKey(const QuadNode &node) {
    return makeNodeKey(node.level, node.ix, node.iz);
}

uint64_t makeQuadrantKey(const QuadNode &node, uint32_t quadrant) {
    return (makeNodeKey(node) << 2U) | static_cast<uint64_t>(quadrant);
}

void mergeNodeSelections(ccstd::vector<QuadNode> &nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const QuadNode &a, const QuadNode &b) {
        return makeNodeKey(a) < makeNodeKey(b);
    });
    size_t count = 0;
    for (const auto &node : nodes) {
        if (count > 0 && makeNodeKey(nodes[count - 1]) == makeNodeKey(node)) {
            auto &merged = nodes[count - 1];
            merged.quadrantMask |= node.quadrantMask;
            merged.minY = std::min(merged.minY, node.minY);
            merged.maxY = std::max(merged.maxY, node.maxY);
        } else {
            nodes[count++] = node;
        }
    }
    nodes.resize(count);
}

void collectShadowOnlyNodes(const ccstd::vector<QuadNode> &geometryNodes,
                            const ccstd::vector<QuadNode> &surfaceNodes, ccstd::vector<QuadNode> &output) {
    output.clear();
    auto surface = surfaceNodes.begin();
    for (auto node : geometryNodes) {
        const auto key = makeNodeKey(node);
        while (surface != surfaceNodes.end() && makeNodeKey(*surface) < key) ++surface;
        if (surface != surfaceNodes.end() && makeNodeKey(*surface) == key) {
            node.quadrantMask &= static_cast<uint8_t>(~surface->quadrantMask);
        }
        if (node.quadrantMask != 0) output.push_back(node);
    }
}

} // namespace landscape
} // namespace cc

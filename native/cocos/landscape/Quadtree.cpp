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

#include "landscape/Quadtree.h"

#include <algorithm>
#include <cmath>

#include "core/geometry/AABB.h"
#include "core/geometry/Frustum.h"
#include "landscape/LandscapeAsset.h"

namespace cc {
namespace landscape {

namespace {
float distanceToAABB(const Vec3 &point, const geometry::AABB &box) {
    const Vec3 &center = box.getCenter();
    const Vec3 &halfExtents = box.getHalfExtents();
    const float dx = std::max(0.0F, std::abs(point.x - center.x) - halfExtents.x);
    const float dy = std::max(0.0F, std::abs(point.y - center.y) - halfExtents.y);
    const float dz = std::max(0.0F, std::abs(point.z - center.z) - halfExtents.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

Quadtree::Quadtree() {
    _box = ccnew geometry::AABB();
}

Quadtree::~Quadtree() = default;

bool Quadtree::init(const LandscapeAsset &asset) {
    if (!asset.valid()) {
        return false;
    }
    _data = asset.data();
    _selected.clear();
    _levelOffsets.assign(static_cast<size_t>(_data.maxLevel) + 1U, 0U);
    _nodesPerSector = 0U;
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        _levelOffsets[level] = _nodesPerSector;
        const uint32_t side = 1U << (_data.maxLevel - level);
        _nodesPerSector += static_cast<size_t>(side) * side;
    }

    const size_t sectorCount = static_cast<size_t>(_data.sectorsX) * _data.sectorsZ;
    _heightRanges.assign(sectorCount * _nodesPerSector,
                         HeightRange{_data.minHeight(), _data.maxHeight()});
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        const uint32_t side = 1U << (_data.maxLevel - level);
        for (uint32_t globalZ = 0; globalZ < _data.sectorsZ * side; ++globalZ) {
            for (uint32_t globalX = 0; globalX < _data.sectorsX * side; ++globalX) {
                float minY = _data.minHeight();
                float maxY = _data.maxHeight();
                if (!asset.getHeightRange(level, globalX, globalZ, minY, maxY)) {
                    return false;
                }
                const uint32_t sectorX = globalX / side;
                const uint32_t sectorZ = globalZ / side;
                const uint32_t localX = globalX % side;
                const uint32_t localZ = globalZ % side;
                _heightRanges[nodeRangeIndex(sectorX, sectorZ, level, localX, localZ)] = HeightRange{minY, maxY};
            }
        }
    }
    computeRanges();
    return true;
}

void Quadtree::setUseLevelHeightRanges(bool enabled) {
    if (_useLevelHeightRanges == enabled) {
        return;
    }
    _useLevelHeightRanges = enabled;
    if (_data.valid()) {
        computeRanges();
    }
}

size_t Quadtree::nodeRangeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                                uint32_t ix, uint32_t iz) const {
    const uint32_t nodesPerSide = 1U << (_data.maxLevel - level);
    const size_t sectorIndex = static_cast<size_t>(sectorZ) * _data.sectorsX + sectorX;
    return sectorIndex * _nodesPerSector + _levelOffsets[level] +
           static_cast<size_t>(iz) * nodesPerSide + ix;
}

void Quadtree::computeRanges() {
    if (_useLevelHeightRanges) {
        computeRangesPerLevel();
    } else {
        computeRangesGlobal();
    }
}

void Quadtree::computeRangesGlobal() {
    const size_t levelCount = static_cast<size_t>(_data.maxLevel) + 1U;
    _lodRange.assign(levelCount, 0.0F);
    _lodMorphStart.assign(levelCount, 0.0F);
    _lodMorphEnd.assign(levelCount, 0.0F);

    const float leaf = _data.sectorSize / static_cast<float>(1U << _data.maxLevel);
    float rangePrev = 0.0F;
    float diagPrev = 0.0F;
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        const float size = leaf * static_cast<float>(1U << level);
        const float height = _data.maxHeight() - _data.minHeight();
        const float diag = std::sqrt(2.0F * size * size + height * height);
        const float morphStart = rangePrev + diagPrev + diag;
        const float range = rangePrev + (morphStart - rangePrev) / config::MORPH_RATIO;
        _lodMorphStart[level] = morphStart * config::VIS_SAFETY;
        _lodRange[level] = range * config::VIS_SAFETY;
        _lodMorphEnd[level] = range * config::VIS_SAFETY;
        rangePrev = range;
        diagPrev = diag;
    }
}

void Quadtree::computeRangesPerLevel() {
    const size_t levelCount = static_cast<size_t>(_data.maxLevel) + 1U;
    _lodRange.assign(levelCount, 0.0F);
    _lodMorphStart.assign(levelCount, 0.0F);
    _lodMorphEnd.assign(levelCount, 0.0F);

    const float globalHeight = _data.maxHeight() - _data.minHeight();
    ccstd::vector<float> levelHeight(levelCount, globalHeight);
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        const uint32_t nodesPerSide = 1U << (_data.maxLevel - level);
        const size_t nodeCount = static_cast<size_t>(nodesPerSide) * nodesPerSide;
        float maxHeight = 0.0F;
        for (uint32_t sectorZ = 0; sectorZ < _data.sectorsZ; ++sectorZ) {
            for (uint32_t sectorX = 0; sectorX < _data.sectorsX; ++sectorX) {
                const size_t sectorOffset =
                    (static_cast<size_t>(sectorZ) * _data.sectorsX + sectorX) * _nodesPerSector +
                    _levelOffsets[level];
                for (size_t index = 0; index < nodeCount; ++index) {
                    const HeightRange &range = _heightRanges[sectorOffset + index];
                    maxHeight = std::max(maxHeight, range.maxY - range.minY);
                }
            }
        }
        // Keep a valid conservative fallback if the height table is absent or
        // contains an invalid span for a level.
        if (maxHeight > 0.0F) {
            levelHeight[level] = maxHeight;
        }
    }

    const float leaf = _data.sectorSize / static_cast<float>(1U << _data.maxLevel);
    float rangePrev = 0.0F;
    float diagPrev = 0.0F;
    for (uint32_t level = 0; level <= _data.maxLevel; ++level) {
        const float size = leaf * static_cast<float>(1U << level);
        const float height = levelHeight[level];
        const float diag = std::sqrt(2.0F * size * size + height * height);
        const float morphStart = rangePrev + diagPrev + diag;
        const float range = rangePrev + (morphStart - rangePrev) / config::MORPH_RATIO;
        _lodMorphStart[level] = morphStart * config::VIS_SAFETY;
        _lodRange[level] = range * config::VIS_SAFETY;
        _lodMorphEnd[level] = range * config::VIS_SAFETY;
        rangePrev = range;
        diagPrev = diag;
    }
}

void Quadtree::nodeHeightRange(uint32_t level, uint32_t ix, uint32_t iz,
                               float &minY, float &maxY) const {
    minY = _data.minHeight();
    maxY = _data.maxHeight();
    if (_heightRanges.empty() || _sectorX >= _data.sectorsX || _sectorZ >= _data.sectorsZ ||
        level > _data.maxLevel) {
        return;
    }
    const uint32_t nodesPerSide = 1U << (_data.maxLevel - level);
    if (ix >= nodesPerSide || iz >= nodesPerSide) {
        return;
    }
    const HeightRange &range = _heightRanges[nodeRangeIndex(_sectorX, _sectorZ, level, ix, iz)];
    minY = range.minY;
    maxY = range.maxY;
}

const ccstd::vector<QuadNode> &Quadtree::select(const Vec3 &camPos, const geometry::Frustum &frustum,
                                                const Vec3 &sectorOrigin, uint32_t sectorX, uint32_t sectorZ) {
    _camPos = camPos;
    _frustum = &frustum;
    _sectorOrigin = sectorOrigin;
    _sectorX = sectorX;
    _sectorZ = sectorZ;
    _selected.clear();
    if (!_data.valid() || sectorX >= _data.sectorsX || sectorZ >= _data.sectorsZ) {
        return _selected;
    }
    traverse(_data.maxLevel, 0U, 0U);
    return _selected;
}

void Quadtree::traverse(uint32_t level, uint32_t ix, uint32_t iz) {
    const float size = _data.sectorSize / static_cast<float>(1U << (_data.maxLevel - level));
    float minY = _data.minHeight();
    float maxY = _data.maxHeight();
    nodeHeightRange(level, ix, iz, minY, maxY);
    const float x = static_cast<float>(ix) * size - _data.sectorSize * 0.5F + _sectorOrigin.x;
    const float z = static_cast<float>(iz) * size - _data.sectorSize * 0.5F + _sectorOrigin.z;
    _box->setCenter(x + size * 0.5F, (minY + maxY) * 0.5F + _sectorOrigin.y, z + size * 0.5F);
    _box->setHalfExtents(size * 0.5F, (maxY - minY) * 0.5F, size * 0.5F);

    if (!_box->aabbFrustum(*_frustum)) {
        return;
    }
    if (level == 0U || distanceToAABB(_camPos, *_box) > _lodRange[level - 1U]) {
        _selected.push_back(QuadNode{level, ix, iz, minY, maxY});
        return;
    }
    for (uint32_t dz = 0; dz < 2U; ++dz) {
        for (uint32_t dx = 0; dx < 2U; ++dx) {
            traverse(level - 1U, ix * 2U + dx, iz * 2U + dz);
        }
    }
}

} // namespace landscape
} // namespace cc

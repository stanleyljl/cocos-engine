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

#include "landscape/Quadtree.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "core/geometry/AABB.h"
#include "core/geometry/Frustum.h"

namespace cc {
namespace landscape {

namespace {
float hash2(int x, int y) {
    uint32_t n = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
    n = (n ^ (n >> 13U)) * 1274126177U;
    n ^= n >> 16U;
    return static_cast<float>(n) / 4294967295.0F;
}

float smoothNoise(float x, float y) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float u = fx * fx * (3.0F - 2.0F * fx);
    const float v = fy * fy * (3.0F - 2.0F * fy);
    const float a = hash2(ix, iy);
    const float b = hash2(ix + 1, iy);
    const float c = hash2(ix, iy + 1);
    const float d = hash2(ix + 1, iy + 1);
    const float ab = a + (b - a) * u;
    const float cd = c + (d - c) * u;
    return ab + (cd - ab) * v;
}

float fbm(float x, float y) {
    float sum = 0.0F;
    float amplitude = 0.5F;
    float frequency = 1.0F;
    for (uint32_t octave = 0; octave < 6U; ++octave) {
        sum += amplitude * smoothNoise(x * frequency, y * frequency);
        frequency *= 2.0F;
        amplitude *= 0.5F;
    }
    return sum;
}

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

void Quadtree::setConfig(float sectorSize, uint32_t maxLevel, float minY, float maxY) {
    const bool valid = std::isfinite(sectorSize) && sectorSize > 0.0F &&
                       maxLevel < config::MAX_LOD_LEVELS && std::isfinite(minY) &&
                       std::isfinite(maxY) && maxY >= minY;
    if (valid) {
        _sectorSize = sectorSize;
        _maxLevel = maxLevel;
        _minY = minY;
        _maxY = maxY;
    } else {
        _sectorSize = config::SECTOR_SIZE;
        _maxLevel = config::MAX_LEVEL;
        _minY = config::TERRAIN_MIN_Y;
        _maxY = config::TERRAIN_MAX_Y;
    }
    computeRanges();
    if (!_heightmap.empty()) {
        buildHeightRanges();
    }
}

void Quadtree::setProceduralWorld(uint32_t sectorsX, uint32_t sectorsZ) {
    _sectorsX = std::max(1U, sectorsX);
    _sectorsZ = std::max(1U, sectorsZ);
    const uint32_t size = config::DEMO_HM_SIZE;
    _heightmap.resize(static_cast<size_t>(size) * size);
    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(z) / static_cast<float>(size);
            const float elevation = fbm(u * 6.0F * static_cast<float>(_sectorsX),
                                        v * 6.0F * static_cast<float>(_sectorsZ));
            _heightmap[static_cast<size_t>(z) * size + x] = std::pow(elevation, 1.7F);
        }
    }
    buildHeightRanges();
}

float Quadtree::proceduralHeightAt(float x, float z) const {
    if (_heightmap.empty()) {
        return _minY;
    }

    const uint32_t mapSize = config::DEMO_HM_SIZE;
    const float worldWidth = _sectorSize * static_cast<float>(_sectorsX);
    const float worldDepth = _sectorSize * static_cast<float>(_sectorsZ);
    const float u = (x + worldWidth * 0.5F) / worldWidth;
    const float v = (z + worldDepth * 0.5F) / worldDepth;
    const auto clampIndex = [mapSize](float value) {
        const int index = static_cast<int>(value * static_cast<float>(mapSize));
        return static_cast<uint32_t>(std::max(0, std::min(index, static_cast<int>(mapSize - 1U))));
    };
    const uint32_t tx = clampIndex(u);
    const uint32_t tz = clampIndex(v);
    return _minY + _heightmap[static_cast<size_t>(tz) * mapSize + tx] * (_maxY - _minY);
}

size_t Quadtree::nodeRangeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                                uint32_t ix, uint32_t iz) const {
    const uint32_t nodesPerSide = 1U << (_maxLevel - level);
    const size_t sectorIndex = static_cast<size_t>(sectorZ) * _sectorsX + sectorX;
    return sectorIndex * _nodesPerSector + _levelOffsets[level] +
           static_cast<size_t>(iz) * nodesPerSide + ix;
}

void Quadtree::sampleHeightRange(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                                 uint32_t ix, uint32_t iz, float &minY, float &maxY) const {
    minY = _minY;
    maxY = _maxY;
    if (_heightmap.empty()) {
        return;
    }

    const uint32_t mapSize = config::DEMO_HM_SIZE;
    const uint32_t nodesPerSector = 1U << (_maxLevel - level);
    const uint32_t globalX = sectorX * nodesPerSector + ix;
    const uint32_t globalZ = sectorZ * nodesPerSector + iz;
    const float worldWidth = _sectorSize * static_cast<float>(_sectorsX);
    const float worldDepth = _sectorSize * static_cast<float>(_sectorsZ);
    // globalX/globalZ are indices of nodes at this level, not sector indices.
    // Convert them using the actual world size of one node. Without this
    // factor, most leaves clamp to the last heightmap texel and produce
    // incorrect AABBs for both frustum culling and distance-based LOD.
    const float nodeWorldSize = _sectorSize / static_cast<float>(nodesPerSector);
    const float nodeX0 = static_cast<float>(globalX) * nodeWorldSize;
    const float nodeZ0 = static_cast<float>(globalZ) * nodeWorldSize;
    const float nodeX1 = nodeX0 + nodeWorldSize;
    const float nodeZ1 = nodeZ0 + nodeWorldSize;
    const uint32_t x0 = static_cast<uint32_t>(std::max(0.0F, std::floor(nodeX0 / worldWidth * mapSize)));
    const uint32_t z0 = static_cast<uint32_t>(std::max(0.0F, std::floor(nodeZ0 / worldDepth * mapSize)));
    const uint32_t x1 = std::min(mapSize - 1U, static_cast<uint32_t>(std::ceil(nodeX1 / worldWidth * mapSize)));
    const uint32_t z1 = std::min(mapSize - 1U, static_cast<uint32_t>(std::ceil(nodeZ1 / worldDepth * mapSize)));

    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (uint32_t z = z0; z <= z1; ++z) {
        for (uint32_t x = x0; x <= x1; ++x) {
            const float value = _heightmap[static_cast<size_t>(z) * mapSize + x];
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
    }
    minY = _minY + lo * (_maxY - _minY);
    maxY = _minY + hi * (_maxY - _minY);
}

void Quadtree::buildHeightRanges() {
    _levelOffsets.assign(static_cast<size_t>(_maxLevel) + 1U, 0U);
    _nodesPerSector = 0U;
    for (uint32_t level = 0; level <= _maxLevel; ++level) {
        _levelOffsets[level] = _nodesPerSector;
        const uint32_t nodesPerSide = 1U << (_maxLevel - level);
        _nodesPerSector += static_cast<size_t>(nodesPerSide) * nodesPerSide;
    }

    const size_t sectorCount = static_cast<size_t>(_sectorsX) * _sectorsZ;
    _heightRanges.assign(sectorCount * _nodesPerSector, HeightRange{_minY, _maxY});
    if (_heightmap.empty()) {
        return;
    }

    // Leaf ranges scan the source heightmap once. Parent ranges are then
    // merged from their four children, matching the source CDLOD tree while
    // keeping all range work out of the per-frame selection path.
    const uint32_t leafSide = 1U << _maxLevel;
    for (uint32_t sectorZ = 0; sectorZ < _sectorsZ; ++sectorZ) {
        for (uint32_t sectorX = 0; sectorX < _sectorsX; ++sectorX) {
            for (uint32_t iz = 0; iz < leafSide; ++iz) {
                for (uint32_t ix = 0; ix < leafSide; ++ix) {
                    float minY = _minY;
                    float maxY = _maxY;
                    sampleHeightRange(sectorX, sectorZ, 0U, ix, iz, minY, maxY);
                    _heightRanges[nodeRangeIndex(sectorX, sectorZ, 0U, ix, iz)] = HeightRange{minY, maxY};
                }
            }

            for (uint32_t level = 1U; level <= _maxLevel; ++level) {
                const uint32_t nodesPerSide = 1U << (_maxLevel - level);
                for (uint32_t iz = 0; iz < nodesPerSide; ++iz) {
                    for (uint32_t ix = 0; ix < nodesPerSide; ++ix) {
                        const HeightRange &child00 = _heightRanges[nodeRangeIndex(sectorX, sectorZ, level - 1U, ix * 2U, iz * 2U)];
                        const HeightRange &child10 = _heightRanges[nodeRangeIndex(sectorX, sectorZ, level - 1U, ix * 2U + 1U, iz * 2U)];
                        const HeightRange &child01 = _heightRanges[nodeRangeIndex(sectorX, sectorZ, level - 1U, ix * 2U, iz * 2U + 1U)];
                        const HeightRange &child11 = _heightRanges[nodeRangeIndex(sectorX, sectorZ, level - 1U, ix * 2U + 1U, iz * 2U + 1U)];
                        _heightRanges[nodeRangeIndex(sectorX, sectorZ, level, ix, iz)] = HeightRange{
                            std::min(std::min(child00.minY, child10.minY), std::min(child01.minY, child11.minY)),
                            std::max(std::max(child00.maxY, child10.maxY), std::max(child01.maxY, child11.maxY)),
                        };
                    }
                }
            }
        }
    }
}

void Quadtree::computeRanges() {
    const size_t levelCount = static_cast<size_t>(_maxLevel) + 1U;
    _lodRange.assign(levelCount, 0.0F);
    _lodMorphStart.assign(levelCount, 0.0F);
    _lodMorphEnd.assign(levelCount, 0.0F);

    const float leaf = _sectorSize / static_cast<float>(1U << _maxLevel);
    float rangePrev = 0.0F;
    float diagPrev = 0.0F;
    for (uint32_t level = 0; level <= _maxLevel; ++level) {
        const float size = leaf * static_cast<float>(1U << level);
        const float diag = std::sqrt(2.0F * size * size + (_maxY - _minY) * (_maxY - _minY));
        const float morphStart = rangePrev + diagPrev + diag;
        const float range = rangePrev + (morphStart - rangePrev) / config::MORPH_RATIO;
        _lodMorphStart[level] = morphStart * config::VIS_SAFETY;
        _lodRange[level] = range * config::VIS_SAFETY;
        _lodMorphEnd[level] = range * config::VIS_SAFETY;
        rangePrev = range;
        diagPrev = diag;
    }
}

void Quadtree::nodeHeightRange(uint32_t level, uint32_t ix, uint32_t iz, float &minY, float &maxY) const {
    minY = _minY;
    maxY = _maxY;
    if (_heightRanges.empty() || _sectorX >= _sectorsX || _sectorZ >= _sectorsZ ||
        level > _maxLevel) {
        return;
    }

    const uint32_t nodesPerSide = 1U << (_maxLevel - level);
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
    traverse(_maxLevel, 0, 0);
    return _selected;
}

void Quadtree::traverse(uint32_t level, uint32_t ix, uint32_t iz) {
    const float size = _sectorSize / static_cast<float>(1U << (_maxLevel - level));
    float minY = _minY;
    float maxY = _maxY;
    nodeHeightRange(level, ix, iz, minY, maxY);
    const float x = static_cast<float>(ix) * size - _sectorSize * 0.5F + _sectorOrigin.x;
    const float z = static_cast<float>(iz) * size - _sectorSize * 0.5F + _sectorOrigin.z;
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

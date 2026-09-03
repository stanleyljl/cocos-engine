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
    if (_heightmap.empty()) {
        return;
    }

    const uint32_t mapSize = config::DEMO_HM_SIZE;
    const uint32_t nodesPerSector = 1U << (_maxLevel - level);
    const uint32_t globalX = _sectorX * nodesPerSector + ix;
    const uint32_t globalZ = _sectorZ * nodesPerSector + iz;
    const float worldWidth = _sectorSize * static_cast<float>(_sectorsX);
    const float worldDepth = _sectorSize * static_cast<float>(_sectorsZ);
    const uint32_t x0 = static_cast<uint32_t>(std::max(0.0F, std::floor(static_cast<float>(globalX) * _sectorSize / worldWidth * mapSize)));
    const uint32_t z0 = static_cast<uint32_t>(std::max(0.0F, std::floor(static_cast<float>(globalZ) * _sectorSize / worldDepth * mapSize)));
    const uint32_t x1 = std::min(mapSize - 1U, static_cast<uint32_t>(std::ceil(static_cast<float>(globalX + 1U) * _sectorSize / worldWidth * mapSize)));
    const uint32_t z1 = std::min(mapSize - 1U, static_cast<uint32_t>(std::ceil(static_cast<float>(globalZ + 1U) * _sectorSize / worldDepth * mapSize)));

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

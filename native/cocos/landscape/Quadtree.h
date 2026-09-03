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

#include <cstddef>

#include "base/Ptr.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeConfig.h"
#include "math/Vec3.h"

namespace cc {
namespace geometry {
class AABB;
class Frustum;
} // namespace geometry

namespace landscape {

class Quadtree {
public:
    Quadtree();
    ~Quadtree();

    void setConfig(float sectorSize, uint32_t maxLevel, float minY, float maxY);
    void setProceduralWorld(uint32_t sectorsX, uint32_t sectorsZ);

    const ccstd::vector<QuadNode> &select(const Vec3 &camPos, const geometry::Frustum &frustum,
                                          const Vec3 &sectorOrigin, uint32_t sectorX, uint32_t sectorZ);

    const ccstd::vector<float> &lodMorphStart() const { return _lodMorphStart; }
    const ccstd::vector<float> &lodMorphEnd() const { return _lodMorphEnd; }
    const ccstd::vector<float> &proceduralHeightmap() const { return _heightmap; }
    uint32_t proceduralHeightmapSize() const { return config::DEMO_HM_SIZE; }
    float proceduralHeightAt(float x, float z) const;

private:
    struct HeightRange {
        float minY{config::TERRAIN_MIN_Y};
        float maxY{config::TERRAIN_MAX_Y};
    };

    void computeRanges();
    void buildHeightRanges();
    void traverse(uint32_t level, uint32_t ix, uint32_t iz);
    void nodeHeightRange(uint32_t level, uint32_t ix, uint32_t iz, float &minY, float &maxY) const;
    void sampleHeightRange(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                           uint32_t ix, uint32_t iz, float &minY, float &maxY) const;
    size_t nodeRangeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                          uint32_t ix, uint32_t iz) const;

    float _sectorSize{config::SECTOR_SIZE};
    uint32_t _maxLevel{config::MAX_LEVEL};
    float _minY{config::TERRAIN_MIN_Y};
    float _maxY{config::TERRAIN_MAX_Y};
    uint32_t _sectorsX{config::DEMO_SECTORS};
    uint32_t _sectorsZ{config::DEMO_SECTORS};

    Vec3 _camPos;
    Vec3 _sectorOrigin;
    uint32_t _sectorX{0};
    uint32_t _sectorZ{0};
    const geometry::Frustum *_frustum{nullptr};
    IntrusivePtr<geometry::AABB> _box;
    ccstd::vector<QuadNode> _selected;
    ccstd::vector<float> _heightmap;
    ccstd::vector<size_t> _levelOffsets;
    ccstd::vector<HeightRange> _heightRanges;
    size_t _nodesPerSector{0};
    ccstd::vector<float> _lodRange;
    ccstd::vector<float> _lodMorphStart;
    ccstd::vector<float> _lodMorphEnd;
};

} // namespace landscape
} // namespace cc

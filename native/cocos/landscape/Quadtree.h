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

class LandscapeAsset;

class Quadtree {
public:
    Quadtree();
    ~Quadtree();

    // Copies the metadata needed for selection; does not load or retain the asset.
    bool init(const LandscapeAsset &asset);

    const ccstd::vector<QuadNode> &select(const Vec3 &camPos, const geometry::Frustum &frustum,
                                          const Vec3 &sectorOrigin, uint32_t sectorX, uint32_t sectorZ);

    const ccstd::vector<float> &lodMorphStart() const { return _lodMorphStart; }
    const ccstd::vector<float> &lodMorphEnd() const { return _lodMorphEnd; }
    bool visibilityDistanceTooSmall() const { return _visibilityDistanceTooSmall; }
private:
    struct HeightRange {
        float minY{0.0F};
        float maxY{0.0F};
    };

    void computeRanges();
    bool traverse(uint32_t level, uint32_t ix, uint32_t iz);
    void nodeHeightRange(uint32_t level, uint32_t ix, uint32_t iz, float &minY, float &maxY) const;
    size_t nodeRangeIndex(uint32_t sectorX, uint32_t sectorZ, uint32_t level,
                          uint32_t ix, uint32_t iz) const;

    LandscapeData _data;

    Vec3 _camPos;
    Vec3 _sectorOrigin;
    uint32_t _sectorX{0};
    uint32_t _sectorZ{0};
    const geometry::Frustum *_frustum{nullptr};
    IntrusivePtr<geometry::AABB> _box;
    ccstd::vector<QuadNode> _selected;
    ccstd::vector<size_t> _levelOffsets;
    ccstd::vector<HeightRange> _heightRanges;
    size_t _nodesPerSector{0};
    ccstd::vector<float> _lodRange;
    ccstd::vector<float> _lodMorphStart;
    ccstd::vector<float> _lodMorphEnd;
    bool _visibilityDistanceTooSmall{false};
};

} // namespace landscape
} // namespace cc

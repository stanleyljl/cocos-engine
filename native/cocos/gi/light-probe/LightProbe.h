
/****************************************************************************
 Copyright (c) 2022 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#pragma once

#include "Delaunay.h"
#include "SH.h"
#include "base/Macros.h"
#include "base/RefCounted.h"
#include "base/std/container/vector.h"
#include "math/Vec3.h"
#include "math/Vec4.h"

namespace cc {
namespace gi {

class LightProbeInfo;

class LightProbeData {
public:
    LightProbeData() = default;

    void getInterpolationSHCoefficients(const Vec3 &position, int32_t &tetIndex, ccstd::vector<Vec3> &coefficients) const;
    Vec4 getInterpolationWeights(const Vec3 &position, int32_t &tetIndex) const;

private:
    Vec4 getBarycentricCoord(const Vec3 &position, const Tetrahedron &tetrahedron) const;
    Vec4 getTetrahedronBarycentricCoord(const Vec3 &position, const Tetrahedron &tetrahedron) const;
    Vec4 getOuterCellBarycentricCoord(const Vec3 &position, const Tetrahedron &tetrahedron) const;

    ccstd::vector<Vertex> _probes;
    ccstd::vector<Tetrahedron> _tetrahedrons;
};

class LightProbe final {
public:
    LightProbe() = default;
    ~LightProbe() = default;

    void initialize(LightProbeInfo *info);

    inline void setEnabled(bool val) { _enabled = val; }
    inline bool isEnabled() const { return _enabled; }

    inline void setQuality(SHQuality quality) { _quality = quality; }
    inline SHQuality getQuality() const { return _quality; }

    inline void setShowProbe(bool val) { _showProbe = val; }
    inline bool isShowProbe() const { return _showProbe; }

    inline void setShowWireframe(bool val) { _showWireframe = val; }
    inline bool isShowWireframe() const { return _showWireframe; }

    inline void setShowConvex(bool val) { _showConvex = val; }
    inline bool isShowConvex() const { return _showConvex; }

    inline void setReduceRinging(float val) { _reduceRinging = val; }
    inline float getReduceRinging() const { return _reduceRinging; }

    inline void setData(const LightProbeData &data) { _data = data; }
    inline const LightProbeData &getData() const { return _data; }

protected:
    bool _enabled{true};
    SHQuality _quality{SHQuality::Normal};
    bool _showProbe{true};
    bool _showWireframe{true};
    bool _showConvex{false};
    float _reduceRinging{0.0F};
    LightProbeData _data;

    CC_DISALLOW_COPY_MOVE_ASSIGN(LightProbe);
};

class LightProbeInfo : public RefCounted {
public:
    LightProbeInfo() = default;
    ~LightProbeInfo() override = default;

    void activate(LightProbe *resource);

    inline void setEnabled(bool val) {
        _enabled = val;
        if (_resource) {
            _resource->setEnabled(val);
        }
    }
    inline bool isEnabled() const { return _enabled; }

    inline void setQuality(SHQuality val) {
        _quality = val;
        if (_resource) {
            _resource->setQuality(val);
        }
    }

    inline SHQuality getQuality() const { return _quality; }

    inline void setShowProbe(bool val) {
        _showProbe = val;
        if (_resource) {
            _resource->setShowProbe(val);
        }
    }
    inline bool isShowProbe() const { return _showProbe; }

    inline void setShowWireframe(bool val) {
        _showWireframe = val;
        if (_resource) {
            _resource->setShowWireframe(val);
        }
    }
    inline bool isShowWireframe() const { return _showWireframe; }

    inline void setShowConvex(bool val) {
        _showConvex = val;
        if (_resource) {
            _resource->setShowConvex(val);
        }
    }
    inline bool isShowConvex() const { return _showConvex; }

    inline void setReduceRinging(float val) {
        _reduceRinging = val;
        if (_resource) {
            _resource->setReduceRinging(val);
        }
    }
    inline float getReduceRinging() const { return _reduceRinging; }

    inline void setData(const LightProbeData &data) {
        _data = data;
        if (_resource) {
            _resource->setData(data);
        }
    }

    inline const LightProbeData &getData() const { return _data; }

private:
    LightProbe *_resource{nullptr};

    bool _enabled{true};
    SHQuality _quality{SHQuality::Normal};
    bool _showProbe{true};
    bool _showWireframe{true};
    bool _showConvex{false};
    float _reduceRinging{0.0F};
    LightProbeData _data;
};

} // namespace gi
} // namespace cc

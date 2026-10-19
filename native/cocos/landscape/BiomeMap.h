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

#include "base/Ptr.h"
#include "math/Vec4.h"

namespace cc {
class Material;
class Texture2D;
namespace gfx {
class Sampler;
class Texture;
}
namespace landscape {

// World-aligned biome inputs shared by terrain shading. Color is the first
// input; spatial storage and shader bindings stay separate from VT residency.
// RGB is raw UNORM gain data (128 = neutral); Alpha masks regional influence.
class BiomeMap {
public:
    BiomeMap();
    ~BiomeMap();
    BiomeMap(const BiomeMap &) = delete;
    BiomeMap &operator=(const BiomeMap &) = delete;

    bool init(Material *material, float width, float depth,
              gfx::Texture *fallback, gfx::Sampler *sampler);
    void destroy();
    // Return whether cached terrain shading needs to be recomposed.
    bool setColorMap(Texture2D *texture);
    bool setColorStrength(float strength);

private:
    void apply();

    IntrusivePtr<Material> _material;
    IntrusivePtr<Texture2D> _colorMap;
    IntrusivePtr<gfx::Texture> _fallback;
    gfx::Sampler *_sampler{nullptr}; // device-owned
    float _colorStrength{0.1F};
    Vec4 _colorParams; // inverse landscape width/depth, effective strength, unused
};

} // namespace landscape
} // namespace cc

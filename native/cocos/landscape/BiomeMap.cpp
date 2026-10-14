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

#include "landscape/BiomeMap.h"

#include <algorithm>

#include "base/Log.h"
#include "core/assets/Material.h"
#include "core/assets/Texture2D.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/Pass.h"

namespace cc {
namespace landscape {

BiomeMap::BiomeMap() = default;
BiomeMap::~BiomeMap() = default;

bool BiomeMap::init(Material *material, float width, float depth,
                    gfx::Texture *fallback, gfx::Sampler *sampler) {
    if (!material || !material->getPasses() || material->getPasses()->empty() ||
        width <= 0.0F || depth <= 0.0F || !fallback || !sampler) {
        return false;
    }
    auto *pass = material->getPasses()->front().get();
    if (pass->getHandle("globalColorMap") == 0U || pass->getHandle("globalColorParams") == 0U) {
        CC_LOG_WARNING("[Landscape] VT effect is missing biome color bindings; rebuild project assets");
        return false;
    }
    _material = material;
    _fallback = fallback;
    _sampler = sampler;
    _colorParams = Vec4{1.0F / width, 1.0F / depth, 0.0F, 0.0F};
    apply();
    return true;
}

void BiomeMap::destroy() {
    _material = nullptr;
    _colorMap = nullptr;
    _fallback = nullptr;
    _sampler = nullptr;
    _colorStrength = 0.1F;
    _colorParams = Vec4{};
}

bool BiomeMap::setColorMap(Texture2D *texture) {
    _colorMap = texture;
    if (!_material) {
        return false;
    }
    // Rebinding the same asset also supports replacement of its GPU texture.
    apply();
    return true;
}

bool BiomeMap::setColorStrength(float strength) {
    strength = std::clamp(strength, 0.0F, 1.0F);
    if (_colorStrength == strength) {
        return false;
    }
    _colorStrength = strength;
    if (!_material) {
        return false;
    }
    const float previous = _colorParams.z;
    apply();
    return previous != _colorParams.z;
}

void BiomeMap::apply() {
    auto *texture = _colorMap ? _colorMap->getGFXTexture() : nullptr;
    _colorParams.z = texture ? _colorStrength : 0.0F;
    _material->setPropertyGFXTexture("globalColorMap", texture ? texture : _fallback.get());
    _material->setPropertyVec4("globalColorParams", _colorParams);
    auto *pass = _material->getPasses()->front().get();
    const auto binding = scene::Pass::getBindingFromHandle(pass->getHandle("globalColorMap"));
    pass->bindSampler(binding, _sampler);
}

} // namespace landscape
} // namespace cc

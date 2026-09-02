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
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {
namespace gfx {
class Device;
class Texture;
class Sampler;
} // namespace gfx

namespace landscape {

/**
 * The terrain material library: one RGBA8 TEX2D_ARRAY holding every material
 * layer's albedo (indexed by the splat map's material ids, up to 32 arbitrary
 * layers — no fixed semantics). Layers are loaded from albedo PNGs (all the same
 * resolution) and bound to the VT-compose shader as `sampler2DArray materialAlbedo`.
 */
class MaterialLibrary {
public:
    MaterialLibrary();
    ~MaterialLibrary();

    // paths: albedo image files, one per layer (layer id = index). All must share
    // one resolution. Extra layers past config::MATERIAL_LIBRARY_MAX are dropped.
    bool init(gfx::Device *device, const ccstd::vector<ccstd::string> &paths);
    void destroy();

    inline bool valid() const { return _array != nullptr; }
    inline gfx::Texture *array() const { return _array; }
    inline gfx::Sampler *sampler() const { return _sampler; }
    inline uint32_t layerCount() const { return _layerCount; }

private:
    gfx::Device *_device{nullptr};    // weak; owned by Root
    IntrusivePtr<gfx::Texture> _array; // RGBA8 TEX2D_ARRAY
    gfx::Sampler *_sampler{nullptr};   // weak; cached by device
    uint32_t _layerCount{0};
};

} // namespace landscape
} // namespace cc


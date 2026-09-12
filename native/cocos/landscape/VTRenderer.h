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

#include "core/Root.h"
#include "landscape/VirtualTexture.h"

namespace cc {
class Material;
class RenderingSubMesh;
namespace landscape {
class LandscapeAsset;
class MaterialLibrary;
class TilePagePool;

// Owns VT pass geometry, material, instance commands and render scheduling.
// VirtualTexture owns the independent storage/residency layer below it.
class VTRenderer {
public:
    VTRenderer();
    ~VTRenderer();
    VTRenderer(const VTRenderer &) = delete;
    VTRenderer &operator=(const VTRenderer &) = delete;

    bool init(const LandscapeAsset &asset, const TilePagePool &tiles, const MaterialLibrary &materials);
    void destroy();
    VirtualTexture &texture() { return _texture; }
    const VirtualTexture &texture() const { return _texture; }
    bool valid() const;
    void render();

private:
    VirtualTexture _texture;
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _material;
    IntrusivePtr<gfx::Buffer> _instances;
    IntrusivePtr<gfx::InputAssembler> _inputAssembler;
    IntrusivePtr<gfx::CommandBuffer> _commands;
    IntrusivePtr<gfx::PipelineState> _pipelineState;
    IntrusivePtr<gfx::RenderPass> _initialPass;
    ccstd::vector<uint32_t> _dirtySlots;
    ccstd::vector<float> _instanceData;
    Root::BeforeRender::EventID _beforeRender;
    bool _subscribed{false};
    bool _needsClear{true};
};
} // namespace landscape
} // namespace cc

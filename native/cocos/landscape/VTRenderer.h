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
#include "landscape/Landscape.h"
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

    bool init(const LandscapeAsset &asset, TilePagePool &tiles, const MaterialLibrary &materials);
    void destroy();
    VirtualTexture &texture() { return _texture; }
    const VirtualTexture &texture() const { return _texture; }
    bool valid() const;
    bool sourcesReady() const { return (!_debugData.cliffEnabled && _cliff.params.w <= 0.0F) || _cliff.ready; }
    bool isBakeNormalEnabled() const { return _debugData.bakeNormalEnabled; }
    void render();
    void setFrozen(bool frozen);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setHeightBlendEnabled(bool enabled);
    void setBakeNormalEnabled(bool enabled);
    void setCliffEnabled(bool enabled);
    void syncCliffSources(TilePagePool &tiles);

private:
    bool initComposeResources(const LandscapeAsset &asset, TilePagePool &tiles,
                              const MaterialLibrary &materials, gfx::Device *device);
    bool initComposeMaterial(const LandscapeAsset &asset, TilePagePool &tiles,
                             const MaterialLibrary &materials, gfx::Device *device);
    bool initDecalResources(const LandscapeAsset &asset, const MaterialLibrary &materials, gfx::Device *device);
    bool initNormalSourceTable(TilePagePool &tiles, gfx::Device *device);
    bool initCliffReference(const LandscapeAsset &asset, TilePagePool &tiles, gfx::Device *device);
    void bindComposeTexture(const char *name, gfx::Texture *texture, gfx::Sampler *sampler);
    bool initDrawResources(gfx::Device *device);
    bool initRootPages(const LandscapeData &data, TilePagePool &tiles);
    void buildPageBatch();
    uint32_t collectPageDecals(uint32_t slot, const Vec4 &region);
    void uploadSourceTables();
    void submitPageBatch();
    void recordPagePass(Material *material, gfx::PipelineState *pipelineState,
                        gfx::Framebuffer *framebuffer, uint32_t atlasSize);
    void invalidateComposedPages();

    // Exactly the three vec4 attributes consumed by the compose/mip shaders.
    struct PageInstance {
        Vec4 atlas; // physical slot, decal count, unused ZW
        Vec4 region;
        Vec4 splatSource;
    };
    static_assert(sizeof(PageInstance) == 12 * sizeof(float), "VT instance layout must match shader attributes");

    // Each mip has its own descriptors: recording a later mip must not overwrite
    // the preceding pass's source texture bindings.
    struct MipPass {
        IntrusivePtr<Material> material;
        IntrusivePtr<gfx::PipelineState> pipelineState;
    };

    // Applied compose state; deferred requests remain in LandscapeRenderer.
    LandscapeDebugData _debugData;
    struct CliffReference {
        bool ready{false};
        uint32_t level{0};
        uint32_t columns{0};
        uint32_t rows{0};
        Vec4 params; // shader state, height scale/bias, optional material ID + 1
        LandscapeData data;
        IntrusivePtr<gfx::Texture> texture;
        ccstd::vector<Vec4> sources;
    } _cliff;
    VirtualTexture _texture;
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _material;
    std::array<MipPass, config::VT_MIP_LEVELS - 1> _mipPasses;
    IntrusivePtr<gfx::Buffer> _instances;
    IntrusivePtr<gfx::Texture> _normalSources;
    ccstd::vector<Vec4> _normalSourceData;
    IntrusivePtr<gfx::Texture> _decalIndices;
    ccstd::vector<Vec4> _decalRegions;
    ccstd::vector<uint8_t> _decalIndexData;
    IntrusivePtr<gfx::InputAssembler> _inputAssembler;
    IntrusivePtr<gfx::CommandBuffer> _commands;
    IntrusivePtr<gfx::PipelineState> _pipelineState;
    IntrusivePtr<gfx::RenderPass> _initialPass;
    ccstd::vector<uint32_t> _dirtySlots;
    ccstd::vector<PageInstance> _pageInstances;
    bool _needsClear{true};
    bool _pendingInvalidation{false};
    bool _hasGlobalColorMap{false};
    IntrusivePtr<Texture2D> _globalColorMap;
    IntrusivePtr<gfx::Texture> _fallbackGlobalColorMap;
    gfx::Sampler *_globalColorSampler{nullptr}; // cached by device
    float _globalColorStrength{0.0F};
    Vec4 _globalColorParams;
};
} // namespace landscape
} // namespace cc

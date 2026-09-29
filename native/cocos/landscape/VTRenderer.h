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
class DecalRenderer;
class LandscapeAsset;
class MaterialLibrary;
class TilePagePool;
class TilePageResolver;

// GPU half of VT: owns the atlas, composition passes and source bindings.
// VirtualTexture is the CPU-only page state machine. Geometry submits demands,
// then resolves sources, renders once, and binds published page mappings.
class VTRenderer {
public:
    VTRenderer();
    ~VTRenderer();
    VTRenderer(const VTRenderer &) = delete;
    VTRenderer &operator=(const VTRenderer &) = delete;

    bool init(const LandscapeAsset &asset, TilePagePool &tiles, const MaterialLibrary &materials, DecalRenderer &decals);
    void destroy();
    VirtualTexture &pages() { return _pages; }
    const VirtualTexture &pages() const { return _pages; }
    RenderTexture *atlas() const { return _atlas.get(); }
    gfx::Texture *albedo() const;
    gfx::Texture *normalRoughnessAO() const;
    gfx::Sampler *sampler() const { return _sampler; }
    Vec4 mapping(VTPageAddress address) const;
    void resolveSources(TilePagePool &tiles, TilePageResolver &resolver);
    bool valid() const;
    bool sourcesReady() const { return (!_debugData.cliffEnabled && _cliff.params.w <= 0.0F) || _cliff.ready; }
    bool isBakeNormalEnabled() const { return _debugData.bakeNormalEnabled; }
    void render(uint32_t maxUpdates = config::VT_PAGE_UPDATE_BUDGET);
    void setFrozen(bool frozen);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setHeightBlendEnabled(bool enabled);
    void setBakeNormalEnabled(bool enabled);
    void setCliffEnabled(bool enabled);

private:
    friend struct LandscapePagingTestAccess;

    // A fixed, bounded reference grid keeps projection height/axis selection independent
    // of camera distance and CDLOD morph. Reuse at most a quarter of the source pool.
    static uint32_t cliffReferenceLevel(const LandscapeData &data) {
        uint32_t level = data.minTileLevel;
        while (level < data.maxLevel &&
               static_cast<uint64_t>(data.sectorsX) * data.sectorsZ *
                   (1ULL << (2U * (data.maxLevel - level))) > config::PAGE_POOL_LAYERS / 4U) ++level;
        return level;
    }

    bool initAtlas(gfx::Device *device);
    gfx::Framebuffer *framebuffer() const;
    gfx::RenderPass *renderPass() const;
    void syncCliffSources(TilePagePool &tiles);
    bool initComposeMaterial(const LandscapeAsset &asset, TilePagePool &tiles,
                             const MaterialLibrary &materials, gfx::Device *device);
    bool initNormalSourceTable(TilePagePool &tiles, gfx::Device *device);
    bool initCliffReference(const LandscapeAsset &asset, TilePagePool &tiles, gfx::Device *device);
    void bindComposeTexture(const char *name, gfx::Texture *texture, gfx::Sampler *sampler);
    bool initPageDrawResources(gfx::Device *device);
    bool initMipPasses(gfx::Device *device);
    bool initCommandResources(gfx::Device *device);
    void buildPageBatch();
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
        IntrusivePtr<gfx::Texture> albedo;
        IntrusivePtr<gfx::Texture> normal;
        IntrusivePtr<gfx::Texture> sourceAlbedo;
        IntrusivePtr<gfx::Texture> sourceNormal;
        IntrusivePtr<gfx::Framebuffer> framebuffer;
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
    VirtualTexture _pages;
    IntrusivePtr<RenderTexture> _atlas;
    gfx::Sampler *_sampler{nullptr}; // device-owned
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _material;
    std::array<MipPass, config::VT_MIP_LEVELS - 1> _mipPasses;
    IntrusivePtr<gfx::Buffer> _instances;
    IntrusivePtr<gfx::Texture> _normalSources;
    ccstd::vector<Vec4> _normalSourceData;
    DecalRenderer *_decals{nullptr}; // borrowed; owner destroys VT first
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

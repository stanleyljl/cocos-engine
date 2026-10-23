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

#include <array>
#include "base/Ptr.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeData.h"
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXDef-common.h"

namespace cc {
class Material;
class Texture2D;
class RenderTexture;
class RenderingSubMesh;
namespace landscape {
class DecalRenderer;
class LandscapeAsset;
class MaterialLibrary;
class SourceDataManager;
struct VTPageSources;

// Direct vertex-buffer layout: a_vtPage, a_vtRegion and a_vtSource (RGBA32F).
struct VTPageInstance {
    Vec4 pageInfo; // X: physical VT slot; Y: overlapping decal count (including gutters); ZW: unused, zero.
    Vec4 region;
    Vec4 splatSource;
    uint32_t physicalSlot() const { return static_cast<uint32_t>(pageInfo.x); }
};

// Owns composition inputs, material and draw resources. Page selection and
// submission belong to VTRenderer; this stage only prepares inputs and records draws.
class VTPageComposer {
public:
    VTPageComposer();
    ~VTPageComposer();
    VTPageComposer(const VTPageComposer &) = delete;
    VTPageComposer &operator=(const VTPageComposer &) = delete;
    bool init(const LandscapeAsset &asset, SourceDataManager &sourceData, const MaterialLibrary &materials, DecalRenderer &decals, bool compressed);
    RenderTexture *target() const { return _compositionTarget.get(); }
    uint32_t size() const;
    gfx::InputAssembler *inputAssembler() const { return _inputAssembler.get(); }
    gfx::RenderPass *renderPass(bool initialize = false) const;
    void record(gfx::CommandBuffer &commands, const VTPageInstance *instances, uint32_t count, bool initialize);
    Material *material() const { return _composeMaterial.get(); }
    bool prepareInputs(uint32_t slot, LandscapeRegion region, const VTPageSources &sources, const SourceDataManager &sourceData, VTPageInstance &instance);
    void uploadInputs();
    void setGlobalColorMap(Texture2D *texture);
    // Return whether cached page pixels need recomposition.
    bool setGlobalColorStrength(float strength);
    bool setHeightBlendEnabled(bool enabled);

private:
    bool initTarget(gfx::Device *device, bool compressed);
    bool initDrawResources(gfx::Device *device);
    bool initRenderPass(gfx::Device *device, bool compressed);
    bool initMaterial(const LandscapeAsset &asset, SourceDataManager &sourceData, const MaterialLibrary &materials, gfx::Device *device, bool compressed, uint32_t targetSize);
    bool initSourceTable(SourceDataManager &sourceData, gfx::Device *device);
    void bindTexture(const char *name, gfx::Texture *texture, gfx::Sampler *sampler);
    void applyGlobalColorMap();

    // A full RGBA atlas, or a scratch page reused by the compressed path.
    IntrusivePtr<RenderTexture> _compositionTarget;
    IntrusivePtr<RenderingSubMesh> _pageMesh;
    IntrusivePtr<gfx::Buffer> _instanceBuffer;
    IntrusivePtr<gfx::InputAssembler> _inputAssembler;
    IntrusivePtr<gfx::PipelineState> _composePipeline;
    IntrusivePtr<gfx::RenderPass> _initialPass;
    IntrusivePtr<Material> _composeMaterial;
    IntrusivePtr<gfx::Texture> _normalSourceTable;
    using NormalSourceRow = std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT>;
    static_assert(sizeof(NormalSourceRow) == config::VT_NORMAL_SOURCE_COUNT * 4 * sizeof(float), "Normal-source rows must match RGBA32F texels");
    ccstd::vector<NormalSourceRow> _normalSourceRows;
    DecalRenderer *_decals{nullptr}; // borrowed; owner destroys VT first
    IntrusivePtr<Texture2D> _globalColorMap;
    IntrusivePtr<gfx::Texture> _fallbackGlobalColorMap;
    gfx::Sampler *_globalColorSampler{nullptr}; // device-owned
    float _globalColorStrength{0.1F};
    float _effectiveGlobalColorStrength{0.0F};
    float _inverseWidth{0.0F};
    float _inverseDepth{0.0F};
};

// Builds mips from completed page pixels; it never re-evaluates materials.
class VTPageMipGenerator {
public:
    VTPageMipGenerator();
    ~VTPageMipGenerator();
    VTPageMipGenerator(const VTPageMipGenerator &) = delete;
    VTPageMipGenerator &operator=(const VTPageMipGenerator &) = delete;
    bool init(const VTPageComposer &composer, bool compressed);
    void record(gfx::CommandBuffer &commands, const VTPageComposer &composer, uint32_t count, bool initialize);

private:
    // Descriptors are independent per mip: later recording cannot overwrite
    // the preceding pass's source views. Prefix views exclude the destination.
    struct MipPass {
        IntrusivePtr<Material> material;
        IntrusivePtr<gfx::PipelineState> pipelineState;
        IntrusivePtr<gfx::Texture> albedo;
        IntrusivePtr<gfx::Texture> normal;
        IntrusivePtr<gfx::Texture> sourceAlbedo;
        IntrusivePtr<gfx::Texture> sourceNormal;
        IntrusivePtr<gfx::Framebuffer> framebuffer;
    };
    std::array<MipPass, config::VT_MIP_LEVELS - 1> _mipPasses;
};

// Encodes each completed scratch page and copies its mips to the ASTC atlas.
class VTPageCompressor {
public:
    VTPageCompressor();
    ~VTPageCompressor();
    VTPageCompressor(const VTPageCompressor &) = delete;
    VTPageCompressor &operator=(const VTPageCompressor &) = delete;
    static bool supported();
    bool init(gfx::Device *device, RenderTexture &composition);
    Texture2D *atlas() const;
    gfx::Texture *albedo() const;
    gfx::Texture *normalRoughnessAO() const;
    void begin(gfx::CommandBuffer &commands, bool discardContents);
    void record(gfx::CommandBuffer &commands, uint32_t slot);
    void end(gfx::CommandBuffer &commands);

private:
    void transition(gfx::CommandBuffer &commands, gfx::AccessFlags previous, gfx::AccessFlags next, bool discardContents);
    std::array<IntrusivePtr<Texture2D>, 2> _atlas;
    IntrusivePtr<gfx::Buffer> _blocks;
    IntrusivePtr<gfx::Shader> _shader;
    IntrusivePtr<gfx::DescriptorSetLayout> _layout;
    IntrusivePtr<gfx::PipelineLayout> _pipelineLayout;
    struct Mip {
        IntrusivePtr<gfx::Buffer> constants;
        IntrusivePtr<gfx::DescriptorSet> bindings;
    };
    std::array<Mip, config::VT_MIP_LEVELS> _mips;
    IntrusivePtr<gfx::PipelineState> _pipeline;
};

} // namespace landscape
} // namespace cc

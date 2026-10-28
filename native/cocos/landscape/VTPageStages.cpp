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
#include "landscape/VTPageStages.h"
#include "landscape/DecalRenderer.h"
#include "landscape/Asset.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/Residency.h"

#include <algorithm>
#include <cmath>

#include "base/Log.h"
#include "core/Root.h"
#include "base/Macros.h"
#include "core/assets/Material.h"
#include "core/assets/EffectAsset.h"
#include "core/assets/RenderTexture.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/assets/Texture2D.h"
#include "landscape/GridMesh.h"
#include "landscape/Config.h"
#include "renderer/gfx-base/GFXCommandBuffer.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDescriptorSetLayout.h"
#include "renderer/gfx-base/GFXPipelineLayout.h"
#include "renderer/gfx-base/GFXShader.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXInputAssembler.h"
#include "renderer/gfx-base/GFXPipelineState.h"
#include "renderer/gfx-base/GFXQueue.h"
#include "renderer/gfx-base/GFXRenderPass.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/Pass.h"
#include "scene/RenderWindow.h"

namespace cc::landscape {
namespace {
constexpr gfx::Format COMPRESSED_FORMAT = gfx::Format::ASTC_RGBA_4X4;
constexpr const char *COMPRESSION_PROGRAM = "vt-astc-cs";
constexpr uint32_t ASTC_BLOCK_SIDE = 4;
constexpr uint32_t ASTC_BLOCK_BYTES = 16;
// Internal projection policy; asset dimensions and VT filter support are supplied separately.
constexpr float TRIPLANAR_BLEND_SHARPNESS = 4.0F;
constexpr float TRIPLANAR_FADE_RATIO = 1.0F / 16.0F;
static_assert(CC_LANDSCAPE_CLIFF_ANGLE >= 0.0F && CC_LANDSCAPE_CLIFF_ANGLE <= 90.0F, "Cliff angle must be between 0 and 90 degrees");
const float CLIFF_NORMAL_Y = std::cos(CC_DEGREES_TO_RADIANS(CC_LANDSCAPE_CLIFF_ANGLE));
constexpr uint32_t COMPRESSION_GROUP_SIDE = 8;

void recordPagePass(gfx::CommandBuffer &commands, gfx::InputAssembler *inputAssembler, gfx::RenderPass *renderPass, scene::Pass *pass, gfx::PipelineState *pipelineState, gfx::Framebuffer *framebuffer, uint32_t atlasSize, uint32_t instanceCount) {
    const gfx::Color colors[2] = {{0, 0, 0, 1}, {0.5F, 0.5F, 1, 1}};
    commands.beginRenderPass(renderPass, framebuffer,
                               gfx::Rect{0, 0, atlasSize, atlasSize}, colors, 1.0F, 0);
    commands.bindPipelineState(pipelineState);
    commands.bindDescriptorSet(static_cast<uint32_t>(pipeline::SetIndex::MATERIAL), pass->getDescriptorSet());
    commands.bindInputAssembler(inputAssembler);
    auto draw = inputAssembler->getDrawInfo();
    draw.instanceCount = instanceCount;
    commands.draw(draw);
    commands.endRenderPass();
}
} // namespace

VTPageComposer::VTPageComposer() = default;
VTPageComposer::~VTPageComposer() {
    // GPU objects borrow mesh/material resources. Release those objects first;
    // ordinary members then release themselves. Fallback creates a new composer.
    _composePipelines = {};
    _initialPass = nullptr;
    _inputAssembler = nullptr;
    _instanceBuffer = nullptr;
    if (_pageMesh) {
        _pageMesh->destroy();
    }
    if (_compositionTarget) {
        _compositionTarget->destroy();
    }
    if (_composeMaterial) {
        _composeMaterial->destroy();
    }
}

bool VTPageComposer::init(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, DecalRenderer &decals, bool compressed) {
    CC_ASSERT(_composeMaterial == nullptr);
    _decals = &decals;
    _rootLevel = asset.data().maxLevel;
    auto *device = Root::getInstance()->getDevice();
    if (!initTarget(device, compressed) || !initMaterial(asset, residency, materials, device, compressed, size()) ||
        !_decals->bindComposeMaterial(_composeMaterial, materials) || !initSourceTable(residency, device)) {
        return false;
    }
    for (const auto &pass : *_composeMaterial->getPasses()) {
        pass->update();
    }
    _pageMesh = GridMesh::createVTQuad(device);
    return _pageMesh && initDrawResources(device) && initRenderPass(device, compressed);
}

bool VTPageComposer::prepareInputs(uint32_t slot, LandscapeRegion region, const VTPageSources &sources, const LandscapeResidency &residency, VTPageInstance &instance) {
    const auto splat = residency.resolve(sources.splat);
    if (splat.layer < 0) {
        return false;
    }
    const auto projection = residency.resolve(sources.splat.ancestor(_rootLevel));
    if (projection.layer < 0) {
        return false;
    }
    NormalSourceRow normals;
    for (size_t i = 0; i < sources.normals.size(); ++i) {
        const auto tile = residency.resolve(sources.normals[i]);
        if (tile.layer < 0) {
            return false;
        }
        normals[i] = residency.shaderParams(tile);
    }
    // All inputs are resident. Write their GPU representation only now.
    instance.region = {region.x, region.z, region.size, 0};
    instance.splatSource = residency.shaderParams(splat);
    instance.projectionSource = residency.shaderParams(projection);
    auto *pass = _composeMaterial->getPasses()->front().get();
    const bool triplanarEnabled = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("triplanarParams"))).x > 0.0F;
    const bool cliff = triplanarEnabled && residency.minimumNormalY(projection.address) < CLIFF_NORMAL_Y;
    instance.pageInfo = {static_cast<float>(slot), static_cast<float>(_decals->collectPageDecals(slot, instance.region)), cliff ? 1.0F : 0.0F, 0};
    _normalSourceRows[slot] = normals;
    return true;
}

void VTPageComposer::uploadInputs() {
    _decals->updateComposeMaterial(_composeMaterial);
    _decals->uploadPageDecals();
    gfx::BufferTextureCopy indexCopy;
    indexCopy.texExtent = {config::VT_NORMAL_SOURCE_COUNT, config::VT_PAGE_COUNT, 1};
    const uint8_t *normalBytes[]{reinterpret_cast<const uint8_t *>(_normalSourceRows.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(normalBytes, _normalSourceTable, &indexCopy, 1);

    for (const auto &pass : *_composeMaterial->getPasses()) {
        pass->update();
    }
}

void VTPageComposer::bindTexture(const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
    _composeMaterial->setPropertyGFXTexture(name, texture);
    for (const auto &pass : *_composeMaterial->getPasses()) {
        pass->bindSampler(scene::Pass::getBindingFromHandle(pass->getHandle(name)), sampler);
    }
}

bool VTPageComposer::initMaterial(const LandscapeAsset &asset, LandscapeResidency &residency, const MaterialLibrary &materials, gfx::Device *device, bool compressed, uint32_t targetSize) {
    const auto &data = asset.data();
    _composeMaterial = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"landscape/builtin-landscape-vt-compose"};
    info.defines = IMaterialInfo::DefinesType{MacroRecord{{"LANDSCAPE_VT_COMPACT", compressed}, {"LANDSCAPE_HEIGHT_UNORM", residency.heightArray()->getFormat() == gfx::Format::R16_UNORM}}};
    _composeMaterial->initialize(info);
    if (!_composeMaterial->getPasses() || _composeMaterial->getPasses()->size() != _composePipelines.size()) {
        return false;
    }
    const char *requiredProperties[] = {
        "vtLayout",
        "splatmap",
        "albedoHeightMap",
        "normalRoughnessAOMap",
        "materialParams",
        "globalColorMap",
        "globalColorParams",
        "heightBlendParams",
        "terrainNormalMap",
        "normalSourceMap",
        "terrainHeightMap",
        "terrainHeightParams",
        "triplanarParams",
    };
    const auto &pass = _composeMaterial->getPasses()->front();
    for (const char *name : requiredProperties) {
        if (pass->getHandle(name) == 0U) {
            CC_LOG_WARNING("[Landscape] VT effect is missing %s; rebuild project effects", name);
            return false;
        }
    }
    bindTexture("splatmap", residency.splatArray(), residency.splatSampler());
    bindTexture("terrainNormalMap", residency.normalArray(), residency.heightSampler());
    bindTexture("terrainHeightMap", residency.heightArray(), residency.heightSampler());
    bindTexture("albedoHeightMap", materials.albedoHeight(), materials.repeatSampler());
    bindTexture("normalRoughnessAOMap", materials.normalRoughnessAO(), materials.repeatSampler());
    if (data.width() <= 0.0F || data.depth() <= 0.0F || !materials.whiteTexture() || !materials.clampSampler()) {
        return false;
    }
    _fallbackGlobalColorMap = materials.whiteTexture();
    _globalColorSampler = materials.clampSampler();
    _inverseWidth = 1.0F / data.width();
    _inverseDepth = 1.0F / data.depth();
    applyGlobalColorMap();
    _composeMaterial->setPropertyVec4Array("materialParams", materials.materialParams());
    _composeMaterial->setPropertyVec4("terrainHeightParams", Vec4{data.heightScale, data.heightBias, data.sectorSize, 0});
    // A fixed sector-boundary band covers the largest page gutters and derivative support.
    _composeMaterial->setPropertyVec4("triplanarParams", Vec4{1.0F, static_cast<float>(config::VT_PAGE_BORDER + 2U) / config::VT_PAGE_INTERIOR, TRIPLANAR_FADE_RATIO, TRIPLANAR_BLEND_SHARPNESS});
    _composeMaterial->setPropertyVec4("vtLayout", Vec4{static_cast<float>(targetSize),
                                                static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER),
                                                device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
    return true;
}

bool VTPageComposer::initSourceTable(LandscapeResidency &residency, gfx::Device *device) {
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = gfx::Format::RGBA32F;
    info.width = config::VT_NORMAL_SOURCE_COUNT;
    info.height = config::VT_PAGE_COUNT;
    _normalSourceTable = device->createTexture(info);
    if (!_normalSourceTable) {
        return false;
    }
    _normalSourceRows.resize(config::VT_PAGE_COUNT);
    bindTexture("normalSourceMap", _normalSourceTable, residency.splatSampler());
    return true;
}

void VTPageComposer::setGlobalColorMap(Texture2D *texture) {
    _globalColorMap = texture;
    // Rebinding the same asset also supports replacement of its GPU texture.
    applyGlobalColorMap();
}

void VTPageComposer::applyGlobalColorMap() {
    auto *texture = _globalColorMap ? _globalColorMap->getGFXTexture() : nullptr;
    _effectiveGlobalColorStrength = texture ? _globalColorStrength : 0.0F;
    bindTexture("globalColorMap", texture ? texture : _fallbackGlobalColorMap.get(), _globalColorSampler);
    _composeMaterial->setPropertyVec4("globalColorParams", Vec4{_inverseWidth, _inverseDepth, _effectiveGlobalColorStrength, 0});
}

bool VTPageComposer::setHeightBlendEnabled(bool enabled) {
    auto *pass = _composeMaterial->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("heightBlendParams")));
    const float strength = enabled ? 1.0F : 0.0F;
    if (params.x == strength) {
        return false;
    }
    // Keep the effect's scale/sharpness when toggling the global blend mode.
    params.x = strength;
    _composeMaterial->setPropertyVec4("heightBlendParams", params);
    // Recomposition reserves its source tiles through the normal scheduling path.
    return true;
}

bool VTPageComposer::setTriplanarEnabled(bool enabled) {
    auto *pass = _composeMaterial->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("triplanarParams")));
    const float strength = enabled ? 1.0F : 0.0F;
    if (params.x == strength) {
        return false;
    }
    params.x = strength;
    _composeMaterial->setPropertyVec4("triplanarParams", params);
    return true;
}

bool VTPageComposer::setGlobalColorStrength(float strength) {
    const float clamped = std::clamp(strength, 0.0F, 1.0F);
    if (_globalColorStrength == clamped) {
        return false;
    }
    _globalColorStrength = clamped;
    const float previous = _effectiveGlobalColorStrength;
    applyGlobalColorMap();
    return previous != _effectiveGlobalColorStrength;
}

void VTPageComposer::record(gfx::CommandBuffer &commands, const VTPageInstance *instances, uint32_t instanceCount, bool initialize) {
    // Updating the vertex buffer is ordered after the preceding page's draws.
    const auto *barrier = Root::getInstance()->getDevice()->getGeneralBarrier(
        {gfx::AccessFlagBit::VERTEX_BUFFER, gfx::AccessFlagBit::TRANSFER_WRITE});
    commands.pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
    commands.updateBuffer(_instanceBuffer, instances, instanceCount * sizeof(VTPageInstance));
    barrier = Root::getInstance()->getDevice()->getGeneralBarrier(
        {gfx::AccessFlagBit::TRANSFER_WRITE, gfx::AccessFlagBit::VERTEX_BUFFER});
    commands.pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
    const auto variant = static_cast<uint32_t>(instances[0].pageInfo.z);
    recordPagePass(commands, _inputAssembler, renderPass(initialize), (*_composeMaterial->getPasses())[variant], _composePipelines[variant], _compositionTarget->getWindow()->getFramebuffer(), size(), instanceCount);
}

bool VTPageComposer::initTarget(gfx::Device *device, bool compressed) {
    if (device->getCapabilities().maxColorRenderTargets < 2 ||
        device->getCapabilities().maxTextureSize < config::VT_ATLAS_SIZE) {
        return false;
    }
    gfx::RenderPassInfo passInfo;
    gfx::ColorAttachment color;
    color.format = gfx::Format::RGBA8;
    color.loadOp = gfx::LoadOp::LOAD;
    color.storeOp = gfx::StoreOp::STORE;
    gfx::GeneralBarrierInfo barrier;
    barrier.prevAccesses = gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    barrier.nextAccesses = gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    if (compressed) {
        barrier.prevAccesses |= gfx::AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE;
        barrier.nextAccesses |= gfx::AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE;
    }
    color.barrier = device->getGeneralBarrier(barrier);
    passInfo.colorAttachments = {color, color};
    // No depth attachment: page rectangles never overlap.
    IRenderTextureCreateInfo info;
    info.name = "landscape-vt-atlas";
    info.width = (compressed ? config::VT_PAGE_RES : config::VT_ATLAS_SIZE);
    info.height = (compressed ? config::VT_PAGE_RES : config::VT_ATLAS_SIZE);
    info.passInfo = passInfo;
    info.colorMipLevels = config::VT_MIP_LEVELS;
    _compositionTarget = ccnew RenderTexture();
    _compositionTarget->initialize(info);
    const auto *window = _compositionTarget->getWindow();
    if (!window || !window->getFramebuffer()) {
        return false;
    }
    const auto *framebuffer = window->getFramebuffer();
    const auto &textures = framebuffer->getColorTextures();
    return textures.size() == 2 && textures[0] && textures[1] && framebuffer->getRenderPass();
}

uint32_t VTPageComposer::size() const {
    return _compositionTarget->getWidth();
}

gfx::RenderPass *VTPageComposer::renderPass(bool initialize) const {
    return initialize ? _initialPass.get() : _compositionTarget->getWindow()->getFramebuffer()->getRenderPass();
}

bool VTPageComposer::initDrawResources(gfx::Device *device) {
    constexpr uint32_t STRIDE = sizeof(VTPageInstance);
    _instanceBuffer = device->createBuffer({gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
                                       gfx::MemoryUsageBit::DEVICE, config::VT_PAGE_COUNT * STRIDE, STRIDE});
    gfx::InputAssemblerInfo iaInfo;
    iaInfo.attributes = _pageMesh->getAttributes();
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtPage", gfx::Format::RGBA32F, false, 1, true, 1});
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtRegion", gfx::Format::RGBA32F, false, 1, true, 2});
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtSource", gfx::Format::RGBA32F, false, 1, true, 3});
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtProjection", gfx::Format::RGBA32F, false, 1, true, 4});
    iaInfo.vertexBuffers = _pageMesh->getVertexBuffers();
    iaInfo.vertexBuffers.push_back(_instanceBuffer.get());
    iaInfo.indexBuffer = _pageMesh->getIndexBuffer();
    _inputAssembler = device->createInputAssembler(iaInfo);
    for (size_t i = 0; i < _composePipelines.size(); ++i) {
        const auto &pass = (*_composeMaterial->getPasses())[i];
        auto *shader = pass->getShaderVariant();
        if (!shader) {
            return false;
        }
        _composePipelines[i] = device->createPipelineState({shader, pass->getPipelineLayout(), renderPass(), {iaInfo.attributes}, *pass->getRasterizerState(), *pass->getDepthStencilState(), *pass->getBlendState(), pass->getPrimitive(), pass->getDynamicStates()});
        if (!_composePipelines[i]) {
            return false;
        }
    }
    return _instanceBuffer && _inputAssembler;
}

bool VTPageComposer::initRenderPass(gfx::Device *device, bool compressed) {
    gfx::RenderPassInfo initialInfo;
    initialInfo.colorAttachments = renderPass()->getColorAttachments();
    for (auto &color : initialInfo.colorAttachments) {
        color.loadOp = gfx::LoadOp::CLEAR;
        color.barrier = device->getGeneralBarrier({gfx::AccessFlagBit::NONE,
            compressed ? gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE | gfx::AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE
                      : gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE});
    }
    _initialPass = device->createRenderPass(initialInfo);
    return _initialPass != nullptr;
}

VTPageMipGenerator::VTPageMipGenerator() = default;
VTPageMipGenerator::~VTPageMipGenerator() {
    for (auto &mip : _mipPasses) {
        mip.pipelineState = nullptr;
        if (mip.material) {
            mip.material->destroy();
        }
        mip = MipPass{};
    }
}

bool VTPageMipGenerator::init(const VTPageComposer &composer, bool compressed) {
    auto *device = Root::getInstance()->getDevice();
    IMaterialInfo info;
    info.effectName = ccstd::string{"landscape/builtin-landscape-vt-mip"};
    info.defines = IMaterialInfo::DefinesType{MacroRecord{{"LANDSCAPE_VT_COMPACT", compressed}}};
    const auto view = [device](gfx::Texture *texture, uint32_t base, uint32_t count) {
        gfx::TextureViewInfo info;
        info.texture = texture;
        info.format = texture->getFormat();
        info.baseLevel = base;
        info.levelCount = count;
        return device->createTexture(info);
    };
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        auto &mip = _mipPasses[level - 1];
        mip.albedo = view(composer.target()->getWindow()->getColorTexture(0), level, 1);
        mip.normal = view(composer.target()->getWindow()->getColorTexture(1), level, 1);
        // Prefix views exclude the destination, preserving absolute mip indices.
        mip.sourceAlbedo = view(composer.target()->getWindow()->getColorTexture(0), 0, level);
        mip.sourceNormal = view(composer.target()->getWindow()->getColorTexture(1), 0, level);
        if (!mip.albedo || !mip.normal || !mip.sourceAlbedo || !mip.sourceNormal) {
            return false;
        }
        mip.framebuffer = device->createFramebuffer({composer.renderPass(), {mip.albedo, mip.normal}, nullptr});
        if (!mip.framebuffer) {
            return false;
        }
        // The mip effect filters completed pages without re-evaluating materials.
        auto &mipMaterial = _mipPasses[level - 1].material;
        mipMaterial = ccnew Material();
        mipMaterial->initialize(info);
        if (!mipMaterial->getPasses() || mipMaterial->getPasses()->empty()) {
            return false;
        }
        auto *mipPass = mipMaterial->getPasses()->front().get();
        if (!mipPass->getHandle("mipLayout") || !mipPass->getHandle("sourceAlbedo") || !mipPass->getHandle("sourceNormal")) {
            CC_LOG_ERROR("[Landscape] VT mip effect properties are missing; rebuild project effects");
            return false;
        }
        // Read the preceding level; the source view excludes the destination.
        mipMaterial->setPropertyGFXTexture("sourceAlbedo", mip.sourceAlbedo.get());
        mipMaterial->setPropertyGFXTexture("sourceNormal", mip.sourceNormal.get());
        mipMaterial->setPropertyVec4("mipLayout", Vec4{static_cast<float>(composer.size() >> (level - 1)),
                                                       static_cast<float>(config::VT_PAGE_RES >> (level - 1)), static_cast<float>(level - 1),
                                                       device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
        mipPass->update();
        auto *mipShader = mipPass->getShaderVariant();
        if (!mipShader) {
            return false;
        }
        _mipPasses[level - 1].pipelineState = device->createPipelineState({mipShader, mipPass->getPipelineLayout(), composer.renderPass(), {composer.inputAssembler()->getAttributes()}, *mipPass->getRasterizerState(), *mipPass->getDepthStencilState(), *mipPass->getBlendState(), mipPass->getPrimitive(), mipPass->getDynamicStates()});
        if (!_mipPasses[level - 1].pipelineState) {
            return false;
        }
    }
    return true;
}

void VTPageMipGenerator::record(gfx::CommandBuffer &commands, const VTPageComposer &composer, uint32_t count, bool initialize) {
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        const auto &mip = _mipPasses[level - 1];
        recordPagePass(commands, composer.inputAssembler(), composer.renderPass(initialize), mip.material->getPasses()->front(), mip.pipelineState, mip.framebuffer, composer.size() >> level, count);
    }
}

VTPageCompressor::VTPageCompressor() = default;
Texture2D *VTPageCompressor::atlas() const {
    return _atlas[0].get();
}
gfx::Texture *VTPageCompressor::albedo() const {
    return _atlas[0]->getGFXTexture();
}
gfx::Texture *VTPageCompressor::normalRoughnessAO() const {
    return _atlas[1]->getGFXTexture();
}

bool VTPageCompressor::supported() {
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    return false;
#else
    auto *device = Root::getInstance()->getDevice();
    const auto features = device->getFormatFeatures(COMPRESSED_FORMAT);
    return device->hasFeature(gfx::Feature::COMPUTE_SHADER) &&
           hasAllFlags(features, gfx::FormatFeature::SAMPLED_TEXTURE | gfx::FormatFeature::LINEAR_FILTER);
#endif
}

bool VTPageCompressor::init(gfx::Device *device, RenderTexture &composition) {
    for (auto &texture : _atlas) {
        texture = ccnew Texture2D();
        texture->reset({config::VT_ATLAS_SIZE, config::VT_ATLAS_SIZE, static_cast<PixelFormat>(COMPRESSED_FORMAT),
                        config::VT_MIP_LEVELS, 0, config::VT_MIP_LEVELS - 1});
        if (!texture->getGFXTexture()) {
            return false;
        }
    }
    // The legacy Material/ProgramLib path only builds graphics shaders. Create
    // this compute pipeline directly, as ClusterLightCulling does, keeping the
    // compiler-produced source in the dedicated compression effect.
    auto *effect = EffectAsset::get("landscape/builtin-landscape-vt-compress");
    if (!effect) {
        return false;
    }
    const IShaderInfo *program = nullptr;
    for (const auto &candidate : effect->getShaders()) {
        if (candidate.name.find(COMPRESSION_PROGRAM) != ccstd::string::npos) {
            program = &candidate;
            break;
        }
    }
    if (!program) {
        return false;
    }
    const auto &source = device->getGfxAPI() == gfx::API::GLES3 ? program->glsl3.compute : program->glsl4.compute;
    if (!source || source->empty()) {
        return false;
    }
    gfx::ShaderInfo shaderInfo;
    shaderInfo.name = COMPRESSION_PROGRAM;
    shaderInfo.stages = {{gfx::ShaderStageFlagBit::COMPUTE, source.value()}};
    shaderInfo.blocks = {{0, 0, "CompressionConstants", {{"compressionParams", gfx::Type::FLOAT4, 1}}, 1}};
    shaderInfo.samplerTextures = {{0, 1, "sourceAlbedo", gfx::Type::SAMPLER2D, 1},
                                  {0, 2, "sourceNormal", gfx::Type::SAMPLER2D, 1}};
    shaderInfo.buffers = {{0, 3, "CompressedBlocks", 1, gfx::MemoryAccessBit::WRITE_ONLY}};
    _shader = device->createShader(shaderInfo);
    gfx::DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings = {{0, gfx::DescriptorType::UNIFORM_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE},
                           {1, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE},
                           {2, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE},
                           {3, gfx::DescriptorType::STORAGE_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE}};
    _layout = device->createDescriptorSetLayout(layoutInfo);
    _pipelineLayout = device->createPipelineLayout({{_layout}});
    constexpr uint32_t BLOCKS_PER_SIDE = config::VT_PAGE_RES / ASTC_BLOCK_SIDE;
    constexpr uint32_t PAGE_BYTES = BLOCKS_PER_SIDE * BLOCKS_PER_SIDE * ASTC_BLOCK_BYTES;
    _blocks = device->createBuffer({gfx::BufferUsageBit::STORAGE | gfx::BufferUsageBit::TRANSFER_SRC,
                                              gfx::MemoryUsageBit::DEVICE, 2 * PAGE_BYTES, ASTC_BLOCK_BYTES});
    if (!_shader || !_blocks || !_layout || !_pipelineLayout) {
        return false;
    }
    gfx::SamplerInfo samplerInfo;
    samplerInfo.minFilter = samplerInfo.magFilter = gfx::Filter::POINT;
    samplerInfo.mipFilter = gfx::Filter::NONE;
    samplerInfo.addressU = gfx::Address::WRAP;
    samplerInfo.addressV = gfx::Address::WRAP;
    samplerInfo.addressW = gfx::Address::WRAP;
    auto *sampler = device->getSampler(samplerInfo);
    for (uint32_t level = 0; level < config::VT_MIP_LEVELS; ++level) {
        auto &uniform = _mips[level].constants;
        uniform = device->createBuffer({gfx::BufferUsageBit::UNIFORM | gfx::BufferUsageBit::TRANSFER_DST,
                                         gfx::MemoryUsageBit::DEVICE, sizeof(Vec4), sizeof(Vec4)});
        const Vec4 params{static_cast<float>(level), 0, 0, 0};
        uniform->update(&params, sizeof(params));
        auto &set = _mips[level].bindings;
        set = device->createDescriptorSet({_layout});
        set->bindBuffer(0, uniform);
        set->bindTexture(1, composition.getWindow()->getColorTexture(0));
        set->bindSampler(1, sampler);
        set->bindTexture(2, composition.getWindow()->getColorTexture(1));
        set->bindSampler(2, sampler);
        set->bindBuffer(3, _blocks);
        set->update();
    }
    gfx::PipelineStateInfo pipelineInfo;
    pipelineInfo.shader = _shader;
    pipelineInfo.pipelineLayout = _pipelineLayout;
    pipelineInfo.bindPoint = gfx::PipelineBindPoint::COMPUTE;
    _pipeline = device->createPipelineState(pipelineInfo);
    return _pipeline != nullptr;
}

void VTPageCompressor::transition(gfx::CommandBuffer &commands, gfx::AccessFlags previous, gfx::AccessFlags next, bool discardContents) {
    auto *device = Root::getInstance()->getDevice();
    gfx::TextureBarrierInfo info;
    info.prevAccesses = previous;
    info.nextAccesses = next;
    info.discardContents = discardContents;
    info.range.levelCount = config::VT_MIP_LEVELS;
    info.range.numSlices = 1;
    const gfx::TextureBarrier *barriers[] = {device->getTextureBarrier(info), device->getTextureBarrier(info)};
    // Texture2D exposes a sampling view; transfers target its underlying image.
    const gfx::Texture *textures[] = {albedo()->getViewInfo().texture, normalRoughnessAO()->getViewInfo().texture};
    commands.pipelineBarrier(nullptr, nullptr, nullptr, 0, barriers, textures, 2);
}

void VTPageCompressor::record(gfx::CommandBuffer &commands, uint32_t slot) {
    auto *device = Root::getInstance()->getDevice();
    for (uint32_t level = 0; level < config::VT_MIP_LEVELS; ++level) {
        const uint32_t size = config::VT_PAGE_RES >> level;
        const uint32_t blocks = size / ASTC_BLOCK_SIDE;
        auto *barrier = device->getGeneralBarrier({gfx::AccessFlagBit::TRANSFER_READ, gfx::AccessFlagBit::COMPUTE_SHADER_WRITE});
        commands.pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
        commands.bindPipelineState(_pipeline);
        commands.bindDescriptorSet(0, _mips[level].bindings);
        gfx::DispatchInfo dispatch;
        dispatch.groupCountX = (blocks + COMPRESSION_GROUP_SIDE - 1) / COMPRESSION_GROUP_SIDE;
        dispatch.groupCountY = (blocks + COMPRESSION_GROUP_SIDE - 1) / COMPRESSION_GROUP_SIDE;
        dispatch.groupCountZ = 1;
        commands.dispatch(dispatch);
        barrier = device->getGeneralBarrier({gfx::AccessFlagBit::COMPUTE_SHADER_WRITE, gfx::AccessFlagBit::TRANSFER_READ});
        commands.pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
        gfx::BufferTextureCopy copy;
        copy.texExtent = {size, size, 1};
        copy.texOffset = {static_cast<int32_t>((slot % config::VT_PAGES_PER_SIDE) * size),
                          static_cast<int32_t>((slot / config::VT_PAGES_PER_SIDE) * size), 0};
        copy.texSubres.mipLevel = level;
        commands.copyBufferToTexture(_blocks, albedo()->getViewInfo().texture, &copy, 1);
        copy.buffOffset = blocks * blocks * ASTC_BLOCK_BYTES;
        commands.copyBufferToTexture(_blocks, normalRoughnessAO()->getViewInfo().texture, &copy, 1);
    }
}

VTPageCompressor::~VTPageCompressor() {
    _pipeline = nullptr;
    _mips = {};
    _pipelineLayout = nullptr;
    _layout = nullptr;
    _shader = nullptr;
    _blocks = nullptr;
    for (auto &texture : _atlas) {
        if (texture) {
            texture->destroy();
        }
        texture = nullptr;
    }
}

void VTPageCompressor::begin(gfx::CommandBuffer &commands, bool discardContents) {
    const auto previous = discardContents ? gfx::AccessFlagBit::NONE : gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    transition(commands, previous, gfx::AccessFlagBit::TRANSFER_WRITE, discardContents);
}

void VTPageCompressor::end(gfx::CommandBuffer &commands) {
    transition(commands, gfx::AccessFlagBit::TRANSFER_WRITE, gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE, false);
}

} // namespace cc::landscape

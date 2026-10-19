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
#include "landscape/VTRenderer.h"

#include <algorithm>

#include "base/Log.h"
#include "base/Macros.h"
#include "core/assets/Material.h"
#include "core/assets/EffectAsset.h"
#include "core/assets/RenderTexture.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/assets/Texture2D.h"
#include "landscape/DecalRenderer.h"
#include "landscape/GridMesh.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "landscape/VirtualTexture.h"
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

namespace cc {
namespace landscape {
namespace {
constexpr gfx::Format COMPRESSED_FORMAT = gfx::Format::ASTC_RGBA_4X4;
constexpr const char *COMPRESSION_PROGRAM = "vt-astc-cs";
constexpr const char *COMPRESSION_NAME = "ASTC 4x4";
} // namespace

VTRenderer::VTRenderer() = default;
VTRenderer::~VTRenderer() { destroy(); }

bool VTRenderer::init(const LandscapeAsset &asset, TilePagePool &tiles, const MaterialLibrary &materials, DecalRenderer &decals) {
    CC_ASSERT(_material == nullptr && _atlas == nullptr);
    auto *root = Root::getInstance();
    auto *device = root ? root->getDevice() : nullptr;
    if (!device || !tiles.valid() || !materials.valid()) {
        return false;
    }
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    _compress = false;
#else
    const auto formatFeatures = device->getFormatFeatures(COMPRESSED_FORMAT);
    _compress = device->hasFeature(gfx::Feature::COMPUTE_SHADER) &&
                hasAllFlags(formatFeatures, gfx::FormatFeature::SAMPLED_TEXTURE | gfx::FormatFeature::LINEAR_FILTER);
#endif
    const auto initialize = [&]() {
        _decals = &decals;
        if (!_pages.init(asset.data()) || !initAtlas(device) ||
            !initComposeMaterial(asset, tiles, materials, device) ||
            !_decals->bindComposeMaterial(_material, materials) ||
            !initNormalSourceTable(tiles, device) || !initCliffReference(asset, tiles, device)) {
            return false;
        }
        _material->getPasses()->front()->update();
        return initPageDrawResources(device) && initMipPasses(device) &&
               (!_compress || initCompression(device)) && initCommandResources(device);
    };
    if (!initialize()) {
        const bool retryUncompressed = _compress;
        destroy();
        if (!retryUncompressed || !initialize()) {
            destroy();
            return false;
        }
        CC_LOG_WARNING("[Landscape] %s initialization failed; using RGBA8 VT atlas", COMPRESSION_NAME);
    }
    CC_LOG_INFO("[Landscape] VT atlas: %s", _compress ? COMPRESSION_NAME : "RGBA8");
    _batchSlots.reserve(config::VT_PAGE_COUNT);
    _batchInstances.reserve(config::VT_PAGE_COUNT);
    return true;
}

bool VTRenderer::initAtlas(gfx::Device *device) {
    if (device->getCapabilities().maxColorRenderTargets < 2 ||
        device->getCapabilities().maxTextureSize < config::VT_ATLAS_SIZE) {
        return false;
    }
    gfx::SamplerInfo sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = gfx::Filter::LINEAR;
    sampler.maxAnisotropy = config::VT_MAX_ANISOTROPY;
    sampler.addressU = sampler.addressV = sampler.addressW = gfx::Address::CLAMP;
    _sampler = device->getSampler(sampler);
    gfx::RenderPassInfo passInfo;
    gfx::ColorAttachment color;
    color.format = gfx::Format::RGBA8;
    color.loadOp = gfx::LoadOp::LOAD;
    color.storeOp = gfx::StoreOp::STORE;
    gfx::GeneralBarrierInfo barrier;
    barrier.prevAccesses = gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    barrier.nextAccesses = gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    if (_compress) {
        barrier.prevAccesses |= gfx::AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE;
        barrier.nextAccesses |= gfx::AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE;
    }
    color.barrier = device->getGeneralBarrier(barrier);
    passInfo.colorAttachments = {color, color};
    // No depth attachment: page rectangles never overlap.
    IRenderTextureCreateInfo info;
    info.name = "landscape-vt-atlas";
    info.width = renderSize();
    info.height = renderSize();
    info.passInfo = passInfo;
    info.colorMipLevels = config::VT_MIP_LEVELS;
    _atlas = ccnew RenderTexture();
    _atlas->initialize(info);
    return framebuffer() && framebuffer()->getColorTextures().size() == 2 && renderPass();
}

gfx::Framebuffer *VTRenderer::framebuffer() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getFramebuffer() : nullptr;
}
gfx::RenderPass *VTRenderer::renderPass() const {
    return framebuffer() ? framebuffer()->getRenderPass() : nullptr;
}
TextureBase *VTRenderer::atlas() const {
    return _compress ? static_cast<TextureBase *>(_compressedAtlas[0].get()) : _atlas.get();
}

gfx::Texture *VTRenderer::albedo() const {
    if (_compress) {
        return _compressedAtlas[0] ? _compressedAtlas[0]->getGFXTexture() : nullptr;
    }
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getColorTexture(0) : nullptr;
}
gfx::Texture *VTRenderer::normalRoughnessAO() const {
    if (_compress) {
        return _compressedAtlas[1] ? _compressedAtlas[1]->getGFXTexture() : nullptr;
    }
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getColorTexture(1) : nullptr;
}

Vec4 VTRenderer::mapping(VTPageAddress address) const {
    const int slot = _pages.resolve(address);
    CC_ASSERTF(slot >= 0, "[Landscape] exact VT page must be published before drawing");
    if (slot < 0) {
        return {};
    }
    const auto region = _pages.region(address);
    return {region.x, region.z, region.size, static_cast<float>(slot)};
}

void VTRenderer::requiredSources(ccstd::vector<NodeAddress> &sources) const {
    if (!_debugData.cliffEnabled && _cliff.params.w <= 0.0F) {
        return;
    }
    for (uint32_t z = 0; z < _cliff.rows; ++z) {
        for (uint32_t x = 0; x < _cliff.columns; ++x) {
            sources.push_back({_cliff.level, x, z});
        }
    }
}

void VTRenderer::bindComposeTexture(const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
    _material->setPropertyGFXTexture(name, texture);
    auto *pass = _material->getPasses()->front().get();
    const auto handle = pass->getHandle(name);
    if (handle) {
        pass->bindSampler(scene::Pass::getBindingFromHandle(handle), sampler);
    }
}

bool VTRenderer::initComposeMaterial(const LandscapeAsset &asset, TilePagePool &tiles,
                                     const MaterialLibrary &materials, gfx::Device *device) {
    const auto &data = asset.data();
    _mesh = GridMesh::createVTQuad(device);
    _material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"landscape/builtin-landscape-vt-compose"};
    info.defines = IMaterialInfo::DefinesType{MacroRecord{{"LANDSCAPE_HEIGHT_UNORM", tiles.heightIsUnorm()}, {"LANDSCAPE_VT_COMPACT", _compress}}};
    _material->initialize(info);
    if (!_mesh || !_material->getPasses() || _material->getPasses()->empty()) {
        return false;
    }
    auto *pass = _material->getPasses()->front().get();
    const char *requiredProperties[] = {
        "sourceParams",
        "vtLayout",
        "splatmap",
        "albedoHeightMap",
        "normalRoughnessAOMap",
        "materialParams",
        "heightBlendParams",
        "terrainNormalMap",
        "normalSourceMap",
        "cliffSourceMap",
        "terrainHeightMap",
        "cliffParams",
        "cliffLayout",
        "cliffMaterialParams",
    };
    for (const char *name : requiredProperties) {
        if (pass->getHandle(name) == 0U) {
            CC_LOG_WARNING("[Landscape] VT effect is missing %s; reimport builtin-landscape effects and rebuild assets", name);
            return false;
        }
    }
    bindComposeTexture("splatmap", tiles.splatArray(), tiles.splatSampler());
    bindComposeTexture("terrainNormalMap", tiles.normalArray(), tiles.heightSampler());
    bindComposeTexture("terrainHeightMap", tiles.heightArray(), tiles.heightSampler());
    bindComposeTexture("albedoHeightMap", materials.albedoHeight(), materials.repeatSampler());
    bindComposeTexture("normalRoughnessAOMap", materials.normalRoughnessAO(), materials.repeatSampler());
    if (!_biomeMap.init(_material, data.width(), data.depth(), materials.whiteTexture(), materials.clampSampler())) {
        return false;
    }
    _material->setPropertyVec4Array("materialParams", materials.materialParams());
    _material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(renderSize()),
                                                static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER),
                                                device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
    _material->setPropertyVec4("sourceParams", Vec4{static_cast<float>(asset.data().tileResolution), _debugData.bakeNormalEnabled ? 1.0F : 0.0F, 0, 0});
    return true;
}

bool VTRenderer::initNormalSourceTable(TilePagePool &tiles, gfx::Device *device) {
    gfx::TextureInfo indexInfo;
    indexInfo.type = gfx::TextureType::TEX2D;
    indexInfo.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    indexInfo.height = config::VT_PAGE_COUNT;
    indexInfo.format = gfx::Format::RGBA32F;
    indexInfo.width = config::VT_NORMAL_SOURCE_COUNT;
    _normalSources = device->createTexture(indexInfo);
    if (!_normalSources) {
        return false;
    }
    _normalSourceData.resize(config::VT_PAGE_COUNT * config::VT_NORMAL_SOURCE_COUNT);
    bindComposeTexture("normalSourceMap", _normalSources, tiles.splatSampler());
    return true;
}

bool VTRenderer::initCliffReference(const LandscapeAsset &asset, TilePagePool &tiles, gfx::Device *device) {
    const auto &data = asset.data();
    gfx::TextureInfo indexInfo;
    indexInfo.type = gfx::TextureType::TEX2D;
    indexInfo.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    indexInfo.format = gfx::Format::RGBA32F;
    _cliff.data = data;
    _cliff.level = cliffReferenceLevel(data);
    _cliff.columns = data.nodesX(_cliff.level);
    _cliff.rows = data.nodesZ(_cliff.level);
    indexInfo.width = _cliff.columns;
    indexInfo.height = _cliff.rows;
    _cliff.texture = device->createTexture(indexInfo);
    if (!_cliff.texture) {
        return false;
    }
    _cliff.sources.resize(static_cast<size_t>(_cliff.columns) * _cliff.rows);
    bindComposeTexture("cliffSourceMap", _cliff.texture, tiles.splatSampler());
    const float referenceSize = data.nodeSize(_cliff.level);
    const auto &cliffMaterial = asset.cliffMaterial();
    _cliff.params = Vec4{0, data.heightScale, data.heightBias, static_cast<float>(cliffMaterial.layer + 1)};
    _material->setPropertyVec4("cliffMaterialParams", Vec4{cliffMaterial.maxNormalY,
                                                           cliffMaterial.globalColorInfluence, 0, 0});
    _material->setPropertyVec4("cliffParams", _cliff.params);
    _material->setPropertyVec4("cliffLayout", Vec4{referenceSize, 0, 0, 0});
    return true;
}

bool VTRenderer::initPageDrawResources(gfx::Device *device) {
    auto *pass = _material->getPasses()->front().get();
    constexpr uint32_t STRIDE = sizeof(PageInstance);
    _instances = device->createBuffer({gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
                                       gfx::MemoryUsageBit::DEVICE, config::VT_PAGE_COUNT * STRIDE, STRIDE});
    gfx::InputAssemblerInfo iaInfo;
    iaInfo.attributes = _mesh->getAttributes();
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtPage", gfx::Format::RGBA32F, false, 1, true, 1});
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtRegion", gfx::Format::RGBA32F, false, 1, true, 2});
    iaInfo.attributes.emplace_back(gfx::Attribute{"a_vtSource", gfx::Format::RGBA32F, false, 1, true, 3});
    iaInfo.vertexBuffers = _mesh->getVertexBuffers();
    iaInfo.vertexBuffers.push_back(_instances.get());
    iaInfo.indexBuffer = _mesh->getIndexBuffer();
    _inputAssembler = device->createInputAssembler(iaInfo);
    auto *shader = pass->getShaderVariant();
    if (!shader) {
        return false;
    }
    _pipelineState = device->createPipelineState({shader, pass->getPipelineLayout(), renderPass(), {iaInfo.attributes}, *pass->getRasterizerState(), *pass->getDepthStencilState(), *pass->getBlendState(), pass->getPrimitive(), pass->getDynamicStates()});
    return _instances && _inputAssembler && _pipelineState;
}

bool VTRenderer::initMipPasses(gfx::Device *device) {
    IMaterialInfo info;
    info.effectName = ccstd::string{"landscape/builtin-landscape-vt-compose"};
    info.technique = 1;
    info.defines = IMaterialInfo::DefinesType{MacroRecord{{"LANDSCAPE_VT_COMPACT", _compress}}};
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
        mip.albedo = view(_atlas->getWindow()->getColorTexture(0), level, 1);
        mip.normal = view(_atlas->getWindow()->getColorTexture(1), level, 1);
        // Prefix views exclude the destination, preserving absolute mip indices.
        mip.sourceAlbedo = view(_atlas->getWindow()->getColorTexture(0), 0, level);
        mip.sourceNormal = view(_atlas->getWindow()->getColorTexture(1), 0, level);
        if (!mip.albedo || !mip.normal || !mip.sourceAlbedo || !mip.sourceNormal) {
            return false;
        }
        mip.framebuffer = device->createFramebuffer({renderPass(), {mip.albedo, mip.normal}, nullptr});
        if (!mip.framebuffer) {
            return false;
        }
        // Technique 1 filters the completed pages, never re-evaluates materials.
        auto &mipMaterial = _mipPasses[level - 1].material;
        mipMaterial = ccnew Material();
        mipMaterial->initialize(info);
        if (!mipMaterial->getPasses() || mipMaterial->getPasses()->empty()) {
            return false;
        }
        auto *mipPass = mipMaterial->getPasses()->front().get();
        if (!mipPass->getHandle("mipLayout") || !mipPass->getHandle("sourceAlbedo") || !mipPass->getHandle("sourceNormal")) {
            CC_LOG_ERROR("[Landscape] VT mip technique is missing; rebuild project effects");
            return false;
        }
        // Read the preceding level; the source view excludes the destination.
        mipMaterial->setPropertyGFXTexture("sourceAlbedo", mip.sourceAlbedo.get());
        mipMaterial->setPropertyGFXTexture("sourceNormal", mip.sourceNormal.get());
        mipMaterial->setPropertyVec4("mipLayout", Vec4{static_cast<float>(renderSize() >> (level - 1)),
                                                       static_cast<float>(config::VT_PAGE_RES >> (level - 1)), static_cast<float>(level - 1),
                                                       device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
        mipPass->update();
        auto *mipShader = mipPass->getShaderVariant();
        if (!mipShader) {
            return false;
        }
        _mipPasses[level - 1].pipelineState = device->createPipelineState({mipShader, mipPass->getPipelineLayout(), renderPass(), {_inputAssembler->getAttributes()}, *mipPass->getRasterizerState(), *mipPass->getDepthStencilState(), *mipPass->getBlendState(), mipPass->getPrimitive(), mipPass->getDynamicStates()});
        if (!_mipPasses[level - 1].pipelineState) {
            return false;
        }
    }
    return true;
}

bool VTRenderer::initCompression(gfx::Device *device) {
    for (auto &texture : _compressedAtlas) {
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
    shaderInfo.blocks = {{1, 0, "CompressionConstants", {{"compressionParams", gfx::Type::FLOAT4, 1}}, 1}};
    shaderInfo.samplerTextures = {{1, 1, "sourceAlbedo", gfx::Type::SAMPLER2D, 1},
                                  {1, 2, "sourceNormal", gfx::Type::SAMPLER2D, 1}};
    shaderInfo.buffers = {{1, 3, "CompressedBlocks", 1, gfx::MemoryAccessBit::WRITE_ONLY}};
    _compressShader = device->createShader(shaderInfo);
    gfx::DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings = {{0, gfx::DescriptorType::UNIFORM_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE},
                           {1, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE},
                           {2, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE},
                           {3, gfx::DescriptorType::STORAGE_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE}};
    _compressLayout = device->createDescriptorSetLayout(layoutInfo);
    _emptyLayout = device->createDescriptorSetLayout({});
    _compressPipelineLayout = device->createPipelineLayout({{_emptyLayout, _compressLayout}});
    constexpr uint32_t BLOCKS = config::VT_PAGE_RES * config::VT_PAGE_RES / 16;
    _compressedBlocks = device->createBuffer({gfx::BufferUsageBit::STORAGE | gfx::BufferUsageBit::TRANSFER_SRC,
                                              gfx::MemoryUsageBit::DEVICE, BLOCKS * 32, 16});
    if (!_compressShader || !_compressedBlocks || !_compressLayout || !_compressPipelineLayout) {
        return false;
    }
    gfx::SamplerInfo samplerInfo;
    samplerInfo.minFilter = samplerInfo.magFilter = gfx::Filter::POINT;
    samplerInfo.mipFilter = gfx::Filter::NONE;
    auto *sampler = device->getSampler(samplerInfo);
    for (uint32_t level = 0; level < config::VT_MIP_LEVELS; ++level) {
        auto &uniform = _compressUniforms[level];
        uniform = device->createBuffer({gfx::BufferUsageBit::UNIFORM | gfx::BufferUsageBit::TRANSFER_DST,
                                         gfx::MemoryUsageBit::DEVICE, sizeof(Vec4), sizeof(Vec4)});
        const Vec4 params{static_cast<float>(level), 0, 0, 0};
        uniform->update(&params, sizeof(params));
        auto &set = _compressSets[level];
        set = device->createDescriptorSet({_compressLayout});
        set->bindBuffer(0, uniform);
        set->bindTexture(1, _atlas->getWindow()->getColorTexture(0));
        set->bindSampler(1, sampler);
        set->bindTexture(2, _atlas->getWindow()->getColorTexture(1));
        set->bindSampler(2, sampler);
        set->bindBuffer(3, _compressedBlocks);
        set->update();
    }
    gfx::PipelineStateInfo pipelineInfo;
    pipelineInfo.shader = _compressShader;
    pipelineInfo.pipelineLayout = _compressPipelineLayout;
    pipelineInfo.bindPoint = gfx::PipelineBindPoint::COMPUTE;
    _compressPipeline = device->createPipelineState(pipelineInfo);
    return _compressPipeline != nullptr;
}

void VTRenderer::transitionAtlas(bool writing) {
    auto *device = Root::getInstance()->getDevice();
    gfx::TextureBarrierInfo info;
    info.prevAccesses = writing ? (_needsClear ? gfx::AccessFlagBit::NONE : gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE)
                               : gfx::AccessFlagBit::TRANSFER_WRITE;
    info.nextAccesses = writing ? gfx::AccessFlagBit::TRANSFER_WRITE : gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    info.discardContents = writing && _needsClear;
    info.range.levelCount = config::VT_MIP_LEVELS;
    info.range.numSlices = 1;
    const gfx::TextureBarrier *barriers[] = {device->getTextureBarrier(info), device->getTextureBarrier(info)};
    // Texture2D exposes a sampling view; transfers target its underlying image.
    const gfx::Texture *textures[] = {albedo()->getViewInfo().texture, normalRoughnessAO()->getViewInfo().texture};
    _commands->pipelineBarrier(nullptr, nullptr, nullptr, 0, barriers, textures, 2);
}

void VTRenderer::compressPage(uint32_t slot) {
    auto *device = Root::getInstance()->getDevice();
    for (uint32_t level = 0; level < config::VT_MIP_LEVELS; ++level) {
        const uint32_t size = config::VT_PAGE_RES >> level;
        const uint32_t blocks = size / 4;
        auto *barrier = device->getGeneralBarrier({gfx::AccessFlagBit::TRANSFER_READ, gfx::AccessFlagBit::COMPUTE_SHADER_WRITE});
        _commands->pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
        _commands->bindPipelineState(_compressPipeline);
        _commands->bindDescriptorSet(1, _compressSets[level]);
        gfx::DispatchInfo dispatch;
        dispatch.groupCountX = (blocks + 7) / 8;
        dispatch.groupCountY = (blocks + 7) / 8;
        dispatch.groupCountZ = 1;
        _commands->dispatch(dispatch);
        barrier = device->getGeneralBarrier({gfx::AccessFlagBit::COMPUTE_SHADER_WRITE, gfx::AccessFlagBit::TRANSFER_READ});
        _commands->pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
        gfx::BufferTextureCopy copy;
        copy.texExtent = {size, size, 1};
        copy.texOffset = {static_cast<int32_t>((slot % config::VT_PAGES_PER_SIDE) * size),
                          static_cast<int32_t>((slot / config::VT_PAGES_PER_SIDE) * size), 0};
        copy.texSubres.mipLevel = level;
        _commands->copyBufferToTexture(_compressedBlocks, albedo()->getViewInfo().texture, &copy, 1);
        copy.buffOffset = blocks * blocks * 16;
        _commands->copyBufferToTexture(_compressedBlocks, normalRoughnessAO()->getViewInfo().texture, &copy, 1);
    }
}

bool VTRenderer::initCommandResources(gfx::Device *device) {
    _commands = device->createCommandBuffer({device->getQueue(), gfx::CommandBufferType::PRIMARY});
    gfx::RenderPassInfo initialInfo;
    initialInfo.colorAttachments = renderPass()->getColorAttachments();
    for (auto &color : initialInfo.colorAttachments) {
        color.loadOp = gfx::LoadOp::CLEAR;
        color.barrier = device->getGeneralBarrier({gfx::AccessFlagBit::NONE,
            _compress ? gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE | gfx::AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE
                      : gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE});
    }
    _initialPass = device->createRenderPass(initialInfo);
    return _commands && _initialPass;
}

void VTRenderer::setGlobalColorMap(Texture2D *texture) {
    if (_biomeMap.setColorMap(texture)) {
        invalidateComposedPages();
    }
}

void VTRenderer::setGlobalColorStrength(float strength) {
    if (_biomeMap.setColorStrength(strength)) {
        // Preserve cached pages while frozen; apply edits when updates resume.
        invalidateComposedPages();
    }
#if CC_DEBUG && CC_LANDSCAPE_DEBUG
    if (_material) {
        auto *pass = _material->getPasses()->front().get();
        const auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("globalColorParams")));
        const auto binding = scene::Pass::getBindingFromHandle(pass->getHandle("globalColorMap"));
        const auto *texture = pass->getDescriptorSet()->getTexture(binding);
        const auto stale = std::count_if(_pages.activeSlots().begin(), _pages.activeSlots().end(), [this](uint32_t slot) {
            return _pages.page(slot).state == VirtualTexture::State::STALE;
        });
        CC_LOG_INFO("[Landscape] Biome strength: requested=%.2f effective=%.2f map=%ux%u frozen=%d stale=%zu/%zu",
                    strength, params.z, texture ? texture->getWidth() : 0U, texture ? texture->getHeight() : 0U,
                    _debugData.freezeLod ? 1 : 0, static_cast<size_t>(stale), _pages.activeSlots().size());
    }
#endif
}

void VTRenderer::invalidateComposedPages() {
    if (_debugData.freezeLod) {
        _pendingInvalidation = true;
    } else {
        _pages.invalidate();
    }
}

void VTRenderer::setFrozen(bool frozen) {
    _debugData.freezeLod = frozen;
    if (!frozen && _pendingInvalidation) {
        _pages.invalidate();
        _pendingInvalidation = false;
    }
}

void VTRenderer::setBakeNormalEnabled(bool enabled) {
    if (!_material || _debugData.bakeNormalEnabled == enabled) {
        return;
    }
    // The owner defers BOTH composition and decoding while pages are frozen.
    CC_ASSERT(!_debugData.freezeLod);
    _debugData.bakeNormalEnabled = enabled;
    auto *pass = _material->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("sourceParams")));
    params.y = enabled ? 1.0F : 0.0F;
    _material->setPropertyVec4("sourceParams", params);
    _pages.invalidate();
}

void VTRenderer::setCliffEnabled(bool enabled) {
    if (_debugData.cliffEnabled == enabled) {
        return;
    }
    CC_ASSERT(!_debugData.freezeLod);
    _debugData.cliffEnabled = enabled;
    // An authored material override needs the same slope mask with projection disabled. Keep
    // its references pinned; negative X means ready with planar projection.
    if (_cliff.params.w <= 0.0F) {
        _cliff.ready = false;
    }
    _cliff.params.x = _cliff.ready ? (enabled ? 1.0F : -1.0F) : 0.0F;
    if (_material) {
        _material->setPropertyVec4("cliffParams", _cliff.params);
    }
    _pages.invalidate();
}

void VTRenderer::syncCliffSources(TilePagePool &tiles) {
    if (_cliff.ready || (!_debugData.cliffEnabled && _cliff.params.w <= 0.0F) || _debugData.freezeLod || !_material) {
        return;
    }
    bool ready = true;
    const float size = _cliff.data.nodeSize(_cliff.level);
    for (uint32_t z = 0; z < _cliff.rows; ++z) {
        for (uint32_t x = 0; x < _cliff.columns; ++x) {
            const int layer = tiles.resident({_cliff.level, x, z});
            ready &= layer >= 0;
            const auto region = _cliff.data.nodeRegion({_cliff.level, x, z});
            _cliff.sources[z * _cliff.columns + x] = Vec4{region.x, region.z, region.size, static_cast<float>(layer)};
        }
    }
    if (!ready) {
        return;
    }
    // Publish once, after the entire fixed reference is resident. No per-camera
    // source LOD switches, partial-resolution seams or repeated VT invalidation.
    gfx::BufferTextureCopy region;
    region.texExtent.width = _cliff.columns;
    region.texExtent.height = _cliff.rows;
    region.texExtent.depth = 1;
    const uint8_t *buffers[]{reinterpret_cast<const uint8_t *>(_cliff.sources.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(buffers, _cliff.texture, &region, 1);
    _cliff.ready = true;
    _cliff.params.x = _debugData.cliffEnabled ? 1.0F : -1.0F;
    _material->setPropertyVec4("cliffParams", _cliff.params);
    _pages.invalidate();
    CC_LOG_INFO("[Landscape] Cliff reference ready: L%u, %ux%u tiles, %.2f m sample spacing",
                _cliff.level, _cliff.columns, _cliff.rows, size / (_cliff.data.tileResolution - 1U));
}

void VTRenderer::setHeightBlendEnabled(bool enabled) {
    if (!_material) {
        return;
    }
    auto *pass = _material->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("heightBlendParams")));
    const float strength = enabled ? 1.0F : 0.0F;
    if (params.x == strength) {
        return;
    }
    // Keep the effect's scale/sharpness when toggling the global blend mode.
    _debugData.heightBlendEnabled = enabled;
    params.x = strength;
    _material->setPropertyVec4("heightBlendParams", params);
    // Frozen pages can reference source tiles that are no longer resident.
    // Re-resolve them through the normal update path after unfreezing.
    invalidateComposedPages();
}

bool VTRenderer::valid() const {
    return albedo() && normalRoughnessAO() && _pipelineState && _commands &&
           std::all_of(_mipPasses.begin(), _mipPasses.end(),
                       [](const MipPass &pass) { return pass.pipelineState && pass.framebuffer; });
}

uint32_t VTRenderer::render(TilePagePool &tiles, TilePageResolver &resolver, uint32_t maxUpdates) {
    if (_debugData.freezeLod || !valid()) {
        return 0;
    }
    syncCliffSources(tiles);
    if (maxUpdates == 0 || !sourcesReady()) {
        return 0;
    }
    // Source slots can change between batches. Resolve only for this submission,
    // so no cached input bindings or separate input-readiness state can go stale.
    _batchSlots.clear();
    _batchInstances.clear();
    for (const uint32_t slot : _pages.activeSlots()) {
        const auto &page = _pages.page(slot);
        if (page.state == VirtualTexture::State::READY) {
            continue;
        }
        VTPageInputs inputs;
        if (!resolver.resolvePage(page.address, _debugData.bakeNormalEnabled, inputs)) {
            continue;
        }
        const uint32_t decalCount = _decals->collectPageDecals(slot, inputs.region);
        _batchSlots.push_back(slot);
        _batchInstances.push_back({Vec4{static_cast<float>(slot), static_cast<float>(decalCount), 0, 0},
                                  inputs.region, inputs.splatSource});
        std::copy(inputs.normalSources.begin(), inputs.normalSources.end(),
                  _normalSourceData.begin() + slot * config::VT_NORMAL_SOURCE_COUNT);
        if (_batchInstances.size() == maxUpdates) {
            break;
        }
    }
    if (_batchSlots.empty()) {
        return 0;
    }
    _decals->uploadPageDecals();
    gfx::BufferTextureCopy indexCopy;
    indexCopy.texExtent = {config::VT_NORMAL_SOURCE_COUNT, config::VT_PAGE_COUNT, 1};
    const uint8_t *normalBytes[]{reinterpret_cast<const uint8_t *>(_normalSourceData.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(normalBytes, _normalSources, &indexCopy, 1);

    _material->getPasses()->front()->update();
    _commands->begin();
    if (_compress) {
        transitionAtlas(true);
    }
    const uint32_t submissions = _compress ? static_cast<uint32_t>(_batchInstances.size()) : 1U;
    for (uint32_t i = 0; i < submissions; ++i) {
        const uint32_t instanceCount = _compress ? 1U : static_cast<uint32_t>(_batchInstances.size());
        // Updating the vertex buffer is ordered after the preceding page's draws.
        const auto *barrier = Root::getInstance()->getDevice()->getGeneralBarrier(
            {gfx::AccessFlagBit::VERTEX_BUFFER, gfx::AccessFlagBit::TRANSFER_WRITE});
        _commands->pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
        _commands->updateBuffer(_instances, _batchInstances.data() + i, instanceCount * sizeof(PageInstance));
        barrier = Root::getInstance()->getDevice()->getGeneralBarrier(
            {gfx::AccessFlagBit::TRANSFER_WRITE, gfx::AccessFlagBit::VERTEX_BUFFER});
        _commands->pipelineBarrier(barrier, nullptr, nullptr, 0, nullptr, nullptr, 0);
        recordPagePass(_material, _pipelineState, framebuffer(), renderSize(), instanceCount);
        for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
            const auto &mip = _mipPasses[level - 1];
            recordPagePass(mip.material, mip.pipelineState, mip.framebuffer.get(), renderSize() >> level, instanceCount);
        }
        if (_compress) {
            compressPage(_batchSlots[i]);
        }
        // Later pages reuse the scratch attachments after compute has read them.
        // Only the first page may use the initial render pass with no prior access.
        _needsClear = false;
    }
    if (_compress) {
        transitionAtlas(false);
    }
    _commands->end();
    gfx::CommandBuffer *command = _commands.get();
    auto *device = Root::getInstance()->getDevice();
    device->flushCommands(&command, 1);
    device->getQueue()->submit(&command, 1);
#if CC_DEBUG && CC_LANDSCAPE_DEBUG
    const auto recomposed = std::count_if(_batchSlots.begin(), _batchSlots.end(), [this](uint32_t slot) {
        return _pages.page(slot).state == VirtualTexture::State::STALE;
    });
    if (recomposed > 0) {
        CC_LOG_INFO("[Landscape] VT recomposed: %zu stale pages submitted (%s)",
                    static_cast<size_t>(recomposed), _compress ? COMPRESSION_NAME : "RGBA8");
    }
#endif
    // Publish only after composition and ALL mip levels are on the same queue.
    _pages.publish(_batchSlots);
    return static_cast<uint32_t>(_batchSlots.size());
}

void VTRenderer::recordPagePass(Material *material, gfx::PipelineState *pipelineState,
                                gfx::Framebuffer *framebuffer, uint32_t atlasSize, uint32_t instanceCount) {
    const gfx::Color colors[2] = {{0, 0, 0, 1}, {0.5F, 0.5F, 1, 1}};
    _commands->beginRenderPass(_needsClear ? _initialPass.get() : renderPass(), framebuffer,
                               gfx::Rect{0, 0, atlasSize, atlasSize}, colors, 1.0F, 0);
    auto *pass = material->getPasses()->front().get();
    _commands->bindPipelineState(pipelineState);
    _commands->bindDescriptorSet(static_cast<uint32_t>(pipeline::SetIndex::MATERIAL), pass->getDescriptorSet());
    _commands->bindInputAssembler(_inputAssembler);
    auto draw = _inputAssembler->getDrawInfo();
    draw.instanceCount = instanceCount;
    _commands->draw(draw);
    _commands->endRenderPass();
}

void VTRenderer::destroy() {
    _compressPipeline = nullptr;
    _compressSets = {};
    _compressUniforms = {};
    _compressPipelineLayout = nullptr;
    _compressLayout = nullptr;
    _emptyLayout = nullptr;
    _compressShader = nullptr;
    _compressedBlocks = nullptr;
    for (auto &texture : _compressedAtlas) {
        if (texture) {
            texture->destroy();
        }
        texture = nullptr;
    }
    _compress = false;
    _cliff = {};
    _normalSources = nullptr;
    _normalSourceData.clear();
    _decals = nullptr;
    _debugData.freezeLod = false;
    _pendingInvalidation = false;
    _biomeMap.destroy();
    _commands = nullptr;
    _pipelineState = nullptr;
    for (auto &mip : _mipPasses) {
        mip.pipelineState = nullptr;
    }
    _initialPass = nullptr;
    _inputAssembler = nullptr;
    _instances = nullptr;
    if (_mesh) {
        _mesh->destroy();
    }
    _mesh = nullptr;
    if (_material) {
        _material->destroy();
    }
    _material = nullptr;
    for (auto &mip : _mipPasses) {
        if (mip.material) {
            mip.material->destroy();
        }
        mip = MipPass{};
    }
    if (_atlas) {
        _atlas->destroy();
    }
    _atlas = nullptr;
    _sampler = nullptr;
    _pages = VirtualTexture{};
    _batchSlots.clear();
    _batchInstances.clear();
    _needsClear = true;
}
} // namespace landscape
} // namespace cc

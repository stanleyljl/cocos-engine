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
VTRenderer::VTRenderer() = default;
VTRenderer::~VTRenderer() { destroy(); }

bool VTRenderer::init(const LandscapeAsset &asset, TilePagePool &tiles, const MaterialLibrary &materials, DecalRenderer &decals) {
    CC_ASSERT(_material == nullptr && _atlas == nullptr);
    auto *root = Root::getInstance();
    auto *device = root ? root->getDevice() : nullptr;
    if (!device || !tiles.valid() || !materials.valid()) {
        return false;
    }
    if (!_pages.init(asset.data())) {
        return false;
    }
    _decals = &decals;
    if (!initAtlas(device) ||
        !initComposeMaterial(asset, tiles, materials, device) ||
        !_decals->bindComposeMaterial(_material, materials) ||
        !initNormalSourceTable(tiles, device) ||
        !initCliffReference(asset, tiles, device)) {
        destroy();
        return false;
    }
    _material->getPasses()->front()->update();
    if (!initPageDrawResources(device) || !initMipPasses(device) || !initCommandResources(device)) {
        destroy();
        return false;
    }
    _dirtySlots.reserve(config::VT_PAGE_COUNT);
    _pageInstances.reserve(config::VT_PAGE_COUNT);
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
    color.barrier = device->getGeneralBarrier(barrier);
    passInfo.colorAttachments = {color, color};
    // No depth attachment: page rectangles never overlap.
    IRenderTextureCreateInfo info;
    info.name = "landscape-vt-atlas";
    info.width = config::VT_ATLAS_SIZE;
    info.height = config::VT_ATLAS_SIZE;
    info.passInfo = passInfo;
    info.colorMipLevels = config::VT_MIP_LEVELS;
    _atlas = ccnew RenderTexture();
    _atlas->initialize(info);
    return albedo() != nullptr && normalRoughnessAO() != nullptr && renderPass() != nullptr;
}

gfx::Framebuffer *VTRenderer::framebuffer() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getFramebuffer() : nullptr;
}
gfx::RenderPass *VTRenderer::renderPass() const {
    return framebuffer() ? framebuffer()->getRenderPass() : nullptr;
}
gfx::Texture *VTRenderer::albedo() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getColorTexture(0) : nullptr;
}
gfx::Texture *VTRenderer::normalRoughnessAO() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getColorTexture(1) : nullptr;
}

Vec4 VTRenderer::mapping(VTPageAddress address) const {
    const int slot = _pages.resolve(address);
    CC_ASSERTF(slot >= 0, "[Landscape] exact VT page must be published before drawing");
    if (slot < 0) {
        return {};
    }
    const auto &region = _pages.page(static_cast<uint32_t>(slot)).inputs.region;
    return {region.x, region.y, region.z, static_cast<float>(slot)};
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

void VTRenderer::resolveSources(TilePagePool &tiles, TilePageResolver &resolver) {
    syncCliffSources(tiles);
    for (const uint32_t slot : _pages.activeSlots()) {
        const auto &page = _pages.page(slot);
        if (page.state != VirtualTexture::State::DIRTY) {
            continue;
        }
        VTPageInputs inputs;
        if (resolver.resolvePage(page.address, _debugData.bakeNormalEnabled, inputs)) {
            _pages.setInputs(slot, inputs);
        } else {
            _pages.clearInputs(slot);
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
    info.effectName = ccstd::string{"builtin-landscape-vt-compose"};
    info.defines = IMaterialInfo::DefinesType{MacroRecord{{"LANDSCAPE_HEIGHT_UNORM", tiles.heightIsUnorm()}}};
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
        "tilingParams",
        "globalColorMap",
        "globalColorParams",
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
    _fallbackGlobalColorMap = materials.whiteTexture();
    _globalColorSampler = materials.clampSampler();
    bindComposeTexture("globalColorMap", _fallbackGlobalColorMap, _globalColorSampler);
    _globalColorParams = Vec4{1.0F / data.worldWidth(),
                              1.0F / data.worldDepth(), 0.0F, 0.0F};
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    _material->setPropertyVec4Array("tilingParams", materials.tilingParams());
    _material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE),
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
    const auto origin = data.gridToLocal({});
    _material->setPropertyVec4("cliffLayout", Vec4{static_cast<float>(origin.x),
                                                   static_cast<float>(origin.z), referenceSize, 0});
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
    info.effectName = ccstd::string{"builtin-landscape-vt-compose"};
    info.technique = 1;
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
        mip.albedo = view(albedo(), level, 1);
        mip.normal = view(normalRoughnessAO(), level, 1);
        // Prefix views exclude the destination, preserving absolute mip indices.
        mip.sourceAlbedo = view(albedo(), 0, level);
        mip.sourceNormal = view(normalRoughnessAO(), 0, level);
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
        mipMaterial->setPropertyVec4("mipLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE >> (level - 1)),
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

bool VTRenderer::initCommandResources(gfx::Device *device) {
    _commands = device->createCommandBuffer({device->getQueue(), gfx::CommandBufferType::PRIMARY});
    gfx::RenderPassInfo initialInfo;
    initialInfo.colorAttachments = renderPass()->getColorAttachments();
    for (auto &color : initialInfo.colorAttachments) {
        color.loadOp = gfx::LoadOp::CLEAR;
        color.barrier = device->getGeneralBarrier({gfx::AccessFlagBit::NONE,
                                                   gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE});
    }
    _initialPass = device->createRenderPass(initialInfo);
    return _commands && _initialPass;
}

void VTRenderer::setGlobalColorMap(Texture2D *texture) {
    if (!_material) {
        return;
    }
    _globalColorMap = texture;
    auto *gfxTexture = texture ? texture->getGFXTexture() : nullptr;
    _hasGlobalColorMap = gfxTexture != nullptr;
    bindComposeTexture("globalColorMap", gfxTexture ? gfxTexture : _fallbackGlobalColorMap.get(), _globalColorSampler);
    _globalColorParams.z = _hasGlobalColorMap ? _globalColorStrength : 0.0F;
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    invalidateComposedPages();
}

void VTRenderer::setGlobalColorStrength(float strength) {
    if (!_material) {
        return;
    }
    _globalColorStrength = std::clamp(strength, 0.0F, 1.0F);
    const float effectiveStrength = _hasGlobalColorMap ? _globalColorStrength : 0.0F;
    if (_globalColorParams.z == effectiveStrength) {
        return;
    }
    _globalColorParams.z = effectiveStrength;
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    // Preserve the cached image while frozen; apply edits when updates resume.
    invalidateComposedPages();
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
    if ((!_debugData.cliffEnabled && _cliff.params.w <= 0.0F) || _debugData.freezeLod || !_material) {
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
    if (_cliff.ready || !ready) {
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

uint32_t VTRenderer::render(uint32_t maxUpdates) {
    if (maxUpdates == 0 || _debugData.freezeLod || !valid() || !sourcesReady()) {
        return 0;
    }
    // Only explicitly reserved pages with complete inputs may be composed.
    // Publish after all mip passes, before committing their new draw bindings.
    _pages.collectUpdates(_dirtySlots, maxUpdates);
    if (_dirtySlots.empty()) {
        return 0;
    }
    buildPageBatch();
    uploadSourceTables();
    submitPageBatch();
    // Publish only after ALL levels have been submitted on the same queue.
    _pages.publish(_dirtySlots);
    _needsClear = false;
    return static_cast<uint32_t>(_dirtySlots.size());
}

void VTRenderer::buildPageBatch() {
    _pageInstances.clear();
    for (uint32_t slot : _dirtySlots) {
        const auto &inputs = _pages.page(slot).inputs;
        const uint32_t decalCount = _decals->collectPageDecals(slot, inputs.region);
        _pageInstances.push_back({Vec4{static_cast<float>(slot), static_cast<float>(decalCount), 0, 0},
                                  inputs.region, inputs.splatSource});
        std::copy(inputs.normalSources.begin(), inputs.normalSources.end(),
                  _normalSourceData.begin() + slot * config::VT_NORMAL_SOURCE_COUNT);
    }
}

void VTRenderer::uploadSourceTables() {
    _decals->uploadPageDecals();
    gfx::BufferTextureCopy indexCopy;
    indexCopy.texExtent = {config::VT_NORMAL_SOURCE_COUNT, config::VT_PAGE_COUNT, 1};
    const uint8_t *normalBytes[]{reinterpret_cast<const uint8_t *>(_normalSourceData.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(normalBytes, _normalSources, &indexCopy, 1);
}

void VTRenderer::recordPagePass(Material *material, gfx::PipelineState *pipelineState,
                                gfx::Framebuffer *framebuffer, uint32_t atlasSize) {
    const gfx::Color colors[2] = {{0, 0, 0, 1}, {0.5F, 0.5F, 1, 1}};
    _commands->beginRenderPass(_needsClear ? _initialPass.get() : renderPass(), framebuffer,
                               gfx::Rect{0, 0, atlasSize, atlasSize}, colors, 1.0F, 0);
    auto *pass = material->getPasses()->front().get();
    _commands->bindPipelineState(pipelineState);
    _commands->bindDescriptorSet(static_cast<uint32_t>(pipeline::SetIndex::MATERIAL), pass->getDescriptorSet());
    _commands->bindInputAssembler(_inputAssembler);
    auto draw = _inputAssembler->getDrawInfo();
    draw.instanceCount = static_cast<uint32_t>(_pageInstances.size());
    _commands->draw(draw);
    _commands->endRenderPass();
}

void VTRenderer::submitPageBatch() {
    _material->getPasses()->front()->update();
    _commands->begin();
    _commands->updateBuffer(_instances, _pageInstances.data(),
                            static_cast<uint32_t>(_pageInstances.size() * sizeof(PageInstance)));
    recordPagePass(_material, _pipelineState, framebuffer(), config::VT_ATLAS_SIZE);

    // Generate mip1 from mip0, then mip2 from mip1 for exactly the same slots.
    // Each pass makes its output shader-readable before the next downsample.
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        const auto &mip = _mipPasses[level - 1];
        recordPagePass(mip.material, mip.pipelineState, mip.framebuffer.get(), config::VT_ATLAS_SIZE >> level);
    }
    _commands->end();
    gfx::CommandBuffer *command = _commands.get();
    auto *device = Root::getInstance()->getDevice();
    device->flushCommands(&command, 1);
    device->getQueue()->submit(&command, 1);
}

void VTRenderer::destroy() {
    _cliff.texture = nullptr;
    _cliff.sources.clear();
    _cliff.ready = false;
    _normalSources = nullptr;
    _normalSourceData.clear();
    _decals = nullptr;
    _debugData.freezeLod = false;
    _pendingInvalidation = false;
    _hasGlobalColorMap = false;
    _globalColorMap = nullptr;
    _fallbackGlobalColorMap = nullptr;
    _globalColorSampler = nullptr;
    _globalColorStrength = 0.0F;
    _globalColorParams = Vec4{};
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
    _dirtySlots.clear();
    _pageInstances.clear();
    _needsClear = true;
}
} // namespace landscape
} // namespace cc

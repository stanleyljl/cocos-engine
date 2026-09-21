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
#include <cmath>

#include "base/Log.h"
#include "base/Macros.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/assets/Texture2D.h"
#include "landscape/GridMesh.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTPaging.h"
#include "renderer/gfx-base/GFXCommandBuffer.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXInputAssembler.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXPipelineState.h"
#include "renderer/gfx-base/GFXQueue.h"
#include "renderer/gfx-base/GFXRenderPass.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/Pass.h"

namespace cc {
namespace landscape {
VTRenderer::VTRenderer() = default;
VTRenderer::~VTRenderer() { destroy(); }

bool VTRenderer::init(const LandscapeAsset &asset, TilePagePool &tiles, const MaterialLibrary &materials) {
    CC_ASSERT(_material == nullptr && !_texture.valid());
    const auto &data = asset.data();
    const size_t rootCount = static_cast<size_t>(data.sectorsX) * data.sectorsZ;
    if (rootCount > config::VT_PAGE_COUNT) {
        CC_LOG_ERROR("[Landscape] VT needs %zu permanent root pages, capacity is %u", rootCount, config::VT_PAGE_COUNT);
        return false;
    }
    auto *root = Root::getInstance();
    auto *device = root ? root->getDevice() : nullptr;
    if (!device || !tiles.valid() || !materials.valid() || !_texture.init(device)) return false;

    if (!initComposeResources(asset, tiles, materials, device) ||
        !initDrawResources(device) || !initRootPages(data, tiles)) {
        destroy();
        return false;
    }
    _dirtySlots.reserve(config::VT_PAGE_COUNT);
    _pageInstances.reserve(config::VT_PAGE_COUNT);
#if CC_LANDSCAPE_DEBUG
    CC_LOG_INFO("[Landscape] VT enabled: %u cached pages, %u interior texels, two RGBA8 atlases with mip0 + mip1 + mip2",
        config::VT_PAGE_COUNT, config::VT_PAGE_INTERIOR);
#endif
    return true;
}

bool VTRenderer::initComposeResources(const LandscapeAsset &asset, TilePagePool &tiles,
                                     const MaterialLibrary &materials, gfx::Device *device) {
    if (!initComposeMaterial(asset, tiles, materials, device) ||
        !initDecalResources(asset, materials, device) ||
        !initNormalSourceTable(tiles, device) ||
        !initCliffReference(asset, tiles, device)) {
        return false;
    }
    _material->getPasses()->front()->update();
    return true;
}

void VTRenderer::bindComposeTexture(const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
    _material->setPropertyGFXTexture(name, texture);
    auto *pass = _material->getPasses()->front().get();
    const auto handle = pass->getHandle(name);
    if (handle) pass->bindSampler(scene::Pass::getBindingFromHandle(handle), sampler);
}

bool VTRenderer::initComposeMaterial(const LandscapeAsset &asset, TilePagePool &tiles,
                                    const MaterialLibrary &materials, gfx::Device *device) {
    const auto &data = asset.data();
    _mesh = GridMesh::createVTQuad(device);
    _material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-landscape-vt-compose"};
    _material->initialize(info);
    if (!_mesh || !_material->getPasses() || _material->getPasses()->empty()) {
        return false;
    }
    auto *pass = _material->getPasses()->front().get();
    const char *requiredProperties[] = {
        "sourceParams", "vtLayout", "splatmap", "albedoHeightMap", "normalRoughnessAOMap",
        "tilingParams", "globalColorMap", "globalColorParams", "heightBlendParams",
        "decalAlbedoMap", "decalNormalMap", "decalRegions", "decalIndexMap", "decalLayerParams",
        "terrainNormalMap", "normalSourceMap", "cliffSourceMap", "terrainHeightMap",
        "cliffParams", "cliffLayout", "cliffMaterialParams",
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
    bindComposeTexture("albedoHeightMap", materials.albedoHeight(), materials.sampler());
    bindComposeTexture("normalRoughnessAOMap", materials.normalRoughnessAO(), materials.sampler());
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

bool VTRenderer::initDecalResources(const LandscapeAsset &asset, const MaterialLibrary &materials, gfx::Device *device) {
    bindComposeTexture("decalAlbedoMap", materials.decalAlbedo(), materials.clampSampler());
    bindComposeTexture("decalNormalMap", materials.decalNormal(), materials.clampSampler());
    ccstd::vector<Vec4> decalRegions(config::DECAL_INSTANCE_MAX);
    const auto &decals = asset.decals();
    for (size_t i = 0; i < decals.size(); ++i) {
        const auto &d = decals[i];
        decalRegions[i] = Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)};
    }
    _decalRegions.assign(decalRegions.begin(), decalRegions.begin() + decals.size());
    ccstd::vector<Vec4> layerParams(config::DECAL_LIBRARY_MAX);
    for (size_t i = 0; i < asset.decalLayers().size(); ++i) {
        const auto &layer = asset.decalLayers()[i];
        layerParams[i] = Vec4{layer.modulateColor ? 1.0F : 0.0F,
            static_cast<float>(layer.detailMaterial0 + 1), static_cast<float>(layer.detailMaterial1 + 1), 0};
    }
    _material->setPropertyVec4Array("decalLayerParams", layerParams);
    gfx::TextureInfo indexInfo;
    indexInfo.type = gfx::TextureType::TEX2D;
    indexInfo.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    indexInfo.format = gfx::Format::RGBA8;
    indexInfo.width = config::DECAL_INSTANCE_MAX / 4;
    indexInfo.height = config::VT_PAGE_COUNT;
    _decalIndices = device->createTexture(indexInfo);
    if (!_decalIndices) return false;
    _decalIndexData.resize(config::VT_PAGE_COUNT * config::DECAL_INSTANCE_MAX, 0);
    bindComposeTexture("decalIndexMap", _decalIndices, materials.clampSampler());
    _material->setPropertyVec4Array("decalRegions", decalRegions);
    _material->setPropertyVec4("decalParams", Vec4{static_cast<float>(decals.size()), 0, 0, 0});
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
    if (!_normalSources) return false;
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
    _cliff.columns = data.sectorsX << (data.maxLevel - _cliff.level);
    _cliff.rows = data.sectorsZ << (data.maxLevel - _cliff.level);
    indexInfo.width = _cliff.columns;
    indexInfo.height = _cliff.rows;
    _cliff.texture = device->createTexture(indexInfo);
    if (!_cliff.texture) return false;
    _cliff.sources.resize(static_cast<size_t>(_cliff.columns) * _cliff.rows);
    bindComposeTexture("cliffSourceMap", _cliff.texture, tiles.splatSampler());
    const float referenceSize = computeNodeSize(data.sectorSize, data.maxLevel, _cliff.level);
    const auto &cliffMaterial = asset.cliffMaterial();
    _cliff.params = Vec4{0, data.heightScale, data.heightBias, static_cast<float>(cliffMaterial.layer + 1)};
    _material->setPropertyVec4("cliffMaterialParams", Vec4{cliffMaterial.maxNormalY,
        cliffMaterial.globalColorInfluence, 0, 0});
    _material->setPropertyVec4("cliffParams", _cliff.params);
    _material->setPropertyVec4("cliffLayout", Vec4{-data.worldWidth() * 0.5F,
        -data.worldDepth() * 0.5F, referenceSize, 0});
    return true;
}

bool VTRenderer::initDrawResources(gfx::Device *device) {
    auto *pass = _material->getPasses()->front().get();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-landscape-vt-compose"};
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
    _pipelineState = device->createPipelineState({shader, pass->getPipelineLayout(), _texture.renderPass(),
        {iaInfo.attributes}, *pass->getRasterizerState(), *pass->getDepthStencilState(),
        *pass->getBlendState(), pass->getPrimitive(), pass->getDynamicStates()});
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        // Technique 1 filters the completed pages, never re-evaluates materials.
        auto &mipMaterial = _mipPasses[level - 1].material;
        mipMaterial = ccnew Material();
        info.technique = 1;
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
        mipMaterial->setPropertyGFXTexture("sourceAlbedo", _texture.mipSourceAlbedo(level));
        mipMaterial->setPropertyGFXTexture("sourceNormal", _texture.mipSourceNormal(level));
        mipMaterial->setPropertyVec4("mipLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE >> (level - 1)),
            static_cast<float>(config::VT_PAGE_RES >> (level - 1)), static_cast<float>(level - 1),
            device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
        mipPass->update();
        auto *mipShader = mipPass->getShaderVariant();
        if (!mipShader) {
            return false;
        }
        _mipPasses[level - 1].pipelineState = device->createPipelineState({mipShader, mipPass->getPipelineLayout(), _texture.renderPass(),
            {iaInfo.attributes}, *mipPass->getRasterizerState(), *mipPass->getDepthStencilState(),
            *mipPass->getBlendState(), mipPass->getPrimitive(), mipPass->getDynamicStates()});
    }
    _commands = device->createCommandBuffer({device->getQueue(), gfx::CommandBufferType::PRIMARY});
    gfx::RenderPassInfo initialInfo;
    initialInfo.colorAttachments = _texture.renderPass()->getColorAttachments();
    for (auto &color : initialInfo.colorAttachments) {
        color.loadOp = gfx::LoadOp::CLEAR;
        color.barrier = device->getGeneralBarrier({gfx::AccessFlagBit::NONE,
            gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE});
    }
    _initialPass = device->createRenderPass(initialInfo);
    if (!valid() || !_initialPass || !_instances || !_inputAssembler) {
        return false;
    }
    return true;
}

bool VTRenderer::initRootPages(const LandscapeData &data, TilePagePool &tiles) {
    // Every sector has a real material fallback, independent of the visible set.
    // These dirty roots are composed during pass preparation, including the first
    // frame, before any terrain draw is submitted on the same graphics queue.
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            const uint64_t key = makeNodeKey(vtRootLevel(data.sectorSize), x, z);
            const int layer = tiles.peekResident(makeNodeKey(data.maxLevel, x, z));
            const Vec4 region{static_cast<float>(x) * data.sectorSize - data.worldWidth() * 0.5F,
                              static_cast<float>(z) * data.sectorSize - data.worldDepth() * 0.5F,
                              data.sectorSize, 0.0F};
            const Vec4 source{region.x, region.y, region.z, static_cast<float>(layer)};
            std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> normals;
            normals.fill(source);
            const VTPageRequest request{VTPageAddress{vtRootLevel(data.sectorSize), x, z}, 0.0F, key};
            const VTPageInputs inputs{region, source, normals};
            if (layer < 0 || _texture.acquirePage(request, inputs, true) < 0) {
                CC_LOG_ERROR("[Landscape] failed to prepare root VT page (%u,%u)", x, z);
                return false;
            }
        }
    }
    return true;
}

void VTRenderer::setGlobalColorMap(Texture2D *texture) {
    if (!_material) return;
    _globalColorMap = texture;
    auto *gfxTexture = texture ? texture->getGFXTexture() : nullptr;
    _hasGlobalColorMap = gfxTexture != nullptr;
    bindComposeTexture("globalColorMap", gfxTexture ? gfxTexture : _fallbackGlobalColorMap.get(), _globalColorSampler);
    _globalColorParams.z = _hasGlobalColorMap ? _globalColorStrength : 0.0F;
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    invalidateComposedPages();
}

void VTRenderer::setGlobalColorStrength(float strength) {
    if (!_material || !std::isfinite(strength)) return;
    _globalColorStrength = std::clamp(strength, 0.0F, 1.0F);
    const float effectiveStrength = _hasGlobalColorMap ? _globalColorStrength : 0.0F;
    if (_globalColorParams.z == effectiveStrength) return;
    _globalColorParams.z = effectiveStrength;
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    // Preserve the cached image while frozen; apply edits when updates resume.
    invalidateComposedPages();
}

void VTRenderer::invalidateComposedPages() {
    if (_debugData.freezeLod) {
        _pendingInvalidation = true;
    } else {
        _texture.invalidate();
    }
}

void VTRenderer::setFrozen(bool frozen) {
    _debugData.freezeLod = frozen;
    if (!frozen && _pendingInvalidation) {
        _texture.invalidate();
        _pendingInvalidation = false;
    }
}

void VTRenderer::setBakeNormalEnabled(bool enabled) {
    if (!_material || _debugData.bakeNormalEnabled == enabled) return;
    // The owner defers BOTH composition and decoding while pages are frozen.
    CC_ASSERT(!_debugData.freezeLod);
    _debugData.bakeNormalEnabled = enabled;
    auto *pass = _material->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("sourceParams")));
    params.y = enabled ? 1.0F : 0.0F;
    _material->setPropertyVec4("sourceParams", params);
    _texture.invalidate();
}

void VTRenderer::setCliffEnabled(bool enabled) {
    if (_debugData.cliffEnabled == enabled) return;
    CC_ASSERT(!_debugData.freezeLod);
    _debugData.cliffEnabled = enabled;
    // An authored material override needs the same slope mask with projection disabled. Keep
    // its references pinned; negative X means ready with planar projection.
    if (_cliff.params.w <= 0.0F) _cliff.ready = false;
    _cliff.params.x = _cliff.ready ? (enabled ? 1.0F : -1.0F) : 0.0F;
    if (_material) _material->setPropertyVec4("cliffParams", _cliff.params);
    _texture.invalidate();
}

void VTRenderer::syncCliffSources(TilePagePool &tiles) {
    if ((!_debugData.cliffEnabled && _cliff.params.w <= 0.0F) || _debugData.freezeLod || !_material) return;
    bool ready = true;
    const float size = computeNodeSize(_cliff.data.sectorSize, _cliff.data.maxLevel, _cliff.level);
    for (uint32_t z = 0; z < _cliff.rows; ++z) {
        for (uint32_t x = 0; x < _cliff.columns; ++x) {
            const int layer = tiles.query(_cliff.level, x, z);
            ready &= layer >= 0;
            _cliff.sources[z * _cliff.columns + x] = Vec4{
                x * size - _cliff.data.worldWidth() * 0.5F,
                z * size - _cliff.data.worldDepth() * 0.5F, size, static_cast<float>(layer)};
        }
    }
    if (_cliff.ready || !ready) return;
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
    _texture.invalidate();
    CC_LOG_INFO("[Landscape] Cliff reference ready: L%u, %ux%u tiles, %.2f m sample spacing",
        _cliff.level, _cliff.columns, _cliff.rows, size / (_cliff.data.tileResolution - 1U));
}

void VTRenderer::setHeightBlendEnabled(bool enabled) {
    if (!_material) return;
    auto *pass = _material->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("heightBlendParams")));
    const float strength = enabled ? 1.0F : 0.0F;
    if (params.x == strength) return;
    // Keep the effect's scale/sharpness when toggling the global blend mode.
    _debugData.heightBlendEnabled = enabled;
    params.x = strength;
    _material->setPropertyVec4("heightBlendParams", params);
    // Frozen pages can reference source tiles that are no longer resident.
    // Re-resolve them through the normal update path after unfreezing.
    invalidateComposedPages();
}

bool VTRenderer::valid() const {
    return _texture.valid() && _pipelineState && _commands &&
        std::all_of(_mipPasses.begin(), _mipPasses.end(),
            [](const MipPass &pass) { return pass.pipelineState != nullptr; });
}

void VTRenderer::render() {
    if (_debugData.freezeLod || !valid()) return;
    // The residency layer budgets fine updates and always includes dirty roots.
    // Fine pages become eligible for selection after this submission; roots
    // also bootstrap the first frame before any Base Pass can sample them.
    _texture.collectDirtyPages(_dirtySlots);
    if (_dirtySlots.empty()) return;
    buildPageBatch();
    uploadSourceTables();
    submitPageBatch();
    // Publish only after ALL levels have been submitted on the same queue.
    _texture.markRendered(_dirtySlots);
    _needsClear = false;
}

uint32_t VTRenderer::collectPageDecals(uint32_t slot, const Vec4 &region) {
    // Include filtering gutters and preserve asset order for alpha composition.
    const float border = region.z * static_cast<float>(config::VT_PAGE_BORDER) / config::VT_PAGE_INTERIOR;
    uint32_t count = 0;
    for (size_t i = 0; i < _decalRegions.size(); ++i) {
        const auto &decal = _decalRegions[i];
        if (decal.x > region.x + region.z + border || decal.y > region.y + region.z + border ||
            decal.x + decal.z < region.x - border || decal.y + decal.z < region.y - border) continue;
        _decalIndexData[slot * config::DECAL_INSTANCE_MAX + count++] = static_cast<uint8_t>(i);
    }
    return count;
}

void VTRenderer::buildPageBatch() {
    _pageInstances.clear();
    for (uint32_t slot : _dirtySlots) {
        const auto &inputs = _texture.page(slot).inputs;
        const uint32_t decalCount = collectPageDecals(slot, inputs.region);
        _pageInstances.push_back({Vec4{static_cast<float>(slot), static_cast<float>(decalCount), 0, 0},
                                  inputs.region, inputs.splatSource});
        std::copy(inputs.normalSources.begin(), inputs.normalSources.end(),
                  _normalSourceData.begin() + slot * config::VT_NORMAL_SOURCE_COUNT);
    }
}

void VTRenderer::uploadSourceTables() {
    gfx::BufferTextureCopy indexCopy;
    indexCopy.texExtent = {config::DECAL_INSTANCE_MAX / 4, config::VT_PAGE_COUNT, 1};
    const uint8_t *indexBytes[]{_decalIndexData.data()};
    Root::getInstance()->getDevice()->copyBuffersToTexture(indexBytes, _decalIndices, &indexCopy, 1);
    indexCopy.texExtent.width = config::VT_NORMAL_SOURCE_COUNT;
    const uint8_t *normalBytes[]{reinterpret_cast<const uint8_t *>(_normalSourceData.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(normalBytes, _normalSources, &indexCopy, 1);
}

void VTRenderer::recordPagePass(Material *material, gfx::PipelineState *pipelineState,
                                 gfx::Framebuffer *framebuffer, uint32_t atlasSize) {
    const gfx::Color colors[2] = {{0, 0, 0, 1}, {0.5F, 0.5F, 1, 1}};
    _commands->beginRenderPass(_needsClear ? _initialPass.get() : _texture.renderPass(), framebuffer,
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
    recordPagePass(_material, _pipelineState, _texture.framebuffer(), config::VT_ATLAS_SIZE);

    // Generate mip1 from mip0, then mip2 from mip1 for exactly the same slots.
    // Each pass makes its output shader-readable before the next downsample.
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        const auto &mip = _mipPasses[level - 1];
        recordPagePass(mip.material, mip.pipelineState, _texture.mipFramebuffer(level), config::VT_ATLAS_SIZE >> level);
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
    _decalIndices = nullptr;
    _decalRegions.clear();
    _decalIndexData.clear();
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
    for (auto &mip : _mipPasses) mip.pipelineState = nullptr;
    _initialPass = nullptr;
    _inputAssembler = nullptr;
    _instances = nullptr;
    if (_mesh) _mesh->destroy();
    _mesh = nullptr;
    if (_material) _material->destroy();
    _material = nullptr;
    for (auto &mip : _mipPasses) {
        if (mip.material) mip.material->destroy();
        mip.material = nullptr;
    }
    _texture.destroy();
    _dirtySlots.clear();
    _pageInstances.clear();
    _needsClear = true;
}
} // namespace landscape
} // namespace cc

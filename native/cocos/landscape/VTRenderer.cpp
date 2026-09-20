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

    _mesh = GridMesh::createVTQuad(device);
    _material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-landscape-vt-compose"};
    _material->initialize(info);
    if (!_mesh || !_material->getPasses() || _material->getPasses()->empty()) {
        destroy();
        return false;
    }
    auto *pass = _material->getPasses()->front().get();
    if (pass->getHandle("sourceParams") == 0U || pass->getHandle("vtLayout") == 0U ||
        pass->getHandle("splatmap") == 0U || pass->getHandle("albedoHeightMap") == 0U ||
        pass->getHandle("normalRoughnessAOMap") == 0U || pass->getHandle("tilingParams") == 0U ||
        pass->getHandle("globalColorMap") == 0U || pass->getHandle("globalColorParams") == 0U ||
        pass->getHandle("heightBlendParams") == 0U || pass->getHandle("decalAlbedoMap") == 0U ||
        pass->getHandle("decalNormalMap") == 0U || pass->getHandle("decalRegions") == 0U ||
        pass->getHandle("decalIndexMap") == 0U || pass->getHandle("decalLayerParams") == 0U ||
        pass->getHandle("terrainNormalMap") == 0U || pass->getHandle("normalSourceMap") == 0U ||
        pass->getHandle("cliffSourceMap") == 0U || pass->getHandle("terrainHeightMap") == 0U ||
        pass->getHandle("cliffParams") == 0U || pass->getHandle("cliffLayout") == 0U ||
        pass->getHandle("cliffMaterialParams") == 0U) {
        CC_LOG_WARNING("[Landscape] VT effect is outdated; reimport builtin-landscape effects and rebuild assets");
        destroy();
        return false;
    }
    auto bind = [this, pass](const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
        _material->setPropertyGFXTexture(name, texture);
        const auto handle = pass->getHandle(name);
        if (handle) pass->bindSampler(scene::Pass::getBindingFromHandle(handle), sampler);
    };
    bind("splatmap", tiles.splatArray(), tiles.splatSampler());
    bind("terrainNormalMap", tiles.normalArray(), tiles.heightSampler());
    bind("terrainHeightMap", tiles.heightArray(), tiles.heightSampler());
    bind("albedoHeightMap", materials.albedoHeight(), materials.sampler());
    bind("normalRoughnessAOMap", materials.normalRoughnessAO(), materials.sampler());
    bind("globalColorMap", materials.globalColorMap(), materials.globalColorSampler());
    bind("decalAlbedoMap", materials.decalAlbedo(), materials.globalColorSampler());
    bind("decalNormalMap", materials.decalNormal(), materials.globalColorSampler());
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
    bind("decalIndexMap", _decalIndices, materials.globalColorSampler());
    indexInfo.format = gfx::Format::RGBA32F;
    indexInfo.width = config::VT_NORMAL_SOURCE_COUNT;
    _normalSources = device->createTexture(indexInfo);
    if (!_normalSources) return false;
    _normalSourceData.resize(config::VT_PAGE_COUNT * config::VT_NORMAL_SOURCE_COUNT);
    bind("normalSourceMap", _normalSources, tiles.splatSampler());
    _cliffData = data;
    _cliffLevel = cliffReferenceLevel(data);
    _cliffColumns = data.sectorsX << (data.maxLevel - _cliffLevel);
    _cliffRows = data.sectorsZ << (data.maxLevel - _cliffLevel);
    indexInfo.width = _cliffColumns;
    indexInfo.height = _cliffRows;
    _cliffSources = device->createTexture(indexInfo);
    if (!_cliffSources) return false;
    _cliffSourceData.resize(static_cast<size_t>(_cliffColumns) * _cliffRows);
    bind("cliffSourceMap", _cliffSources, tiles.splatSampler());
    const float referenceSize = computeNodeSize(data.sectorSize, data.maxLevel, _cliffLevel);
    const auto &cliffMaterial = asset.cliffMaterial();
    _cliffParams = Vec4{0, data.heightScale, data.heightBias, static_cast<float>(cliffMaterial.layer + 1)};
    _material->setPropertyVec4("cliffMaterialParams", Vec4{cliffMaterial.maxNormalY,
        cliffMaterial.globalColorInfluence, 0, 0});
    _material->setPropertyVec4("cliffParams", _cliffParams);
    _material->setPropertyVec4("cliffLayout", Vec4{-data.worldWidth() * 0.5F,
        -data.worldDepth() * 0.5F, referenceSize, 0});
    _material->setPropertyVec4Array("decalRegions", decalRegions);
    _material->setPropertyVec4("decalParams", Vec4{static_cast<float>(decals.size()), 0, 0, 0});
    _hasGlobalColorMap = materials.hasGlobalColorMap();
    _globalColorParams = Vec4{1.0F / data.worldWidth(),
        1.0F / data.worldDepth(), materials.globalColorStrength(), 0.0F};
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    _material->setPropertyVec4Array("tilingParams", materials.tilingParams());
    _material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE),
        static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER),
        device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
    _material->setPropertyVec4("sourceParams", Vec4{static_cast<float>(asset.data().tileResolution), _rvtNormalEnabled ? 1.0F : 0.0F, 0, 0});
    pass->update();

    constexpr uint32_t STRIDE = 12 * sizeof(float);
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
        destroy();
        return false;
    }
    _pipelineState = device->createPipelineState({shader, pass->getPipelineLayout(), _texture.renderPass(),
        {iaInfo.attributes}, *pass->getRasterizerState(), *pass->getDepthStencilState(),
        *pass->getBlendState(), pass->getPrimitive(), pass->getDynamicStates()});
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        // Technique 1 filters the completed pages, never re-evaluates materials.
        auto &mipMaterial = _mipMaterials[level - 1];
        mipMaterial = ccnew Material();
        info.technique = 1;
        mipMaterial->initialize(info);
        if (!mipMaterial->getPasses() || mipMaterial->getPasses()->empty()) {
            destroy();
            return false;
        }
        auto *mipPass = mipMaterial->getPasses()->front().get();
        if (!mipPass->getHandle("mipLayout") || !mipPass->getHandle("sourceAlbedo") || !mipPass->getHandle("sourceNormal")) {
            CC_LOG_ERROR("[Landscape] VT mip technique is missing; rebuild project effects");
            destroy();
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
            destroy();
            return false;
        }
        _mipPipelineStates[level - 1] = device->createPipelineState({mipShader, mipPass->getPipelineLayout(), _texture.renderPass(),
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
        destroy();
        return false;
    }
    // Every sector has a real material fallback, independent of the visible set.
    // These dirty roots are always composed by BeforeRender, including the first
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
            if (layer < 0 || _texture.acquirePage(key, region, source, normals, true) < 0) {
                CC_LOG_ERROR("[Landscape] failed to prepare root VT page (%u,%u)", x, z);
                destroy();
                return false;
            }
        }
    }
    _dirtySlots.reserve(config::VT_PAGE_COUNT);
    _instanceData.reserve(config::VT_PAGE_COUNT * 12);
    // After device acquire / scene updates, before ANY camera's Base Pass.
    // No extra camera, visibility layer, scene models or forward-pipeline fork.
    _beforeRender = root->on<Root::BeforeRender>([this](Root *) { render(); });
    _subscribed = true;
#if CC_LANDSCAPE_DEBUG
    CC_LOG_INFO("[Landscape] VT enabled: %u cached pages, %u interior texels, two RGBA8 atlases with mip0 + mip1 + mip2",
        config::VT_PAGE_COUNT, config::VT_PAGE_INTERIOR);
#endif
    return true;
}

void VTRenderer::setGlobalColorStrength(float strength) {
    if (!_material || !std::isfinite(strength)) return;
    const float effectiveStrength = _hasGlobalColorMap ? std::clamp(strength, 0.0F, 1.0F) : 0.0F;
    if (_globalColorParams.z == effectiveStrength) return;
    _globalColorParams.z = effectiveStrength;
    _material->setPropertyVec4("globalColorParams", _globalColorParams);
    // Preserve the cached image while frozen; apply edits when updates resume.
    if (_frozen) {
        _pendingInvalidation = true;
    } else {
        _texture.invalidate();
    }
}

void VTRenderer::setFrozen(bool frozen) {
    _frozen = frozen;
    if (!frozen && _pendingInvalidation) {
        _texture.invalidate();
        _pendingInvalidation = false;
    }
}

void VTRenderer::setRVTNormalEnabled(bool enabled) {
    if (!_material || _rvtNormalEnabled == enabled) return;
    // The owner defers BOTH composition and decoding while pages are frozen.
    CC_ASSERT(!_frozen);
    _rvtNormalEnabled = enabled;
    auto *pass = _material->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("sourceParams")));
    params.y = enabled ? 1.0F : 0.0F;
    _material->setPropertyVec4("sourceParams", params);
    _texture.invalidate();
}

void VTRenderer::setCliffEnabled(bool enabled) {
    if (_cliffEnabled == enabled) return;
    CC_ASSERT(!_frozen);
    _cliffEnabled = enabled;
    // An authored material override needs the same slope mask with projection disabled. Keep
    // its references pinned; negative X means ready with planar projection.
    if (_cliffParams.w <= 0.0F) _cliffReady = false;
    _cliffParams.x = _cliffReady ? (enabled ? 1.0F : -1.0F) : 0.0F;
    if (_material) _material->setPropertyVec4("cliffParams", _cliffParams);
    _texture.invalidate();
}

void VTRenderer::syncCliffSources(TilePagePool &tiles) {
    if ((!_cliffEnabled && _cliffParams.w <= 0.0F) || _frozen || !_material) return;
    bool ready = true;
    const float size = computeNodeSize(_cliffData.sectorSize, _cliffData.maxLevel, _cliffLevel);
    for (uint32_t z = 0; z < _cliffRows; ++z) {
        for (uint32_t x = 0; x < _cliffColumns; ++x) {
            const int layer = tiles.query(_cliffLevel, x, z);
            ready &= layer >= 0;
            _cliffSourceData[z * _cliffColumns + x] = Vec4{
                x * size - _cliffData.worldWidth() * 0.5F,
                z * size - _cliffData.worldDepth() * 0.5F, size, static_cast<float>(layer)};
        }
    }
    if (_cliffReady || !ready) return;
    // Publish once, after the entire fixed reference is resident. No per-camera
    // source LOD switches, partial-resolution seams or repeated VT invalidation.
    gfx::BufferTextureCopy region;
    region.texExtent.width = _cliffColumns;
    region.texExtent.height = _cliffRows;
    region.texExtent.depth = 1;
    const uint8_t *buffers[]{reinterpret_cast<const uint8_t *>(_cliffSourceData.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(buffers, _cliffSources, &region, 1);
    _cliffReady = true;
    _cliffParams.x = _cliffEnabled ? 1.0F : -1.0F;
    _material->setPropertyVec4("cliffParams", _cliffParams);
    _texture.invalidate();
    CC_LOG_INFO("[Landscape] Cliff reference ready: L%u, %ux%u tiles, %.2f m sample spacing",
        _cliffLevel, _cliffColumns, _cliffRows, size / (_cliffData.tileResolution - 1U));
}

void VTRenderer::setHeightBlendEnabled(bool enabled) {
    if (!_material) return;
    auto *pass = _material->getPasses()->front().get();
    auto params = ccstd::get<Vec4>(pass->getUniform(pass->getHandle("heightBlendParams")));
    const float strength = enabled ? 1.0F : 0.0F;
    if (params.x == strength) return;
    // Keep the effect's scale/sharpness when toggling the global blend mode.
    params.x = strength;
    _material->setPropertyVec4("heightBlendParams", params);
    // Frozen pages can reference source tiles that are no longer resident.
    // Re-resolve them through the normal update path after unfreezing.
    if (_frozen) {
        _pendingInvalidation = true;
    } else {
        _texture.invalidate();
    }
}

bool VTRenderer::valid() const {
    return _texture.valid() && _pipelineState && _commands &&
        std::all_of(_mipPipelineStates.begin(), _mipPipelineStates.end(),
            [](const auto &state) { return state != nullptr; });
}

void VTRenderer::render() {
    if (_frozen || !valid()) return;
    // The residency layer budgets fine updates and always includes dirty roots.
    // Fine pages become eligible for selection after this submission; roots
    // also bootstrap the first frame before any Base Pass can sample them.
    _texture.collectDirtyPages(_dirtySlots);
    if (_dirtySlots.empty()) return;
    _instanceData.clear();
    for (uint32_t slot : _dirtySlots) {
        const auto &page = _texture.page(slot);
        // Include filtering gutters and retain the asset-defined compositing order.
        const float border = page.region.z * static_cast<float>(config::VT_PAGE_BORDER) / config::VT_PAGE_INTERIOR;
        uint32_t count = 0;
        for (size_t i = 0; i < _decalRegions.size(); ++i) {
            const auto &d = _decalRegions[i];
            if (d.x > page.region.x + page.region.z + border || d.y > page.region.y + page.region.z + border ||
                d.x + d.z < page.region.x - border || d.y + d.z < page.region.y - border) continue;
            _decalIndexData[slot * config::DECAL_INSTANCE_MAX + count++] = static_cast<uint8_t>(i);
        }
        _instanceData.insert(_instanceData.end(), {static_cast<float>(slot), static_cast<float>(count), 0, 0,
            page.region.x, page.region.y, page.region.z, page.region.w,
            page.source.x, page.source.y, page.source.z, page.source.w});
        std::copy(page.normalSources.begin(), page.normalSources.end(),
                  _normalSourceData.begin() + slot * config::VT_NORMAL_SOURCE_COUNT);
    }
    auto *pass = _material->getPasses()->front().get();
    gfx::BufferTextureCopy indexCopy;
    indexCopy.texExtent = {config::DECAL_INSTANCE_MAX / 4, config::VT_PAGE_COUNT, 1};
    const uint8_t *indexBytes[]{_decalIndexData.data()};
    Root::getInstance()->getDevice()->copyBuffersToTexture(indexBytes, _decalIndices, &indexCopy, 1);
    indexCopy.texExtent.width = config::VT_NORMAL_SOURCE_COUNT;
    const uint8_t *normalBytes[]{reinterpret_cast<const uint8_t *>(_normalSourceData.data())};
    Root::getInstance()->getDevice()->copyBuffersToTexture(normalBytes, _normalSources, &indexCopy, 1);
    pass->update();
    _commands->begin();
    _commands->updateBuffer(_instances, _instanceData.data(), static_cast<uint32_t>(_instanceData.size() * sizeof(float)));
    const gfx::Color colors[2] = {{0, 0, 0, 1}, {0.5F, 0.5F, 1, 1}};
    _commands->beginRenderPass(_needsClear ? _initialPass.get() : _texture.renderPass(), _texture.framebuffer(),
        gfx::Rect{0, 0, config::VT_ATLAS_SIZE, config::VT_ATLAS_SIZE}, colors, 1.0F, 0);
    _commands->bindPipelineState(_pipelineState);
    _commands->bindDescriptorSet(static_cast<uint32_t>(pipeline::SetIndex::MATERIAL), pass->getDescriptorSet());
    _commands->bindInputAssembler(_inputAssembler);
    auto draw = _inputAssembler->getDrawInfo();
    draw.instanceCount = static_cast<uint32_t>(_dirtySlots.size());
    _commands->draw(draw);
    _commands->endRenderPass();
    // Each pass makes its output shader-readable before the next downsample.
    // Separate material descriptors keep both recorded source bindings intact.
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        auto *mipPass = _mipMaterials[level - 1]->getPasses()->front().get();
        const uint32_t size = config::VT_ATLAS_SIZE >> level;
        _commands->beginRenderPass(_needsClear ? _initialPass.get() : _texture.renderPass(), _texture.mipFramebuffer(level),
            gfx::Rect{0, 0, size, size}, colors, 1.0F, 0);
        _commands->bindPipelineState(_mipPipelineStates[level - 1]);
        _commands->bindDescriptorSet(static_cast<uint32_t>(pipeline::SetIndex::MATERIAL), mipPass->getDescriptorSet());
        _commands->bindInputAssembler(_inputAssembler);
        _commands->draw(draw);
        _commands->endRenderPass();
    }
    _commands->end();
    gfx::CommandBuffer *command = _commands.get();
    auto *device = Root::getInstance()->getDevice();
    device->flushCommands(&command, 1);
    device->getQueue()->submit(&command, 1);
    // Publish only after ALL levels have been submitted on the same queue.
    _texture.markRendered(_dirtySlots);
    _needsClear = false;
}

void VTRenderer::destroy() {
    _cliffSources = nullptr;
    _cliffSourceData.clear();
    _cliffReady = false;
    _normalSources = nullptr;
    _normalSourceData.clear();
    _decalIndices = nullptr;
    _decalRegions.clear();
    _decalIndexData.clear();
    _frozen = false;
    _pendingInvalidation = false;
    _hasGlobalColorMap = false;
    _globalColorParams = Vec4{};
    if (_subscribed && Root::getInstance()) Root::getInstance()->off(_beforeRender);
    _subscribed = false;
    _commands = nullptr;
    _pipelineState = nullptr;
    for (auto &state : _mipPipelineStates) state = nullptr;
    _initialPass = nullptr;
    _inputAssembler = nullptr;
    _instances = nullptr;
    if (_mesh) _mesh->destroy();
    _mesh = nullptr;
    if (_material) _material->destroy();
    _material = nullptr;
    for (auto &material : _mipMaterials) {
        if (material) material->destroy();
        material = nullptr;
    }
    _texture.destroy();
    _dirtySlots.clear();
    _instanceData.clear();
    _needsClear = true;
}
} // namespace landscape
} // namespace cc

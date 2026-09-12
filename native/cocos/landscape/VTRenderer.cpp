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

#include "base/Log.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "landscape/GridMesh.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "renderer/gfx-base/GFXCommandBuffer.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXInputAssembler.h"
#include "renderer/gfx-base/GFXPipelineState.h"
#include "renderer/gfx-base/GFXQueue.h"
#include "renderer/gfx-base/GFXRenderPass.h"
#include "scene/Pass.h"

namespace cc {
namespace landscape {
VTRenderer::VTRenderer() = default;
VTRenderer::~VTRenderer() { destroy(); }

bool VTRenderer::init(const LandscapeAsset &asset, const TilePagePool &tiles, const MaterialLibrary &materials) {
    destroy();
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
        pass->getHandle("normalRoughnessAOMap") == 0U) {
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
    bind("albedoHeightMap", materials.albedoHeight(), materials.sampler());
    bind("normalRoughnessAOMap", materials.normalRoughnessAO(), materials.sampler());
    ccstd::vector<Vec4> detailParams(config::MATERIAL_LIBRARY_MAX);
    for (const auto &layer : asset.materialLayers()) {
        detailParams[layer.id] = Vec4{layer.uvScale, layer.detailHeightScale, layer.detailHeightBias, 0.0F};
    }
    _material->setPropertyVec4Array("detailParams", detailParams);
    _material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE),
        static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER),
        device->getCapabilities().screenSpaceSignY * device->getCapabilities().clipSpaceSignY});
    _material->setPropertyVec4("sourceParams", Vec4{static_cast<float>(asset.data().tileResolution), 0, 0, 0});
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
    _commands = device->createCommandBuffer({device->getQueue(), gfx::CommandBufferType::PRIMARY});
    gfx::RenderPassInfo initialInfo;
    initialInfo.colorAttachments = _texture.renderPass()->getColorAttachments();
    for (auto &color : initialInfo.colorAttachments) {
        color.loadOp = gfx::LoadOp::CLEAR;
        color.barrier = device->getGeneralBarrier({gfx::AccessFlagBit::NONE,
            gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE});
    }
    _initialPass = device->createRenderPass(initialInfo);
    _dirtySlots.reserve(config::VT_PAGE_COUNT);
    _instanceData.reserve(config::VT_PAGE_COUNT * 12);
    // After device acquire / scene updates, before ANY camera's Base Pass.
    // No extra camera, visibility layer, scene models or forward-pipeline fork.
    _beforeRender = root->on<Root::BeforeRender>([this](Root *) { render(); });
    _subscribed = true;
    CC_LOG_INFO("[Landscape] VT enabled: %u cached pages, %u interior texels, two RGBA8 atlases",
        config::VT_PAGE_COUNT, config::VT_PAGE_INTERIOR);
    return true;
}

bool VTRenderer::valid() const {
    return _texture.valid() && _pipelineState && _commands;
}

void VTRenderer::render() {
    if (!valid()) return;
    _texture.collectDirtyPages(_dirtySlots);
    if (_dirtySlots.empty()) return;
    _instanceData.clear();
    for (uint32_t slot : _dirtySlots) {
        const auto &page = _texture.page(slot);
        _instanceData.insert(_instanceData.end(), {static_cast<float>(slot), 0, 0, 0,
            page.region.x, page.region.y, page.region.z, page.region.w,
            page.source.x, page.source.y, page.source.z, page.source.w});
    }
    auto *pass = _material->getPasses()->front().get();
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
    _commands->end();
    gfx::CommandBuffer *command = _commands.get();
    auto *device = Root::getInstance()->getDevice();
    device->flushCommands(&command, 1);
    device->getQueue()->submit(&command, 1);
    // Same graphics queue: subsequent Base Pass reads follow this submission.
    _texture.markRendered(_dirtySlots);
    _needsClear = false;
}

void VTRenderer::destroy() {
    if (_subscribed && Root::getInstance()) Root::getInstance()->off(_beforeRender);
    _subscribed = false;
    _commands = nullptr;
    _pipelineState = nullptr;
    _initialPass = nullptr;
    _inputAssembler = nullptr;
    _instances = nullptr;
    if (_mesh) _mesh->destroy();
    _mesh = nullptr;
    if (_material) _material->destroy();
    _material = nullptr;
    _texture.destroy();
    _dirtySlots.clear();
    _instanceData.clear();
    _needsClear = true;
}
} // namespace landscape
} // namespace cc

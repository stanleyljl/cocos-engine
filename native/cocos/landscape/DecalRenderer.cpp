/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

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

#include "landscape/DecalRenderer.h"

#include <algorithm>
#include <cmath>
#include "base/Log.h"
#include "base/Macros.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/scene-graph/Node.h"
#include "landscape/GridMesh.h"
#include "landscape/MaterialLibrary.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "renderer/pipeline/InstancedBuffer.h"
#include "renderer/pipeline/PipelineStateManager.h"
#include "scene/Model.h"
#include "scene/Pass.h"
#include "scene/RenderScene.h"
#include "scene/RenderWindow.h"

namespace cc {
namespace landscape {

static Material *createDecalMaterial(bool wireframe, bool heightUnorm) {
    auto *material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"landscape/builtin-landscape-decal"};
    MacroRecord defines;
    defines["USE_INSTANCING"] = true;
    defines["LANDSCAPE_DEBUG_UNLIT"] = false;
    defines["LANDSCAPE_HEIGHT_UNORM"] = heightUnorm;
    // Compile debug edges out of the normal material entirely.
    defines["LANDSCAPE_WIREFRAME"] = wireframe;
    info.defines = IMaterialInfo::DefinesType{defines};

    if (wireframe) {
        RasterizerStateInfo rasterizer;
        rasterizer.cullMode = gfx::CullMode::NONE;
        PassOverrides overrides;
        overrides.rasterizerState = rasterizer;
        info.states = IMaterialInfo::PassOverridesType{overrides};
    }

    material->initialize(info);
    return material;
}

static void setLandscapeInstanceAttribute(scene::Model *model, TypedArray &scratch, const char *name, const Vec4 &value) {
    // Model copies synchronously. Reusing this buffer avoids per-draw ArrayBuffers.
    auto &instance = ccstd::get<Float32Array>(scratch);
    instance[0] = value.x;
    instance[1] = value.y;
    instance[2] = value.z;
    instance[3] = value.w;
    model->setInstancedAttribute(name, scratch);
}

void LandscapeGridParams::apply(scene::Model *model, TypedArray &scratch) const {
    setLandscapeInstanceAttribute(model, scratch, "a_gridInst", grid);
    setLandscapeInstanceAttribute(model, scratch, "a_tileInst", tile);
    setLandscapeInstanceAttribute(model, scratch, "a_mappingInst", mapping);
}

DecalRenderer::DecalRenderer() = default;
DecalRenderer::~DecalRenderer() {
    for (auto &entry : _nodes) {
        for (auto &draw : entry.second.draws) {
            recycle(draw);
        }
    }
    for (const auto &model : _pool) {
        model->detachFromScene();
        model->destroy();
    }
}

bool DecalRenderer::init(Node *node, scene::RenderScene *scene, bool heightUnorm) {
    _node = node;
    _scene = scene;
    _solid = createDecalMaterial(false, heightUnorm);
    _wire = createDecalMaterial(true, heightUnorm);
    _mesh = GridMesh::create(Root::getInstance()->getDevice(), 16);
    _scratch = Float32Array(4);
    if (!_mesh || !_solid || !_wire ||
        !_solid->getPasses() || _solid->getPasses()->empty() ||
        !_wire->getPasses() || _wire->getPasses()->empty()) {
        return false;
    }
    // Prepare model shader variants and the instanced pipeline before the first decal appears.
    // The disabled model stays in the pool for the first real draw.
    _pool.push_back(createModel());
    compilePipelines();
    return true;
}

std::array<Material *, 2> DecalRenderer::materials() const {
    return {_solid.get(), _wire.get()};
}

bool DecalRenderer::setAsset(LandscapeAsset *asset, gfx::Device *device) {
    _asset = asset;
    for (auto &entry : _nodes) {
        for (auto &draw : entry.second.draws) {
            recycle(draw);
        }
    }
    _nodes.clear();
    _instances.clear();
    for (const auto &decal : asset->decals()) {
        auto &instance = _instances.emplace_back();
        instance.decal = decal;
        initializeInstance(instance);
    }
    _regionsDirty = true;
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = gfx::Format::RGBA8;
    // RG and BA each store a 16-bit instance index, low byte first.
    info.width = config::DECAL_INSTANCE_MAX / 2;
    info.height = config::VT_PAGE_COUNT;
    _pageIndexTexture = device->createTexture(info);
    _pageIndexData.resize(config::VT_PAGE_COUNT * config::DECAL_INSTANCE_MAX * 2, 0);
    return _pageIndexTexture != nullptr;
}

void DecalRenderer::initializeInstance(Instance &instance) {
    const auto &data = _asset->data();
    const auto &d = instance.decal;
    const float step = data.nodeSize(0) / 16.0F;
    const float x = d.x + d.size * 0.5F;
    const float z = d.z + d.size * 0.5F;
    float minY = data.minHeight();
    float maxY = data.maxHeight();
    const auto address = data.nodeAtClamped(0, {x, z});
    _asset->getHeightRange(address.level, address.x, address.z, minY, maxY);
    // Stable whole-decal fade, independent of geometry LOD and streamed heights.
    instance.center = {x, (minY + maxY) * 0.5F, z};
    if (_asset->decalLayers()[d.layer].heightScale == 0.0F) {
        return;
    }
    // The fixed lattice is content data. Camera and terrain LOD changes
    // only select existing blocks; they never regenerate their rectangles.
    instance.firstX = static_cast<int>(std::floor(std::max(d.x, 0.0F) / step));
    instance.firstZ = static_cast<int>(std::floor(std::max(d.z, 0.0F) / step));
    const int endX = static_cast<int>(std::ceil(std::min(d.x + d.size, data.width()) / step));
    const int endZ = static_cast<int>(std::ceil(std::min(d.z + d.size, data.depth()) / step));
    instance.columns = std::max(0, endX - instance.firstX);
    for (int iz = instance.firstZ; iz < endZ; ++iz) {
        for (int ix = instance.firstX; ix < endX; ++ix) {
            const float left = std::max(ix * step, d.x);
            const float bottom = std::max(iz * step, d.z);
            const float right = std::min((ix + 1) * step, d.x + d.size);
            const float top = std::min((iz + 1) * step, d.z + d.size);
            instance.grids.emplace_back(left, bottom, right - left, top - bottom);
        }
    }
}

bool DecalRenderer::addDecal(uint32_t id, const LandscapeAsset::Decal &decal) {
    if (id == 0 || _instances.size() >= config::DECAL_INSTANCE_MAX) {
        return false;
    }
    const auto &data = _asset->data();
    if (decal.layer >= _asset->decalLayers().size() || decal.size <= 0.0F || decal.size > 64.0F ||
        decal.x < 0.0F || decal.z < 0.0F ||
        decal.x + decal.size > data.width() || decal.z + decal.size > data.depth()) {
        return false;
    }
    auto &instance = _instances.emplace_back();
    instance.decal = decal;
    instance.id = id;
    initializeInstance(instance);
    _regionsDirty = true;
    updateView(_viewPosition);
    return true;
}

bool DecalRenderer::removeDecal(uint32_t id, LandscapeRegion &region) {
    if (id == 0) {
        return false;
    }
    const auto it = std::find_if(_instances.begin(), _instances.end(), [id](const Instance &instance) {
        return instance.id == id;
    });
    if (it == _instances.end()) {
        return false;
    }
    region = {it->decal.x, it->decal.z, it->decal.size};
    const auto index = static_cast<uint32_t>(it - _instances.begin());
    for (auto &entry : _nodes) {
        auto &draws = entry.second.draws;
        draws.erase(std::remove_if(draws.begin(), draws.end(), [this, index](Draw &draw) {
            if (draw.decal == index) {
                recycle(draw);
                return true;
            }
            // Compact indices without rebuilding other instances or their models.
            if (draw.decal > index) {
                --draw.decal;
            }
            return false;
        }), draws.end());
    }
    _instances.erase(it);
    _regionsDirty = true;
    return true;
}

bool DecalRenderer::bindComposeMaterial(Material *material, const MaterialLibrary &materials) {
    const auto &pass = material->getPasses()->front();
    for (const char *name : {"decalAlbedoMap", "decalNormalMap", "decalRegions",
                             "decalIndexMap", "decalLayerParams", "decalParams"}) {
        if (!pass->getHandle(name)) {
            CC_LOG_WARNING("[Landscape] VT effect is missing %s; rebuild project effects", name);
            return false;
        }
    }
    const auto bind = [&](const char *name, gfx::Texture *texture) {
        material->setPropertyGFXTexture(name, texture);
        for (const auto &composePass : *material->getPasses()) {
            composePass->bindSampler(scene::Pass::getBindingFromHandle(composePass->getHandle(name)), materials.clampSampler());
        }
    };
    bind("decalAlbedoMap", materials.decalAlbedo());
    bind("decalNormalMap", materials.decalNormal());
    bind("decalIndexMap", _pageIndexTexture);
    ccstd::vector<Vec4> layers(config::DECAL_LIBRARY_MAX);
    for (size_t i = 0; i < _asset->decalLayers().size(); ++i) {
        const auto &layer = _asset->decalLayers()[i];
        layers[i] = Vec4{layer.modulateColor ? 1.0F : 0.0F,
                         static_cast<float>(layer.detailMaterial0 + 1), static_cast<float>(layer.detailMaterial1 + 1), 0};
    }
    material->setPropertyVec4Array("decalLayerParams", layers);
    _regionsDirty = true;
    updateComposeMaterial(material);
    return true;
}

void DecalRenderer::updateComposeMaterial(Material *material) {
    if (!_regionsDirty) {
        return;
    }
    ccstd::vector<Vec4> regions(config::DECAL_INSTANCE_MAX);
    for (size_t i = 0; i < _instances.size(); ++i) {
        const auto &d = _instances[i].decal;
        regions[i] = Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)};
    }
    material->setPropertyVec4Array("decalRegions", regions);
    material->setPropertyVec4("decalParams", Vec4{static_cast<float>(_instances.size()), 0, 0, 0});
    _regionsDirty = false;
}

uint32_t DecalRenderer::collectPageDecals(uint32_t slot, const Vec4 &region) {
    // Both flat and displaced decals contribute surface color/normal to VT.
    // Include filtering gutters and preserve authored order followed by runtime insertion order.
    const float border = region.z * static_cast<float>(config::VT_PAGE_BORDER) / config::VT_PAGE_INTERIOR;
    uint32_t count = 0;
    for (size_t i = 0; i < _instances.size(); ++i) {
        const auto &d = _instances[i].decal;
        if (d.x > region.x + region.z + border || d.z > region.y + region.z + border ||
            d.x + d.size < region.x - border || d.z + d.size < region.y - border) {
            continue;
        }
        const size_t offset = (slot * config::DECAL_INSTANCE_MAX + count++) * 2;
        _pageIndexData[offset] = static_cast<uint8_t>(i & 0xFFU);
        _pageIndexData[offset + 1] = static_cast<uint8_t>(i >> 8U);
    }
    return count;
}

void DecalRenderer::uploadPageDecals() {
    gfx::BufferTextureCopy copy;
    copy.texExtent = {config::DECAL_INSTANCE_MAX / 2, config::VT_PAGE_COUNT, 1};
    const uint8_t *bytes[]{_pageIndexData.data()};
    Root::getInstance()->getDevice()->copyBuffersToTexture(bytes, _pageIndexTexture, &copy, 1);
}

void DecalRenderer::updateBinding(const Draw &draw, const TerrainNode &node, bool boundsChanged) {
    node.gridParams.apply(draw.model, _scratch);
    if (boundsChanged) {
        const auto &d = _instances[draw.decal].decal;
        const float lift = _asset->decalLayers()[d.layer].heightScale;
        draw.model->createBoundingShape(Vec3{draw.grid.x, node.minY, draw.grid.y},
                                       Vec3{draw.grid.x + draw.grid.z, node.maxY + lift, draw.grid.y + draw.grid.w});
        draw.model->updateWorldBound();
    }
}

void DecalRenderer::updateInstance(const Draw &draw, const TerrainNode &node) {
    updateBinding(draw, node, true);
    const auto &d = _instances[draw.decal].decal;
    const auto &layer = _asset->decalLayers()[d.layer];
    setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalRegion", Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)});
    setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalGrid", draw.grid);
    setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalFade", Vec4{layer.heightScale, _instances[draw.decal].fade, 0, 0});
}

void DecalRenderer::appendDecal(NodeDraws &node, uint32_t decal) {
    const auto &instance = _instances[decal];
    const auto &d = _instances[decal].decal;
    const auto &region = node.terrain.region;
    if (region.x + region.size <= d.x || region.z + region.size <= d.z ||
        region.x >= d.x + d.size || region.z >= d.z + d.size) {
        return;
    }
    const size_t firstDraw = node.draws.size();
    const float step = _asset->data().nodeSize(0) / 16.0F;
    const int firstX = static_cast<int>(std::floor(std::max(region.x, d.x) / step));
    const int firstZ = static_cast<int>(std::floor(std::max(region.z, d.z) / step));
    const int endX = static_cast<int>(std::ceil(std::min(region.x + region.size, d.x + d.size) / step));
    const int endZ = static_cast<int>(std::ceil(std::min(region.z + region.size, d.z + d.size) / step));
    for (int iz = firstZ; iz < endZ; ++iz) {
        for (int ix = firstX; ix < endX; ++ix) {
            IntrusivePtr<scene::Model> model;
            if (_pool.empty()) {
                model = createModel();
            } else {
                model = std::move(_pool.back());
                _pool.pop_back();
            }
            const size_t index = static_cast<size_t>(iz - instance.firstZ) * instance.columns + ix - instance.firstX;
            CC_ASSERT(index < instance.grids.size());
            auto &draw = node.draws.emplace_back(Draw{model, decal, instance.grids[index]});
            updateInstance(draw, node.terrain);
            model->setEnabled(_enabled);
        }
    }
    // Preserve composition order when a lower-index decal enters range later.
    std::inplace_merge(node.draws.begin(), node.draws.begin() + firstDraw, node.draws.end(),
                       [](const Draw &a, const Draw &b) { return a.decal < b.decal; });
}

IntrusivePtr<scene::Model> DecalRenderer::createModel() {
    auto *model = Root::getInstance()->createModel<scene::Model>();
    model->setNode(_node);
    model->setTransform(_node);
    model->setCastShadow(_castShadow);
    model->setReceiveShadow(_receiveShadow);
    model->initSubModel(0, _mesh, _wireframe ? _wire.get() : _solid.get());
    model->setEnabled(false);
    model->attachToScene(_scene);
    return model;
}

void DecalRenderer::compilePipelines() {
    if (_pool.empty()) {
        return;
    }
    auto *subModel = _pool.back()->getSubModels().front().get();
    auto *renderPass = Root::getInstance()->getMainWindow()->getFramebuffer()->getRenderPass();
    const auto &passes = *subModel->getPasses();
    for (uint32_t i = 0; i < passes.size(); ++i) {
        // Use the real instance layout without adding a draw to a live render queue.
        // Creating shader objects alone does not compile programs on GLES3.
        pipeline::InstancedBuffer instances(passes[i]);
        instances.merge(subModel, i);
        for (const auto &instance : instances.getInstances()) {
            pipeline::PipelineStateManager::getOrCreatePipelineState(passes[i], instance.shader, instance.ia, renderPass);
        }
    }
}

void DecalRenderer::recycle(Draw &draw) {
    draw.model->setEnabled(false);
    _pool.push_back(std::move(draw.model));
}

void DecalRenderer::removeNode(uint64_t nodeKey) {
    const auto it = _nodes.find(nodeKey);
    if (it == _nodes.end()) {
        return;
    }
    for (auto &draw : it->second.draws) {
        recycle(draw);
    }
    _nodes.erase(it);
}

void DecalRenderer::updateNode(const TerrainNode &node) {
    auto result = _nodes.try_emplace(node.nodeKey);
    auto &entry = result.first->second;
    const bool boundsChanged = result.second || entry.terrain.minY != node.minY || entry.terrain.maxY != node.maxY;
    entry.terrain = node;
    if (result.second) {
        for (uint32_t i = 0; i < _instances.size(); ++i) {
            if (_instances[i].active) {
                appendDecal(entry, i);
            }
        }
    } else {
        for (const auto &draw : entry.draws) {
            updateBinding(draw, node, boundsChanged);
        }
    }
}

void DecalRenderer::updateView(const Vec3 &viewPosition) {
    _viewPosition = viewPosition;
    const Vec3 localView = viewPosition - _node->getWorldPosition();
    enum class ViewChange : uint8_t {
        NONE,
        FADE,
        ENTER,
    };
    std::array<ViewChange, config::DECAL_INSTANCE_MAX> changes{};
    bool changed = false;
    for (uint32_t i = 0; i < _instances.size(); ++i) {
        auto &instance = _instances[i];
        if (instance.grids.empty()) {
            continue;
        }
        const float distance = (localView - instance.center).length();
        const bool active = distance < _asset->decalFarDistance();
        const float t = std::clamp((distance - _asset->decalNearDistance()) /
                                   (_asset->decalFarDistance() - _asset->decalNearDistance()), 0.0F, 1.0F);
        const float fade = 1.0F - t * t * (3.0F - 2.0F * t);
        if (active == instance.active && fade == instance.fade) {
            continue;
        }
        changes[i] = active && !instance.active ? ViewChange::ENTER : ViewChange::FADE;
        changed = true;
        instance.active = active;
        instance.fade = fade;
    }
    if (!changed) {
        return;
    }
    // Visit existing draws once, even when several decals are fading together.
    for (auto &entry : _nodes) {
        auto &draws = entry.second.draws;
        size_t count = 0;
        for (size_t i = 0; i < draws.size(); ++i) {
            auto &draw = draws[i];
            const auto &instance = _instances[draw.decal];
            if (!instance.active) {
                recycle(draw);
                continue;
            }
            if (changes[draw.decal] != ViewChange::NONE) {
                const auto &d = _instances[draw.decal].decal;
                const float lift = _asset->decalLayers()[d.layer].heightScale;
                setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalFade", Vec4{lift, instance.fade, 0, 0});
            }
            if (count != i) {
                draws[count] = std::move(draw);
            }
            ++count;
        }
        draws.resize(count);
    }
    // Entering range needs models/bindings; ordinary movement only writes fade.
    for (uint32_t i = 0; i < _instances.size(); ++i) {
        if (changes[i] != ViewChange::ENTER) {
            continue;
        }
        for (auto &entry : _nodes) {
            appendDecal(entry.second, i);
        }
    }
}

void DecalRenderer::refreshInstances(const Vec3 &viewPosition) {
    updateView(viewPosition);
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            updateInstance(draw, entry.second.terrain);
        }
    }
}

void DecalRenderer::preparePasses(uint32_t stamp) {
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            if (draw.model->isEnabled()) {
                draw.model->updateTransform(stamp);
                draw.model->updateUBOs(stamp);
            }
        }
    }
}

void DecalRenderer::collectPassModels(const QuadNode &node, const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const {
    const auto it = _nodes.find(node.address().key());
    if (it == _nodes.end()) {
        return;
    }
    for (const auto &draw : it->second.draws) {
        const auto *model = draw.model.get();
        if (model->isEnabled() && (!shadow || model->isCastShadow()) && model->getWorldBounds()->aabbFrustum(frustum)) {
            models.push_back(model);
        }
    }
}

void DecalRenderer::onGlobalPipelineStateChanged() {
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            draw.model->onGlobalPipelineStateChanged();
        }
    }
    for (const auto &model : _pool) {
        model->onGlobalPipelineStateChanged();
    }
    compilePipelines();
}

void DecalRenderer::setEnabled(bool enabled) {
    _enabled = enabled;
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            draw.model->setEnabled(enabled);
        }
    }
}

void DecalRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    refreshMaterials();
}

void DecalRenderer::refreshMaterials() {
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            draw.model->setSubModelMaterial(0, _wireframe ? _wire.get() : _solid.get());
        }
    }
    for (const auto &model : _pool) {
        model->setSubModelMaterial(0, _wireframe ? _wire.get() : _solid.get());
    }
    refreshInstances(_viewPosition);
    compilePipelines();
}

void DecalRenderer::setCastShadow(bool enabled) {
    _castShadow = enabled;
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            draw.model->setCastShadow(enabled);
        }
    }
    for (const auto &model : _pool) {
        model->setCastShadow(enabled);
    }
}

void DecalRenderer::setReceiveShadow(bool enabled) {
    _receiveShadow = enabled;
    for (const auto &entry : _nodes) {
        for (const auto &draw : entry.second.draws) {
            draw.model->setReceiveShadow(enabled);
        }
    }
    for (const auto &model : _pool) {
        model->setReceiveShadow(enabled);
    }
    // Variant changes can rebuild instance layouts; restore all active bindings.
    refreshMaterials();
}

} // namespace landscape
} // namespace cc

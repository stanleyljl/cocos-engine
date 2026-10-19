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
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/Pass.h"
#include "scene/RenderScene.h"

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

void LandscapeSurfaceInstance::apply(scene::Model *model, TypedArray &scratch) const {
    setLandscapeInstanceAttribute(model, scratch, "a_gridInst", grid);
    setLandscapeInstanceAttribute(model, scratch, "a_quadrantInst", quadrant);
    setLandscapeInstanceAttribute(model, scratch, "a_tileInst", tile);
    setLandscapeInstanceAttribute(model, scratch, "a_vtInst", vt);
}

DecalRenderer::DecalRenderer() = default;
DecalRenderer::~DecalRenderer() {
    for (const auto &draw : _draws) {
        draw.model->detachFromScene();
        draw.model->destroy();
    }
}

bool DecalRenderer::init(Node *node, scene::RenderScene *scene, bool heightUnorm) {
    _node = node;
    _scene = scene;
    _solid = createDecalMaterial(false, heightUnorm);
    _wire = createDecalMaterial(true, heightUnorm);
    _mesh = GridMesh::create(Root::getInstance()->getDevice(), 16);
    _scratch = Float32Array(4);
    _candidates.reserve(config::DECAL_INSTANCE_MAX);
    return _mesh && _solid && _wire &&
           _solid->getPasses() && !_solid->getPasses()->empty() &&
           _wire->getPasses() && !_wire->getPasses()->empty();
}

std::array<Material *, 2> DecalRenderer::materials() const {
    return {_solid.get(), _wire.get()};
}

bool DecalRenderer::setAsset(LandscapeAsset *asset, gfx::Device *device) {
    _asset = asset;
    const auto &data = asset->data();
    _centers.clear();
    for (const auto &d : asset->decals()) {
        const float x = d.x + d.size * 0.5F;
        const float z = d.z + d.size * 0.5F;
        float minY = data.minHeight();
        float maxY = data.maxHeight();
        const auto address = data.nodeAtClamped(0, {x, z});
        asset->getHeightRange(address.level, address.x, address.z, minY, maxY);
        // Stable whole-decal fade, independent of geometry LOD and streamed heights.
        _centers.emplace_back(x, (minY + maxY) * 0.5F, z);
    }
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

bool DecalRenderer::bindComposeMaterial(Material *material, const MaterialLibrary &materials) {
    auto *pass = material->getPasses()->front().get();
    for (const char *name : {"decalAlbedoMap", "decalNormalMap", "decalRegions",
                             "decalIndexMap", "decalLayerParams", "decalParams"}) {
        if (!pass->getHandle(name)) {
            CC_LOG_WARNING("[Landscape] VT effect is missing %s; rebuild project effects", name);
            return false;
        }
    }
    const auto bind = [&](const char *name, gfx::Texture *texture) {
        material->setPropertyGFXTexture(name, texture);
        pass->bindSampler(scene::Pass::getBindingFromHandle(pass->getHandle(name)), materials.clampSampler());
    };
    bind("decalAlbedoMap", materials.decalAlbedo());
    bind("decalNormalMap", materials.decalNormal());
    bind("decalIndexMap", _pageIndexTexture);
    ccstd::vector<Vec4> regions(config::DECAL_INSTANCE_MAX);
    const auto &decals = _asset->decals();
    for (size_t i = 0; i < decals.size(); ++i) {
        const auto &d = decals[i];
        regions[i] = Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)};
    }
    ccstd::vector<Vec4> layers(config::DECAL_LIBRARY_MAX);
    for (size_t i = 0; i < _asset->decalLayers().size(); ++i) {
        const auto &layer = _asset->decalLayers()[i];
        layers[i] = Vec4{layer.modulateColor ? 1.0F : 0.0F,
                         static_cast<float>(layer.detailMaterial0 + 1), static_cast<float>(layer.detailMaterial1 + 1), 0};
    }
    material->setPropertyVec4Array("decalRegions", regions);
    material->setPropertyVec4Array("decalLayerParams", layers);
    material->setPropertyVec4("decalParams", Vec4{static_cast<float>(decals.size()), 0, 0, 0});
    return true;
}

uint32_t DecalRenderer::collectPageDecals(uint32_t slot, const Vec4 &region) {
    // Both flat and displaced decals contribute surface color/normal to VT.
    // Include filtering gutters and preserve manifest alpha-composition order.
    const float border = region.z * static_cast<float>(config::VT_PAGE_BORDER) / config::VT_PAGE_INTERIOR;
    uint32_t count = 0;
    const auto &decals = _asset->decals();
    for (size_t i = 0; i < decals.size(); ++i) {
        const auto &d = decals[i];
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

void DecalRenderer::updateInstance(const Draw &draw, const TerrainPatch &patch) {
    patch.surface.apply(draw.model, _scratch);
    const auto &d = _asset->decals()[draw.decal];
    const auto &layer = _asset->decalLayers()[d.layer];
    setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalRegion", Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)});
    setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalGrid", draw.grid);
    const float distance = (_viewPosition - (_node->getWorldPosition() + _centers[draw.decal])).length();
    const float t = std::clamp((distance - _asset->decalNearDistance()) / (_asset->decalFarDistance() - _asset->decalNearDistance()), 0.0F, 1.0F);
    setLandscapeInstanceAttribute(draw.model, _scratch, "a_decalFade", Vec4{layer.heightScale, 1.0F - t * t * (3.0F - 2.0F * t), 0, 0});
    auto boundsMax = patch.boundsMax;
    boundsMax.y += layer.heightScale;
    draw.model->createBoundingShape(patch.boundsMin, boundsMax);
    draw.model->updateWorldBound();
}

void DecalRenderer::appendDraw(const TerrainPatch &patch, uint32_t decal, const Vec4 &grid) {
    const size_t at = _active++;
    if (at == _draws.size()) {
        IntrusivePtr<scene::Model> model = Root::getInstance()->createModel<scene::Model>();
        model->setNode(_node);
        model->setTransform(_node);
        model->setCastShadow(_castShadow);
        model->setReceiveShadow(_receiveShadow);
        model->initSubModel(0, _mesh, _wireframe ? _wire.get() : _solid.get());
        model->attachToScene(_scene);
        _draws.push_back(Draw{model, decal, {}});
    }
    auto &draw = _draws[at];
    draw.decal = decal;
    draw.grid = grid;
    updateInstance(draw, patch);
    draw.model->setEnabled(_enabled);
}

bool DecalRenderer::beginSync(const Vec3 &viewPosition) {
    _viewPosition = viewPosition;
    _active = 0;
    _patches.clear();
    _candidates.clear();
    // Evaluate eligibility once per decal, independent of patch subdivision.
    for (uint32_t i = 0; i < _asset->decals().size(); ++i) {
        const auto &d = _asset->decals()[i];
        if (_asset->decalLayers()[d.layer].heightScale == 0.0F) {
            continue;
        }
        const float distance = (_viewPosition - (_node->getWorldPosition() + _centers[i])).length();
        if (distance >= _asset->decalFarDistance()) {
            continue;
        }
        _candidates.push_back(i);
    }
    if (_candidates.empty()) {
        endSync();
    }
    return !_candidates.empty();
}

void DecalRenderer::addPatch(const TerrainPatch &patch) {
    if (_candidates.empty()) {
        return;
    }
    const size_t firstDraw = _active;
    // Fixed lattice at 1/16 of the finest terrain cell. Patch ownership changes
    // only the base height/material mapping, never the decal tessellation.
    const auto &data = _asset->data();
    const float step = data.nodeSize(0) / 16.0F;
    const auto &region = patch.region;
    const float x = region.x;
    const float z = region.z;
    const float size = region.size;
    for (const uint32_t i : _candidates) {
        const auto &d = _asset->decals()[i];
        if (x + size <= d.x || z + size <= d.z || x >= d.x + d.size || z >= d.z + d.size) {
            continue;
        }
        const int firstX = static_cast<int>(std::floor((std::max(x, d.x)) / step));
        const int firstZ = static_cast<int>(std::floor((std::max(z, d.z)) / step));
        const int endX = static_cast<int>(std::ceil((std::min(x + size, d.x + d.size)) / step));
        const int endZ = static_cast<int>(std::ceil((std::min(z + size, d.z + d.size)) / step));
        for (int iz = firstZ; iz < endZ; ++iz) {
            for (int ix = firstX; ix < endX; ++ix) {
                const float left = std::max(ix * step, d.x);
                const float bottom = std::max(iz * step, d.z);
                const float right = std::min((ix + 1) * step, d.x + d.size);
                const float top = std::min((iz + 1) * step, d.z + d.size);
                appendDraw(patch, i, Vec4{left, bottom, right - left, top - bottom});
            }
        }
    }
    if (_active != firstDraw) {
        _patches[patch.nodeKey].push_back({patch, firstDraw, _active});
    }
}

void DecalRenderer::endSync() {
    for (size_t i = _active; i < _draws.size(); ++i) {
        _draws[i].model->setEnabled(false);
    }
}

void DecalRenderer::refreshInstances(const Vec3 &viewPosition) {
    _viewPosition = viewPosition;
    for (const auto &entry : _patches) {
        for (const auto &patch : entry.second) {
            for (size_t i = patch.firstDraw; i < patch.endDraw; ++i) {
                updateInstance(_draws[i], patch.terrain);
            }
        }
    }
}

void DecalRenderer::preparePasses(uint32_t stamp) {
    for (size_t i = 0; i < _active; ++i) {
        _draws[i].model->updateTransform(stamp);
        _draws[i].model->updateUBOs(stamp);
    }
}

void DecalRenderer::collectPassModels(const QuadNode &node, const geometry::Frustum &frustum,
                                      bool shadow, ccstd::vector<const scene::Model *> &models) const {
    const auto it = _patches.find(node.address().key());
    if (it == _patches.end()) {
        return;
    }
    for (const auto &patch : it->second) {
        if (!(node.quadrantMask & patch.terrain.quadrantMask)) {
            continue;
        }
        for (size_t i = patch.firstDraw; i < patch.endDraw; ++i) {
            const auto *model = _draws[i].model.get();
            if (model->isEnabled() && (!shadow || model->isCastShadow()) && model->getWorldBounds()->aabbFrustum(frustum)) {
                models.push_back(model);
            }
        }
    }
}

void DecalRenderer::onGlobalPipelineStateChanged() {
    for (const auto &draw : _draws) {
        draw.model->onGlobalPipelineStateChanged();
    }
}

void DecalRenderer::setEnabled(bool enabled) {
    _enabled = enabled;
    for (size_t i = 0; i < _draws.size(); ++i) {
        _draws[i].model->setEnabled(enabled && i < _active);
    }
}

void DecalRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    for (const auto &draw : _draws) {
        draw.model->setSubModelMaterial(0, wireframe ? _wire.get() : _solid.get());
    }
    refreshInstances(_viewPosition);
}

void DecalRenderer::setCastShadow(bool enabled) {
    _castShadow = enabled;
    for (const auto &draw : _draws) {
        draw.model->setCastShadow(enabled);
    }
}

void DecalRenderer::setReceiveShadow(bool enabled) {
    _receiveShadow = enabled;
    for (const auto &draw : _draws) {
        draw.model->setReceiveShadow(enabled);
    }
    // Variant changes can rebuild instance layouts, including frozen models.
    setWireframe(_wireframe);
}
} // namespace landscape
} // namespace cc

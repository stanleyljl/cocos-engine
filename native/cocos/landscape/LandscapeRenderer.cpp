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
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "base/Log.h"
#include "base/Macros.h"
#include "core/Root.h"
#include "core/TypedArray.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/geometry/AABB.h"
#include "core/scene-graph/Node.h"
#include "landscape/LandscapeRenderer.h"
#include "landscape/GridMesh.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTRenderer.h"
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "renderer/gfx-base/states/GFXSampler.h"
#include "scene/Pass.h"
#include "scene/RenderScene.h"

namespace cc {
namespace landscape {

namespace {
Material *createLandscapeMaterial(bool wireframe, bool heightUnorm) {
    auto *material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"landscape/builtin-landscape"};
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

void setRuntimeTexture(Material *material, const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
    if (material == nullptr) {
        return;
    }
    CC_ASSERT(texture != nullptr);
    const auto &passes = *material->getPasses();
    for (size_t i = 0; i < passes.size(); ++i) {
        const auto &pass = passes[i];
        const uint32_t handle = pass->getHandle(name);
        if (handle == 0U) {
            continue;
        }
        // Keep the material's property record as well as the pass binding.
        material->setPropertyGFXTexture(name, texture, static_cast<index_t>(i));
        pass->bindSampler(scene::Pass::getBindingFromHandle(handle), sampler);
    }
}

} // namespace

LandscapeRenderer::LandscapeRenderer() = default;

LandscapeRenderer::~LandscapeRenderer() {
    destroy();
}

bool LandscapeRenderer::init(Node *node, scene::RenderScene *scene, LandscapeAsset *asset) {
    CC_ASSERT(_node == nullptr && _scene == nullptr);
    auto *device = Root::getInstance()->getDevice();
    if (node == nullptr || scene == nullptr || asset == nullptr || !asset->valid()) {
        return false;
    }

    _node = node;
    _scene = scene;
    _mesh = GridMesh::create(device, 16);
    _heightUnorm = hasAllFlags(device->getFormatFeatures(gfx::Format::R16_UNORM), gfx::FormatFeature::SAMPLED_TEXTURE | gfx::FormatFeature::LINEAR_FILTER);
    _materialSolid = createLandscapeMaterial(false, _heightUnorm);
    _materialWire = createLandscapeMaterial(true, _heightUnorm);
    _decals = std::make_unique<DecalRenderer>();
    const bool decalsReady = _decals->init(node, scene, _heightUnorm);
    if (_mesh == nullptr || !decalsReady || _materialSolid == nullptr || _materialWire == nullptr) {
        destroy();
        return false;
    }

    for (auto *material : {_materialSolid.get(), _materialWire.get()}) {
        const auto passes = material->getPasses();
        if (passes == nullptr || passes->empty()) {
            CC_LOG_WARNING("[Landscape] builtin-landscape material has no passes");
            destroy();
            return false;
        }
    }

    _instanceAttributeScratch = Float32Array(4);
    _asset = asset;
    _data = asset->data();
    _tilePages = std::make_unique<TilePagePool>();
    if (!_tilePages->init(device, asset, config::PAGE_POOL_LAYERS, _heightUnorm)) {
        _tilePages.reset();
        return false;
    }
    _materialLibrary = std::make_unique<MaterialLibrary>();
    if (!_materialLibrary->init(device, *asset)) {
        return false;
    }
    if (!_decals->setAsset(asset, device)) {
        return false;
    }
    _vtRenderer = std::make_unique<VTRenderer>();
    if (!_vtRenderer->init(*asset, *_tilePages, *_materialLibrary, *_decals)) {
        CC_LOG_ERROR("[Landscape] VT initialization failed; terrain requires VT material shading");
        _vtRenderer.reset();
        return false;
    }
    _schedule = std::make_unique<LandscapeSchedule>(*asset, *_tilePages, *_vtRenderer);
    _decals->setEnabled(_debugData.decal3DEnabled);
    _decals->setWireframe(_debugData.wireframe);
    _decals->setCastShadow(_castShadow);
    _decals->setReceiveShadow(_receiveShadow);
    bindRuntimeTextures();
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::setLodRanges(const ccstd::vector<float> &morphStart, const ccstd::vector<float> &morphEnd) {
    const size_t count = std::min({morphStart.size(), morphEnd.size(),
                                   static_cast<size_t>(config::MAX_LOD_LEVELS)});
    _morphStart.assign(config::MAX_LOD_LEVELS, 0.0F);
    _morphEnd.assign(config::MAX_LOD_LEVELS, 0.0F);
    for (size_t i = 0; i < count; ++i) {
        _morphStart[i] = morphStart[i];
        _morphEnd[i] = morphEnd[i];
    }
    updateMaterialProperties();
}

std::array<Material *, 4> LandscapeRenderer::surfaceMaterials() const {
    const auto decals = _decals->materials();
    return {_materialSolid.get(), _materialWire.get(), decals[0], decals[1]};
}

void LandscapeRenderer::bindRuntimeTextures() {
    // Bind full VT textures with anisotropic mip filtering for terrain and decals.
    auto &vt = *_vtRenderer;
    for (auto *material : surfaceMaterials()) {
        setRuntimeTexture(material, "holeSplatmap", _tilePages->splatArray(), _tilePages->splatSampler());
        setRuntimeTexture(material, "heightmap", _tilePages->heightArray(), _tilePages->heightSampler());
        setRuntimeTexture(material, "vtAlbedo", vt.albedo(), vt.sampler());
        setRuntimeTexture(material, "vtPageMap", vt.pageTable().texture(), _tilePages->splatSampler());
        setRuntimeTexture(material, "vtNormalRoughnessAO", vt.normalRoughnessAO(), vt.sampler());
    }
    for (auto *material : _decals->materials()) {
        setRuntimeTexture(material, "decalHeightMap", _materialLibrary->decalHeight(), _tilePages->heightSampler());
    }
}

void LandscapeRenderer::setGlobalColorMap(Texture2D *texture) {
    _vtRenderer->setGlobalColorMap(texture);
}

void LandscapeRenderer::setGlobalColorStrength(float strength) {
    _vtRenderer->setGlobalColorStrength(strength);
}

void LandscapeRenderer::setDebugData(const LandscapeDebugData &data) {
    const auto previous = _debugData;
    _debugData = data;
    // Unlit records the applied variant only after shader compilation succeeds.
    _debugData.unlit = previous.unlit;
    if (previous.wireframe != data.wireframe) {
        _decals->setWireframe(data.wireframe);
    }
    if (previous.decal3DEnabled != data.decal3DEnabled) {
        _decals->setEnabled(data.decal3DEnabled);
    }
    _vtRenderer->setHeightBlendEnabled(data.heightBlendEnabled);
    _vtRenderer->setRefineFarDetail(data.refineFarDetail);
    if (previous.wireframe != data.wireframe) {
        refreshTerrainMaterials();
    }
    setUnlit(data.unlit);
    updateMaterialProperties();
}

IntrusivePtr<scene::Model> LandscapeRenderer::createModel() {
    auto *model = Root::getInstance()->createModel<scene::Model>();
    if (model == nullptr) {
        return nullptr;
    }
    model->setNode(_node);
    model->setTransform(_node);
    model->setCastShadow(_castShadow);
    model->setReceiveShadow(_receiveShadow);
    model->initSubModel(0, _mesh, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
    model->setEnabled(false);
    model->attachToScene(_scene);
    return model;
}

void LandscapeRenderer::updateMaterialProperties() {
    const Vec4 terrainParams{
        _data.heightScale,
        _data.heightBias,
        _debugData.lodColor ? 1.0F : 0.0F,
        _debugData.showRanges ? 1.0F : 0.0F,
    };
    ccstd::vector<Vec4> lodMorph(config::MAX_LOD_LEVELS);
    for (uint32_t i = 0; i < config::MAX_LOD_LEVELS; ++i) {
        const float start = i < _morphStart.size() ? _morphStart[i] : 0.0F;
        const float end = i < _morphEnd.size() ? _morphEnd[i] : 0.0F;
        lodMorph[i] = Vec4{start, end, 0.0F, 0.0F};
    }

    const VTPageLayout layout(_data.sectorSize);
    for (auto *material : surfaceMaterials()) {
        material->setPropertyVec4("holeParams", Vec4{_holeMode == 0U ? 0.0F : 1.0F, 0, 0, 0});
        material->setPropertyVec4("terrainParams", terrainParams);
        material->setPropertyVec4("sectorParams", Vec4{_data.sectorSize, layout.pageSize(0), 0, 0});
        material->setPropertyVec4Array("lodMorph", lodMorph);
        material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE),
                                                   static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER),
                                                   static_cast<float>(config::VT_FILTER_MARGIN)});
    }
    for (auto *material : _decals->materials()) {
        material->setPropertyVec4("decalControl", Vec4{_debugData.decal3DEnabled ? 1.0F : 0.0F, 0, 0, 0});
    }
    updateMorphCameraProperty();
}

void LandscapeRenderer::updateMorphCameraProperty() {
    const Vec4 cameraPosition{
        _viewPosition.x,
        _viewPosition.y,
        _viewPosition.z,
        0.0F,
    };
    for (auto *material : surfaceMaterials()) {
        material->setPropertyVec4("morphCameraPos", cameraPosition);
    }
}

LandscapeGridParams LandscapeRenderer::resolveGridParams(const QuadNode &node, uint32_t mapping) {
    LandscapeGridParams gridParams;

    const auto region = _data.nodeRegion(node.address());
    gridParams.grid = Vec4{region.x, region.z, region.size, static_cast<float>(node.level)};
    const auto edges = _schedule->currentTree().edgeMasks(node.address());
    gridParams.mapping = Vec4{static_cast<float>(mapping), static_cast<float>(edges.coarser), static_cast<float>(edges.finer), 0};

    const auto tile = _tilePages->resolve(node.address());
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] unprepared exact height source: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.address.level, tile.address.x, tile.address.z, tile.layer);
    // a_tileInst.xy = source tile origin in landscape-local coordinates,
    // z = source tile size, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    gridParams.tile = _tilePages->shaderParams(tile);
    return gridParams;
}

void LandscapeRenderer::updateModel(scene::Model *model, const QuadNode &node, const LandscapeGridParams &gridParams) {
    gridParams.apply(model, _instanceAttributeScratch);
    model->setEnabled(true);
    updateModelBounds(model, node);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const QuadNode &node) {
    const auto region = _data.nodeRegion(node.address());
    model->createBoundingShape(Vec3{region.x, node.minY, region.z},
                               Vec3{region.x + region.size, node.maxY, region.z + region.size});
    model->updateWorldBound();
}

void LandscapeRenderer::preparePasses(ExpectedTree expected) {
    const auto frame = Root::getInstance()->getFrameCount();
    auto *oldMappings = _vtRenderer->pageTable().texture();
    _schedule->update(std::move(expected), _viewPosition - _node->getWorldPosition(), frame);
    if (oldMappings != _vtRenderer->pageTable().texture()) {
        bindRuntimeTextures();
    }
    if (!_schedule->currentTree().changedNodes().empty()) {
        syncTerrainModels();
    }
    _decals->updateView(_viewPosition);
    if (!isReady()) {
        return;
    }
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->updateTransform(frame);
            draw.model->updateUBOs(frame);
        }
    }
    _decals->preparePasses(frame);
}

void LandscapeRenderer::collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const {
    if (!isReady()) {
        return;
    }
    const auto &origin = _node->getWorldPosition();
    float decalLift = 0;
    if (_debugData.decal3DEnabled) {
        for (const auto &layer : _asset->decalLayers()) {
            decalLift = std::max(decalLift, layer.heightScale);
        }
    }
    _schedule->currentTree().cull([&](NodeAddress address, bool leaf) {
        if (!leaf) {
            const auto node = _schedule->geometry(address);
            const auto region = _data.nodeRegion(address);
            // Include the bound source's height range (also for shared sources
            // below minTileLevel) and raised decals before pruning a subtree.
            const float top = node.maxY + decalLift;
            const geometry::AABB bounds{region.x + region.size * 0.5F + origin.x,
                                        (node.minY + top) * 0.5F + origin.y,
                                        region.z + region.size * 0.5F + origin.z,
                                        region.size * 0.5F, (top - node.minY) * 0.5F, region.size * 0.5F};
            return bounds.aabbFrustum(frustum);
        }
        const auto draw = _draws.find(address.key());
        if (draw == _draws.end() || !draw->second.model) {
            return false;
        }
        const auto *model = draw->second.model.get();
        const auto &bounds = *model->getWorldBounds();
        const bool visible = bounds.aabbFrustum(frustum);
        if (visible && model->isEnabled() && (!shadow || model->isCastShadow())) {
            models.push_back(model);
        }
        if (_debugData.decal3DEnabled) {
            // Raised decals may enter the frustum while the terrain is outside.
            // Only that case needs a second, expanded bounds test.
            if (!visible) {
                auto decalBounds = bounds;
                decalBounds.center.y += decalLift * 0.5F;
                decalBounds.halfExtents.y += decalLift * 0.5F;
                if (!decalBounds.aabbFrustum(frustum)) {
                    return false;
                }
            }
            _decals->collectPassModels(_schedule->currentTree().nodes().at(address.key()).geometry, frustum, shadow, models);
        }
        return true;
    });
}

void LandscapeRenderer::onGlobalPipelineStateChanged() {
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->onGlobalPipelineStateChanged();
        }
    }
    for (const auto &model : _pool) {
        model->onGlobalPipelineStateChanged();
    }
    _decals->onGlobalPipelineStateChanged();
}

bool LandscapeRenderer::addDecal(uint32_t id, float x, float z, float size, uint32_t layer) {
    if (!_decals->addDecal(id, {x, z, size, layer})) {
        return false;
    }
    _vtRenderer->invalidate({x, z, size});
    return true;
}

bool LandscapeRenderer::removeDecal(uint32_t id) {
    LandscapeRegion region;
    if (!_decals->removeDecal(id, region)) {
        return false;
    }
    _vtRenderer->invalidate(region);
    return true;
}

void LandscapeRenderer::recycleModel(GridDraw &draw) {
    if (draw.model) {
        draw.model->setEnabled(false);
        _pool.push_back(std::move(draw.model));
    }
    if (draw.mapping >= 0) {
        _vtRenderer->pageTable().release(static_cast<uint32_t>(draw.mapping));
        draw.mapping = -1;
    }
}

void LandscapeRenderer::syncTerrainModels() {
    // Every committed node uses the same full grid. Neighbors only refresh the
    // morph/stitch edge masks; material changes update the mapping without replacing models.
    ccstd::vector<uint32_t> changedMappings;
    const auto &changed = _schedule->currentTree().changedNodes();
    const auto &nodes = _schedule->currentTree().nodes();
    changedMappings.reserve(changed.size());
    // Release removed owners before assigning their blocks to new LOD nodes.
    for (const auto key : changed) {
        if (nodes.count(key) == 0) {
            const auto it = _draws.find(key);
            if (it != _draws.end()) {
                recycleModel(it->second);
                _draws.erase(it);
            }
            _decals->removeNode(key);
        }
    }
    for (const auto key : changed) {
        const auto it = nodes.find(key);
        if (it == nodes.end()) {
            continue;
        }
        auto &draw = _draws[key];
        const auto &node = it->second.geometry;
        if (draw.mapping < 0) {
            draw.mapping = static_cast<int>(_vtRenderer->pageTable().allocate());
        }
        const auto mapping = static_cast<uint32_t>(draw.mapping);
        if (_vtRenderer->pageTable().update(mapping, it->second.material, *_vtRenderer)) {
            changedMappings.push_back(mapping);
        }
        if (!draw.model) {
            if (_pool.empty()) {
                draw.model = createModel();
            } else {
                draw.model = std::move(_pool.back());
                _pool.pop_back();
            }
        }
        const auto gridParams = resolveGridParams(node, mapping);
        if (draw.model) {
            updateModel(draw.model, node, gridParams);
        }
        const auto region = _data.nodeRegion(node.address());
        _decals->updateNode({key, region, node.minY, node.maxY, gridParams});
    }
    _vtRenderer->pageTable().upload(std::move(changedMappings));
}

void LandscapeRenderer::setHoleMode(uint32_t mode) {
    _holeMode = mode == 0U ? 0U : 1U;
    updateMaterialProperties();
}

void LandscapeRenderer::setCastShadow(bool enabled) {
    if (_castShadow == enabled) {
        return;
    }
    _castShadow = enabled;
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->setCastShadow(enabled);
        }
    }
    for (const auto &model : _pool) {
        model->setCastShadow(enabled);
    }
    _decals->setCastShadow(enabled);
}

void LandscapeRenderer::setReceiveShadow(bool enabled) {
    if (_receiveShadow == enabled) {
        return;
    }
    _receiveShadow = enabled;
    // Model refreshes CC_RECEIVE_SHADOW for every submodel. Include pooled
    // instances and decals so all models use the selected variant.
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->setReceiveShadow(enabled);
        }
    }
    for (const auto &model : _pool) {
        model->setReceiveShadow(enabled);
    }
    _decals->setReceiveShadow(enabled);
    // The receive variant adds an instanced shadow-bias attribute. Rebuild the
    // layouts and restore terrain attributes for active models.
    refreshTerrainMaterials();
}

void LandscapeRenderer::refreshTerrainMaterials() {
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
            const auto &node = _schedule->currentTree().nodes().at(entry.first).geometry;
            resolveGridParams(node, static_cast<uint32_t>(draw.mapping)).apply(draw.model, _instanceAttributeScratch);
        }
    }
    for (const auto &model : _pool) {
        model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
    }
    // Inactive models get fresh instance data when reused.
}

void LandscapeRenderer::setViewPos(const Vec3 &position) {
    if (_viewPosition.x == position.x &&
        _viewPosition.y == position.y &&
        _viewPosition.z == position.z) {
        return;
    }
    _viewPosition = position;
    updateMorphCameraProperty();
}

TextureBase *LandscapeRenderer::getVTAtlas() const {
    return _vtRenderer->atlas();
}

void LandscapeRenderer::setUnlit(bool enabled) {
    if (_debugData.unlit == enabled) {
        return;
    }
    // These passes belong exclusively to this renderer. Update their shader
    // variants in place, preserving texture bindings and instanced batching.
    const auto compile = [this](bool unlit) {
        bool success = true;
        for (auto *material : surfaceMaterials()) {
            for (const auto &pass : *material->getPasses()) {
                pass->getDefines()["LANDSCAPE_DEBUG_UNLIT"] = unlit;
                success = pass->tryCompile() && success;
            }
        }
        return success;
    };
    if (compile(enabled)) {
        _debugData.unlit = enabled;
    } else {
        compile(_debugData.unlit);
        CC_LOG_ERROR("[Landscape] failed to switch unlit shader variant");
    }
    // Refresh cached submodel shaders and instance attributes, including pooled
    // models, so reused models retain the correct instance layout.
    refreshTerrainMaterials();
    _decals->refreshMaterials();
}

void LandscapeRenderer::destroy() {
    _schedule.reset();
    const auto removeModel = [](const IntrusivePtr<scene::Model> &model) {
        if (model == nullptr) {
            return;
        }
        model->detachFromScene();
        model->destroy();
    };

    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        removeModel(draw.model);
    }
    _draws.clear();
    for (const auto &model : _pool) {
        removeModel(model);
    }

    _pool.clear();
    _instanceAttributeScratch = ccstd::monostate{};
    _vtRenderer.reset();
    _decals.reset();

    _tilePages.reset();
    _asset = nullptr;
    _mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _materialLibrary.reset();
    _node = nullptr;
    _scene = nullptr;
    _heightUnorm = false;
    _debugData.unlit = false;
}

} // namespace landscape
} // namespace cc

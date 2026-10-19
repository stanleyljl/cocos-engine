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

#include "landscape/LandscapeRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "application/ApplicationManager.h"
#include "base/Log.h"
#include "base/Macros.h"
#include "base/std/container/unordered_set.h"
#include "core/Root.h"
#include "core/TypedArray.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/geometry/AABB.h"
#include "core/scene-graph/Node.h"
#include "landscape/GridMesh.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTRenderer.h"
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXBuffer.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXInputAssembler.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "renderer/gfx-base/states/GFXSampler.h"
#include "renderer/pipeline/InstancedBuffer.h"
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

} // namespace

LandscapeRenderer::LandscapeRenderer() = default;

LandscapeRenderer::~LandscapeRenderer() {
    destroy();
}

bool LandscapeRenderer::init(Node *node, scene::RenderScene *scene) {
    CC_ASSERT(_node == nullptr && _scene == nullptr);
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (node == nullptr || scene == nullptr || device == nullptr) {
        return false;
    }

    _node = node;
    _scene = scene;
    _mesh = GridMesh::create(device, 16);
    const bool heightUnorm = TilePagePool::supportsHeightUnorm(device);
    _materialSolid = createLandscapeMaterial(false, heightUnorm);
    _materialWire = createLandscapeMaterial(true, heightUnorm);
    _decals = std::make_unique<DecalRenderer>();
    const bool decalsReady = _decals->init(node, scene, heightUnorm);
    if (_mesh == nullptr ||
        !decalsReady || _materialSolid == nullptr || _materialWire == nullptr) {
        destroy();
        return false;
    }

    const auto passes = _materialSolid->getPasses();
    if (passes == nullptr || passes->empty()) {
        CC_LOG_WARNING("[Landscape] builtin-landscape material has no passes");
        destroy();
        return false;
    }
    _instanceAttributeScratch = Float32Array(4);
    _decals->setEnabled(_debugData.decal3DEnabled);
    _decals->setWireframe(_debugData.wireframe);
    _decals->setCastShadow(_castShadow);
    _decals->setReceiveShadow(_receiveShadow);
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::setLodRanges(const ccstd::vector<float> &morphStart,
                                     const ccstd::vector<float> &morphEnd) {
    _syncCache.invalidate();
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

bool LandscapeRenderer::setAsset(LandscapeAsset *asset) {
    CC_ASSERT(_node != nullptr && _scene != nullptr && _asset == nullptr);
    if (asset == nullptr || !asset->valid() || Root::getInstance() == nullptr) {
        return false;
    }
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (device == nullptr) {
        return false;
    }
    _asset = asset;
    _data = asset->data();
    _selection.init(_data);
    if ((_data.tileResolution - 1U) % 16U != 0) {
        CC_LOG_ERROR("[Landscape] Height source resolution must align with the 16-cell terrain grid.");
        return false;
    }
    _vtLayout = VTPageLayout(_data.sectorSize);
    _heightSampleSpacing = _data.nodeSize(_data.minTileLevel) /
                           static_cast<float>(_data.tileResolution - 1U);
    _tilePages = std::make_unique<TilePagePool>();
    if (!_tilePages->init(device, asset, config::PAGE_POOL_LAYERS)) {
        _tilePages.reset();
        return false;
    }
    _sourceResolver = std::make_unique<TilePageResolver>(*_tilePages, _data);
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
    _vtRenderer->setFrozen(_debugData.freezeLod && _ready);
    bindRuntimeTextures();
    updateMaterialProperties();
    return true;
}

std::array<Material *, 4> LandscapeRenderer::surfaceMaterials() const {
    const auto decals = _decals ? _decals->materials() : std::array<Material *, 2>{};
    return {_materialSolid.get(), _materialWire.get(), decals[0], decals[1]};
}

LandscapeDrawCalls LandscapeRenderer::getDrawCalls(uint32_t frame) const {
    return _drawCallFrame == frame ? _drawCalls : LandscapeDrawCalls{};
}

void LandscapeRenderer::estimateDrawCalls(const ccstd::vector<const scene::Model *> &models) {
    const uint32_t frame = CC_CURRENT_ENGINE()->getTotalFrames();
    if (_drawCallFrame != frame) {
        _drawCallFrame = frame;
        _drawCalls = {};
    }
    std::array<const scene::Pass *, 4> basePasses{};
    const auto materials = surfaceMaterials();
    for (size_t i = 0; i < materials.size(); ++i) {
        auto *material = materials[i];
        if (material && material->getPasses() && !material->getPasses()->empty()) {
            basePasses[i] = material->getPasses()->front().get();
        }
    }
    // Landscape instances share attributes/textures within each material.
    // Estimate batches by material, mesh and the engine's instance limit.
    // This describes visible models, not commands submitted by the pipeline.
    std::array<ccstd::unordered_map<const gfx::Buffer *, uint32_t>, 4> instances;
    for (const auto *model : models) {
        for (const auto &subModel : model->getSubModels()) {
            const auto *pass = subModel->getPass(0);
            const auto it = std::find(basePasses.begin(), basePasses.end(), pass);
            if (it == basePasses.end()) {
                continue;
            }
            const auto index = static_cast<size_t>(it - basePasses.begin());
            auto &count = index < 2 ? _drawCalls.terrainBase : _drawCalls.decalsBase;
            if (pass->getBatchingScheme() == scene::BatchingSchemes::INSTANCING) {
                auto &instanceCount = instances[index][subModel->getInputAssembler()->getIndexBuffer()];
                if (instanceCount % pipeline::InstancedBuffer::MAX_CAPACITY == 0) {
                    ++count;
                }
                ++instanceCount;
            } else {
                ++count;
            }
        }
    }
}

void LandscapeRenderer::bindRuntimeTextures() {
    // Bind full VT textures with anisotropic mip filtering for terrain and decals.
    auto &vt = *_vtRenderer;
    for (auto *material : surfaceMaterials()) {
        const auto setRuntimeTexture = [material](const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
            CC_ASSERT(texture != nullptr);
            material->setPropertyGFXTexture(name, texture);
            for (const auto &pass : *material->getPasses()) {
                const uint32_t handle = pass->getHandle(name);
                if (handle != 0U) {
                    const uint32_t binding = scene::Pass::getBindingFromHandle(handle);
                    pass->bindSampler(binding, sampler);
                }
            }
        };
        setRuntimeTexture("holeSplatmap", _tilePages->splatArray(), _tilePages->splatSampler());
        setRuntimeTexture("heightmap", _tilePages->heightArray(), _tilePages->heightSampler());
        setRuntimeTexture("terrainNormalMap", _tilePages->normalArray(), _tilePages->heightSampler());
        setRuntimeTexture("vtAlbedo", vt.albedo(), vt.sampler());
        setRuntimeTexture("vtPageMap", vt.gridMappings(), _tilePages->splatSampler());
        if (material != _materialSolid.get() && material != _materialWire.get()) {
            setRuntimeTexture("decalHeightMap", _materialLibrary->decalHeight(), _tilePages->heightSampler());
        }
        setRuntimeTexture("vtNormalRoughnessAO", vt.normalRoughnessAO(), vt.sampler());
    }
}

void LandscapeRenderer::setGlobalColorMap(Texture2D *texture) {
    if (_vtRenderer) {
        _vtRenderer->setGlobalColorMap(texture);
    }
}

void LandscapeRenderer::setGlobalColorStrength(float strength) {
    if (_vtRenderer) {
        _vtRenderer->setGlobalColorStrength(strength);
    }
}

void LandscapeRenderer::setDebugData(const LandscapeDebugData &data) {
    // Freeze before changing compose modes; unfreeze only after recording all
    // requested modes so composition and surface decoding advance together.
    if (data.freezeLod) {
        setFreezeLod(true);
    }
    if (_debugData.lodColor != data.lodColor) {
        setLodColor(data.lodColor);
    }
    if (_debugData.showRanges != data.showRanges) {
        setShowRanges(data.showRanges);
    }
    if (_debugData.wireframe != data.wireframe) {
        setWireframe(data.wireframe);
    }
    if (_debugData.unlit != data.unlit) {
        setUnlit(data.unlit);
    }
    if (_debugData.heightBlendEnabled != data.heightBlendEnabled) {
        setHeightBlendEnabled(data.heightBlendEnabled);
    }
    if (_debugData.decal3DEnabled != data.decal3DEnabled) {
        setDecal3DEnabled(data.decal3DEnabled);
    }
    if (_debugData.bakeNormalEnabled != data.bakeNormalEnabled) {
        setBakeNormalEnabled(data.bakeNormalEnabled);
    }
    if (_debugData.cliffEnabled != data.cliffEnabled) {
        setCliffEnabled(data.cliffEnabled);
    }
    _debugData.showBox = data.showBox;
    _debugData.showVTAtlas = data.showVTAtlas;
    if (!data.freezeLod) {
        setFreezeLod(false);
    }
}

void LandscapeRenderer::setDecal3DEnabled(bool enabled) {
    _debugData.decal3DEnabled = enabled;
    updateMaterialProperties();
    // Decal visibility affects geometry only and remains immediate while residency is frozen.
    if (_decals) {
        _decals->setEnabled(enabled);
    }
}

void LandscapeRenderer::setBakeNormalEnabled(bool enabled) {
    _debugData.bakeNormalEnabled = enabled;
    if ((_debugData.freezeLod && _ready) || !_vtRenderer) {
        return;
    }
    _vtRenderer->setBakeNormalEnabled(enabled);
    updateMaterialProperties();
}

void LandscapeRenderer::setHeightBlendEnabled(bool enabled) {
    _debugData.heightBlendEnabled = enabled;
    if (_vtRenderer) {
        _vtRenderer->setHeightBlendEnabled(enabled);
    }
}

void LandscapeRenderer::setCliffEnabled(bool enabled) {
    _syncCache.invalidatePosition();
    _debugData.cliffEnabled = enabled;
    if ((_debugData.freezeLod && _ready) || !_vtRenderer) {
        return;
    }
    _vtRenderer->setCliffEnabled(enabled);
    _syncCache.invalidate();
}

void LandscapeRenderer::setLodColor(bool enabled) {
    _debugData.lodColor = enabled;
    updateMaterialProperties();
}

void LandscapeRenderer::setShowRanges(bool enabled) {
    _debugData.showRanges = enabled;
    updateMaterialProperties();
}

IntrusivePtr<scene::Model> LandscapeRenderer::createModel() {
    if (Root::getInstance() == nullptr || _node == nullptr || _scene == nullptr) {
        return nullptr;
    }

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
    if (_materialSolid == nullptr || _materialWire == nullptr || !_decals) {
        return;
    }

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

    for (auto *material : surfaceMaterials()) {
        material->setPropertyVec4("holeParams", Vec4{_holeMode == 0U ? 0.0F : 1.0F, 0, 0, 0});
        material->setPropertyVec4("terrainParams", terrainParams);
        material->setPropertyVec4("sectorParams", Vec4{_data.sectorSize, _vtLayout.pageSize(0), 0, 0});
        material->setPropertyVec4("rvtNormalParams", Vec4{_vtRenderer && _vtRenderer->isBakeNormalEnabled() ? 1.0F : 0.0F, 0, 0, 0});
        material->setPropertyVec4("heightParams", Vec4{_heightSampleSpacing,
                                                       static_cast<float>(_data.tileResolution),
                                                       0.0F, 0.0F});
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
    if (_materialSolid == nullptr || _materialWire == nullptr || !_decals) {
        return;
    }

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

LandscapeSurfaceInstance LandscapeRenderer::resolveSurface(const GridDraw &draw) {
    LandscapeSurfaceInstance surface;

    const auto &node = draw.node;
    const auto region = _data.nodeRegion(node.address());
    surface.grid = Vec4{region.x, region.z, region.size, static_cast<float>(node.level)};
    surface.mapping = Vec4{static_cast<float>(draw.mapping), static_cast<float>(_selection.edgeMask(node.address())), 0, 0};

    const auto tile = _sourceResolver->resolve(node.address());
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] unprepared exact height source: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.address.level, tile.address.x, tile.address.z, tile.layer);
    // a_tileInst.xy = source tile origin in landscape-local coordinates,
    // z = source tile size, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    surface.tile = _sourceResolver->shaderParams(tile);
    return surface;
}

void LandscapeRenderer::updateInstanceData(scene::Model *model, const GridDraw &draw) {
    if (model) {
        resolveSurface(draw).apply(model, _instanceAttributeScratch);
    }
}

float LandscapeRenderer::regionDistance(const QuadNode &node, float x, float z, float size) const {
    // Match the translation-only terrain placement used by quadtree selection.
    const Vec3 camera = _viewPosition - _node->getWorldPosition();
    const auto axisDistance = [](float p, float lo, float hi) {
        return std::max({lo - p, p - hi, 0.0F});
    };
    const float dx = axisDistance(camera.x, x, x + size);
    const float dy = axisDistance(camera.y, node.minY, node.maxY);
    const float dz = axisDistance(camera.z, z, z + size);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void LandscapeRenderer::updateModel(GridDraw &draw) {
    auto *model = draw.model.get();
    updateInstanceData(model, draw);
    model->setEnabled(true);
    updateModelBounds(model, draw);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const GridDraw &draw) {
    const auto &node = draw.node;
    const auto region = _data.nodeRegion(node.address());
    model->createBoundingShape(Vec3{region.x, node.minY, region.z},
                               Vec3{region.x + region.size, node.maxY, region.z + region.size});
    model->updateWorldBound();
}

void LandscapeRenderer::preparePasses(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    if (!valid()) {
        return;
    }
    sync(geometryNodes, surfaceNodes);
    if (!_ready) {
        return;
    }
    const auto stamp = Root::getInstance()->getFrameCount();
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->updateTransform(stamp);
            draw.model->updateUBOs(stamp);
        }
    }
    _decals->preparePasses(stamp);
}

void LandscapeRenderer::collectPassModels(const geometry::Frustum &frustum,
                                          bool shadow, ccstd::vector<const scene::Model *> &models) const {
    if (!_ready) {
        return;
    }
    const auto &origin = _node->getWorldPosition();
    float decalLift = 0;
    if (_debugData.decal3DEnabled) {
        for (const auto &layer : _asset->decalLayers()) {
            decalLift = std::max(decalLift, layer.heightScale);
        }
    }
    _selection.cull([&](NodeAddress address, bool leaf) {
        if (!leaf) {
            const auto node = drawNode(address);
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
            _decals->collectPassModels(draw->second.node, frustum, shadow, models);
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
    if (_decals) {
        _decals->onGlobalPipelineStateChanged();
    }
}

QuadNode LandscapeRenderer::drawNode(NodeAddress address) const {
    float minY = _data.minHeight();
    float maxY = _data.maxHeight();
    _asset->getHeightRange(address.level, address.x, address.z, minY, maxY);
    const auto source = address.ancestor(std::max(address.level, _data.minTileLevel));
    float sourceMin = minY;
    float sourceMax = maxY;
    _asset->getHeightRange(source.level, source.x, source.z, sourceMin, sourceMax);
    return {address.level, address.x, address.z, std::min(minY, sourceMin), std::max(maxY, sourceMax)};
}

bool LandscapeRenderer::colorRegion(NodeAddress node) const {
    if (_colorPaths.count(node.key())) {
        return true;
    }
    while (node.level < _data.maxLevel) {
        node = node.ancestor(node.level + 1);
        if (_colorSelected.count(node.key())) {
            return true;
        }
    }
    return false;
}

bool LandscapeRenderer::planResources(ResourcePlan &plan) const {
    // Derive residency from the displayed and proposed draws. These are the
    // owners; no separately maintained reference counts or deltas are needed.
    struct Usage {
        ccstd::unordered_set<uint64_t> sources;
        ccstd::unordered_set<uint64_t> pages;
    };
    const auto collect = [this](uint64_t key, const GridDraw &draw, Usage &usage) {
        const auto node = NodeAddress::fromKey(key);
        usage.sources.insert(node.ancestor(std::max(node.level, _data.minTileLevel)).key());
        for (const auto page : draw.material.pages) {
            usage.pages.insert(page.key());
        }
    };
    ccstd::unordered_set<uint64_t> removed;
    for (const auto node : _pending.selection.removed) {
        removed.insert(node.key());
    }
    Usage current;
    Usage next;
    // With no candidate, current is already the complete working set. Avoid
    // collecting and merging an identical next set after a commit/publication.
    const bool changing = !_pending.empty();
    for (const auto &entry : _draws) {
        collect(entry.first, entry.second, current);
        if (changing && !removed.count(entry.first) && !_pending.draws.count(entry.first)) {
            collect(entry.first, entry.second, next);
        }
    }
    for (const auto &entry : _pending.draws) {
        collect(entry.first, entry.second, next);
    }
    ccstd::vector<NodeAddress> fixedSources;
    _vtRenderer->requiredSources(fixedSources);
    for (const auto source : fixedSources) {
        current.sources.insert(source.key());
        if (changing) {
            next.sources.insert(source.key());
        }
    }

    const auto &pages = _vtRenderer->pages();
    const auto sourceCapacity = _tilePages->layerCount();
    // Leave room for a future replacement while old bindings are still drawn.
    // A reduction may proceed even when the current usage exceeds this margin.
    const auto headroom = std::min(size_t{80}, static_cast<size_t>(sourceCapacity) / 4);
    if (_initialized && changing &&
        ((next.sources.size() > current.sources.size() && next.sources.size() + headroom > sourceCapacity) ||
         (next.pages.size() > current.pages.size() && next.pages.size() + 4 > pages.capacity()))) {
        return false;
    }

    // Keep both cuts alive until the candidate is completely ready to draw.
    current.sources.insert(next.sources.begin(), next.sources.end());
    current.pages.insert(next.pages.begin(), next.pages.end());
    plan.sources.clear();
    for (const auto key : current.sources) {
        plan.sources.push_back(NodeAddress::fromKey(key));
    }
    plan.pages.clear();
    for (const auto key : current.pages) {
        const auto address = NodeAddress::fromKey(key);
        plan.pages.push_back({address.level, address.x, address.z});
    }
    std::sort(plan.pages.begin(), plan.pages.end(), [](auto a, auto b) { return a.key() < b.key(); });
    if (!pages.canReserve(plan.pages) || !_tilePages->canReserve(plan.sources)) {
        return false;
    }

    // Geometry and cliff sources stay pinned. Composition inputs are temporary:
    // each page must fit individually, but batches may share the remaining slots.
    const auto &pinned = current.sources;
    auto working = pinned;
    for (const auto page : plan.pages) {
        if (!pages.needsComposition(page)) {
            continue;
        }
        ccstd::vector<NodeAddress> inputs;
        _sourceResolver->pageSources(page, _vtRenderer->isBakeNormalEnabled(), inputs);
        if (!_tilePages->canReserve(inputs)) {
            return false;
        }
        size_t extraPinned = 0;
        size_t extraWorking = 0;
        for (const auto source : inputs) {
            extraPinned += pinned.count(source.key()) == 0;
            extraWorking += working.count(source.key()) == 0;
        }
        if (pinned.size() + extraPinned > sourceCapacity) {
            return false;
        }
        if (working.size() + extraWorking > sourceCapacity) {
            continue;
        }
        for (const auto source : inputs) {
            if (working.insert(source.key()).second) {
                plan.sources.push_back(source);
            }
        }
    }
    return true;
}

bool LandscapeRenderer::reserveResources() {
    ResourcePlan plan;
    return planResources(plan) && reserveResources(plan);
}

bool LandscapeRenderer::reserveResources(const ResourcePlan &plan) {
    // Invalidate only when attempting a reservation. Refused plans leave the
    // current working set valid; successful reservations record its revision.
    _reservedPageRevision.reset();
    auto *oldMappings = _vtRenderer->gridMappings();
    // Commit recycles removed nodes' mapping slots before models allocate new ones.
    // Material-only updates leave the selection and slot count unchanged.
    const auto nextCount = _draws.size() - _pending.selection.removed.size() + _pending.selection.added.size();
    if (!_vtRenderer->ensureGridMappingCapacity(static_cast<uint32_t>(std::max(_draws.size(), nextCount)))) {
        return false;
    }
    if (_vtRenderer->gridMappings() != oldMappings) {
        bindRuntimeTextures();
    }
    // The complete plan is validated before either cache releases live slots.
    if (!_vtRenderer->pages().reserve(plan.pages) || !_tilePages->reserve(plan.sources)) {
        return false;
    }
    _reservedPageRevision = _vtRenderer->pages().revision();
    return true;
}

bool LandscapeRenderer::stage(const RenderSelection::Change &change, ChangeKind kind) {
    _pending.selection = change;
    _pending.kind = kind;
    const bool roots = kind == ChangeKind::INITIAL;
    ResourcePlan plan;
    const auto maxBias = _vtLayout.rootLevel();
    for (uint32_t bias = roots ? maxBias : 0; bias <= maxBias; ++bias) {
        _pending.draws.clear();
        for (const auto node : change.added) {
            auto &draw = _pending.draws[node.key()];
            draw.node = drawNode(node);
            draw.material = _vtLayout.gridMapping(_data.nodeRegion(node), draw.node.minY, draw.node.maxY,
                                            _viewPosition - _node->getWorldPosition(), bias);
        }
        if (planResources(plan)) {
            // Consume this validated plan now, instead of rebuilding it in
            // updateResources. Page publication still triggers replanning there.
            if (!reserveResources(plan)) {
                break;
            }
            return true;
        }
    }
    // Refused candidates have not touched residency. Preserve its
    // revision so updateResources does not rebuild the unchanged working set.
    // An attempted reservation above explicitly invalidates it before mutation.
    _pending = {};
    return false;
}

bool LandscapeRenderer::stageMaterial(NodeAddress node) {
    const auto it = _draws.find(node.key());
    if (it == _draws.end()) {
        return false;
    }
    const auto &draw = it->second;
    auto material = _vtLayout.gridMapping(_data.nodeRegion(node), draw.node.minY, draw.node.maxY,
                                         _viewPosition - _node->getWorldPosition(), 0);
    if (material.cells == draw.material.cells && material.pages.size() == draw.material.pages.size() &&
        std::equal(material.pages.begin(), material.pages.end(), draw.material.pages.begin(), [](const auto &a, const auto &b) {
            return a.key() == b.key();
        })) {
        return false;
    }
    _pending.kind = ChangeKind::MATERIAL;
    auto &replacement = _pending.draws[node.key()];
    replacement.node = draw.node;
    replacement.material = std::move(material);
    if (reserveResources()) {
        return true;
    }
    _pending = {};
    return false;
}

void LandscapeRenderer::resetPlanning() {
    _requests = _selection.requests();
    _requestIndex = 0;
    _materialIndex = 0;
    _materialNodes.clear();
    struct MaterialRequest {
        NodeAddress node;
        float distance;
        bool visible;
    };
    ccstd::vector<MaterialRequest> ordered;
    ordered.reserve(_selection.leaves().size());
    for (const auto key : _selection.leaves()) {
        const auto n = NodeAddress::fromKey(key);
        const auto region = _data.nodeRegion(n);
        ordered.push_back({n, regionDistance(drawNode(n), region.x, region.z, region.size), colorRegion(n)});
    }
    // Evaluate bounds/distance once per leaf, not once per sort comparison.
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        if (a.visible != b.visible) {
            return a.visible;
        }
        return a.distance != b.distance ? a.distance < b.distance : a.node.key() < b.node.key();
    });
    for (const auto &entry : ordered) {
        _materialNodes.push_back(entry.node);
    }
    _planningInvalid = false;
    _settled = false;
}

void LandscapeRenderer::planNext() {
    // Scan until a candidate is admitted or the current queue is exhausted.
    for (;;) {
        RenderSelection::Change change;
        // Release geometry first, then alternate material and geometry work.
        // Newly visible materials must not wait for every geometry split.
        const bool geometryPending = _requestIndex < _requests.size();
        const bool materialPending = _materialIndex < _materialNodes.size();
        if (geometryPending && (!_requests[_requestIndex].split || !materialPending || !_preferMaterial)) {
            if (_selection.plan(_requests[_requestIndex++], change) && stage(change, ChangeKind::GEOMETRY)) {
                return;
            }
        } else if (materialPending) {
            if (stageMaterial(_materialNodes[_materialIndex++])) {
                return;
            }
        } else {
            if (_planningInvalid) {
                resetPlanning();
            } else {
                _settled = true;
            }
            return;
        }
    }
}

void LandscapeRenderer::commitPending() {
    const bool geometry = _pending.kind == ChangeKind::GEOMETRY;
    for (const auto node : _pending.selection.removed) {
        _dirtyNodes.insert(node.key());
        if (geometry) {
            _selection.neighbors(node, [this](NodeAddress n, uint32_t) { _dirtyNodes.insert(n.key()); });
        }
        const auto it = _draws.find(node.key());
        if (it != _draws.end()) {
            recycleModel(it->second);
            _draws.erase(it);
        }
    }
    if (geometry) {
        _selection.commit(_pending.selection);
    }
    for (auto &entry : _pending.draws) {
        const auto node = NodeAddress::fromKey(entry.first);
        _dirtyNodes.insert(entry.first);
        if (_pending.kind == ChangeKind::MATERIAL) {
            // Resources are ready: switch only the binding, retaining the model
            // and its mapping slot throughout the material update.
            _draws.at(entry.first).material = std::move(entry.second.material);
        } else {
            _draws[entry.first] = std::move(entry.second);
        }
        if (geometry) {
            _selection.neighbors(node, [this](NodeAddress n, uint32_t) { _dirtyNodes.insert(n.key()); });
        }
    }
    clearPending();
    const bool first = !_initialized;
    _initialized = true;
    _preferMaterial = geometry;
    if (geometry || first) {
        resetPlanning();
    } else {
        // Material commits do not invalidate geometry candidates or the node
        // order. Retry refused geometry against the released budget, while
        // preserving progress through the material queue.
        _requestIndex = 0;
        // A material downgrade may have released space for nearer requests
        // refused earlier in this pass. Retry them after finishing the queue.
        _planningInvalid = true;
        _settled = false;
    }
}

void LandscapeRenderer::clearPending() {
    _pending = {};
    _reservedPageRevision.reset();
}

// Generated resources are expected to load successfully. Stop the entire
// loading session on an unexpected error; keep drawing committed bindings.
void LandscapeRenderer::stopLoading() {
    clearPending();
    _loadingFailed = true;
    CC_LOG_ERROR("[Landscape] Tile loading stopped; re-enable the landscape to reload.");
}

bool LandscapeRenderer::updateResources(bool synchronous) {
    auto &pages = _vtRenderer->pages();
    if (_pending.empty() && pages.ready() && _reservedPageRevision) {
        return true;
    }
    if (!_reservedPageRevision || *_reservedPageRevision != pages.revision()) {
        if (!reserveResources()) {
            const bool rejectedChange = !_pending.empty();
            clearPending();
            // Capacity refusal cancels the complete candidate, including
            // any neighbor splits. Keep the committed cut and retry its
            // resources in both warmup and streaming. Do not turn a
            // refused detail upgrade into a permanent loading failure.
            if (!rejectedChange || !_initialized || !reserveResources()) {
                CC_LOG_WARNING("[Landscape] Resource reservation deferred; retaining committed bindings.");
                return false;
            }
        }
    }
    const auto sourceRevision = _tilePages->updateRevision();
    const auto pageRevision = pages.revision();
    if (!_tilePages->loadReserved(synchronous)) {
        stopLoading();
        return false;
    }
    const auto composed = _vtRenderer->render(*_tilePages, *_sourceResolver, synchronous ? config::VT_PAGE_COUNT : _remainingVTPages);
    if (!synchronous) {
        _remainingVTPages -= composed;
    }
    if (_tilePages->ready() && pages.ready() && _vtRenderer->sourcesReady()) {
        if (!_pending.empty()) {
            commitPending();
        }
    } else if (synchronous && sourceRevision == _tilePages->updateRevision() && pageRevision == pages.revision()) {
        _loadingFailed = true;
        CC_LOG_ERROR("[Landscape] Initial synchronous warmup made no progress.");
        return false;
    }
    return true;
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    if (!valid() || _loadingFailed) {
        return;
    }
    if (_asset->loadingFailed()) {
        stopLoading();
        return;
    }
    auto &pages = _vtRenderer->pages();
    const auto &origin = _node->getWorldPosition();
    const SyncCache::Positions positions{_viewPosition.x, _viewPosition.y, _viewPosition.z, origin.x, origin.y, origin.z};
    if (_ready && _debugData.freezeLod) {
        return;
    }
    const auto frame = Root::getInstance()->getFrameCount();
    if (!_streamingFrame || *_streamingFrame != frame) {
        _streamingFrame = frame;
        _remainingVTPages = config::VT_PAGE_UPDATE_BUDGET;
        if (_ready) {
            _tilePages->update(config::PAGE_UPLOAD_BUDGET);
        }
    }
    if (_ready && _settled && _pending.empty() && pages.ready() &&
        _syncCache.matches(positions, _tilePages->updateRevision(), pages.revision(), geometryNodes, surfaceNodes)) {
        return;
    }
    updateSelection(geometryNodes, surfaceNodes, positions);
    if (!_initialized && _pending.empty()) {
        RenderSelection::Change roots;
        for (const auto key : _selection.leaves()) {
            roots.added.push_back(NodeAddress::fromKey(key));
        }
        if (!stage(roots, ChangeKind::INITIAL)) {
            _loadingFailed = true;
            CC_LOG_ERROR("[Landscape] Initial coverage exceeds resource capacity.");
            return;
        }
    }
    streamResources();
    if (!_dirtyNodes.empty()) {
        syncTerrainModels();
    }
    _decals->updateView(_viewPosition);
    _syncCache.store(positions, _tilePages->updateRevision(), pages.revision(), geometryNodes, surfaceNodes);
}

void LandscapeRenderer::updateSelection(const ccstd::vector<QuadNode> &geometryNodes,
                                        const ccstd::vector<QuadNode> &surfaceNodes, const SyncCache::Positions &positions) {
    const bool selectionChanged = !_syncCache.matchesSelection(geometryNodes, surfaceNodes);
    if (selectionChanged) {
        _selection.target(geometryNodes);
        _colorPaths.clear();
        _colorSelected.clear();
        for (const auto &node : surfaceNodes) {
            auto address = node.address();
            _colorSelected.insert(address.key());
            for (;;) {
                _colorPaths.insert(address.key());
                if (address.level == _data.maxLevel) {
                    break;
                }
                address = address.ancestor(address.level + 1);
            }
        }
    }
    if (selectionChanged || !_syncCache.matchesPosition(positions)) {
        if (selectionChanged || (_pending.empty() && (_settled || !_initialized))) {
            resetPlanning();
        } else {
            _planningInvalid = true;
        }
    }
}

void LandscapeRenderer::streamResources() {
    auto &pages = _vtRenderer->pages();
    // Warmup and streaming use the same capacity admission and atomic commits.
    // Warmup loads synchronously until no more changes fit; it need not reach
    // the requested detail. Runtime composition is limited to eight pages per
    // frame; cached geometry/material transitions need no additional budget.
    const bool warmingUp = !_ready;
    do {
        if (_pending.empty() && pages.ready()) {
            planNext();
        }
        if (!updateResources(warmingUp)) {
            break;
        }
        if (_pending.empty() && _settled && pages.ready()) {
            if (!_ready) {
                CC_LOG_INFO("[Landscape] Initial view ready: %zu draw nodes, %zu active VT pages", _draws.size(), pages.activeSlots().size());
            }
            _ready = true;
            _vtRenderer->setFrozen(_debugData.freezeLod);
        }
        // Never spin waiting for IO, uploads or a partially composed transaction.
        // A ready commit can immediately continue with the next candidate.
    } while (warmingUp ? !_ready : (_pending.empty() && !_settled && pages.ready()));
}

void LandscapeRenderer::recycleModel(GridDraw &draw) {
    if (draw.model) {
        draw.model->setEnabled(false);
        _pool.push_back(std::move(draw.model));
    }
    if (draw.mapping >= 0) {
        _vtRenderer->releaseGridMapping(static_cast<uint32_t>(draw.mapping));
        draw.mapping = -1;
    }
}

void LandscapeRenderer::syncTerrainModels() {
    // Every committed node uses the same full grid. Neighbors only refresh the
    // stitch mask; material changes update the mapping without replacing models.
    ccstd::vector<uint32_t> changedMappings;
    changedMappings.reserve(_dirtyNodes.size());
    // Release removed owners before assigning their blocks to new LOD nodes.
    for (const auto key : _dirtyNodes) {
        if (_draws.count(key) == 0) {
            _decals->removeNode(key);
        }
    }
    for (const auto key : _dirtyNodes) {
        const auto it = _draws.find(key);
        if (it == _draws.end()) {
            continue;
        }
        auto &draw = it->second;
        if (draw.mapping < 0) {
            draw.mapping = static_cast<int>(_vtRenderer->allocateGridMapping());
        }
        const auto mapping = static_cast<uint32_t>(draw.mapping);
        if (_vtRenderer->updateGridMapping(mapping, draw.material)) {
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
        if (draw.model) {
            updateModel(draw);
        }
        const auto region = _data.nodeRegion(draw.node.address());
        _decals->updateNode({key, region, draw.node.minY, draw.node.maxY, resolveSurface(draw)});
    }
    _vtRenderer->uploadGridMappings(std::move(changedMappings));
    _dirtyNodes.clear();
}

void LandscapeRenderer::setFreezeLod(bool frozen) {
    if (_debugData.freezeLod == frozen) {
        return;
    }
    _debugData.freezeLod = frozen;
    if (_vtRenderer) {
        _vtRenderer->setFrozen(frozen && _ready);
    }
    if (_decals) {
        _decals->refreshInstances(_viewPosition);
    }
    if (!frozen) {
        setBakeNormalEnabled(_debugData.bakeNormalEnabled);
        setCliffEnabled(_debugData.cliffEnabled);
    }
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            updateModelBounds(draw.model, draw);
        }
    }
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
    if (_decals) {
        _decals->setCastShadow(enabled);
    }
}

void LandscapeRenderer::setReceiveShadow(bool enabled) {
    if (_receiveShadow == enabled) {
        return;
    }
    _receiveShadow = enabled;
    // Model refreshes CC_RECEIVE_SHADOW for every submodel. Include pooled
    // instances and decals so toggling works even while LOD is frozen.
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->setReceiveShadow(enabled);
        }
    }
    for (const auto &model : _pool) {
        model->setReceiveShadow(enabled);
    }
    if (_decals) {
        _decals->setReceiveShadow(enabled);
    }
    // The receive variant adds an instanced shadow-bias attribute. Rebuild the
    // layouts and restore terrain attributes, including frozen active models.
    refreshTerrainMaterials();
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _debugData.wireframe = wireframe;
    if (_decals) {
        _decals->setWireframe(wireframe);
    }
    refreshTerrainMaterials();
}

void LandscapeRenderer::refreshTerrainMaterials() {
    for (const auto &entry : _draws) {
        const auto &draw = entry.second;
        if (draw.model) {
            draw.model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
            updateInstanceData(draw.model, draw);
        }
    }
    for (const auto &model : _pool) {
        model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
    }
    // Inactive models get fresh instance data when reused.
}

void LandscapeRenderer::setViewPos(const Vec3 &position) {
    if (_debugData.freezeLod && _ready) {
        return;
    }
    if (_viewPosition.x == position.x &&
        _viewPosition.y == position.y &&
        _viewPosition.z == position.z) {
        return;
    }
    _viewPosition = position;
    updateMorphCameraProperty();
}

TextureBase *LandscapeRenderer::vtAtlas() const {
    return _vtRenderer ? _vtRenderer->atlas() : nullptr;
}

void LandscapeRenderer::setUnlit(bool enabled) {
    if (_debugData.unlit == enabled || !valid()) {
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
    // models. This is also required while the LOD selection is frozen.
    refreshTerrainMaterials();
    if (_decals) {
        _decals->refreshMaterials();
    }
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _node != nullptr && _mesh != nullptr &&
           _materialSolid != nullptr && _materialWire != nullptr &&
           _tilePages != nullptr && _tilePages->valid() && _asset != nullptr &&
           _materialLibrary != nullptr && _materialLibrary->valid() &&
           _vtRenderer != nullptr && _vtRenderer->valid();
}

void LandscapeRenderer::destroy() {
    _ready = false;
    _drawCalls = {};
    _loadingFailed = false;
    _syncCache.clear();
    _streamingFrame.reset();
    _remainingVTPages = 0;
    _planningInvalid = false;
    _preferMaterial = true;
    clearPending();
    _dirtyNodes.clear();
    _colorPaths.clear();
    _colorSelected.clear();
    _requests.clear();
    _materialNodes.clear();
    _settled = false;
    _initialized = false;

    _sourceResolver.reset();
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
    _debugData.unlit = false;
}

} // namespace landscape
} // namespace cc

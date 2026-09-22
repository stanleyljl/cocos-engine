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
#include <cstdint>
#include <cmath>

#include "base/Log.h"
#include "base/Macros.h"
#include "base/std/container/unordered_set.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/scene-graph/Node.h"
#include "core/TypedArray.h"
#include "landscape/GridMesh.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTRenderer.h"
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXBuffer.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "renderer/gfx-base/states/GFXSampler.h"
#include "scene/RenderScene.h"
#include "scene/Pass.h"

namespace cc {
namespace landscape {

namespace {
Material *createLandscapeMaterial(bool wireframe, bool decal = false) {
    auto *material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-landscape"};
    MacroRecord defines;
    defines["USE_INSTANCING"] = true;
    defines["LANDSCAPE_DEBUG_UNLIT"] = false;
    defines["LANDSCAPE_DECAL_MESH"] = decal;
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
    for (uint32_t i = 0; i < _meshes.size(); ++i) _meshes[i] = GridMesh::create(device, 1U << i);
    _materialSolid = createLandscapeMaterial(false);
    _materialWire = createLandscapeMaterial(true);
    _decalSolid = createLandscapeMaterial(false, true);
    _decalWire = createLandscapeMaterial(true, true);
    _decalMesh = GridMesh::create(device, 16);
    if (std::any_of(_meshes.begin(), _meshes.end(), [](const auto &mesh) { return mesh == nullptr; }) ||
        _decalMesh == nullptr ||
        _materialSolid == nullptr || _materialWire == nullptr || _decalSolid == nullptr || _decalWire == nullptr) {
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
    initDecalCenters();
    _vtRootLevel = vtRootLevel(_data.sectorSize);
    _heightSampleSpacing = computeNodeSize(_data.sectorSize, _data.maxLevel, _data.minTileLevel) /
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
    _vtRenderer = std::make_unique<VTRenderer>();
    if (!_vtRenderer->init(*asset, *_tilePages, *_materialLibrary)) {
        CC_LOG_ERROR("[Landscape] VT initialization failed; terrain requires VT material shading");
        _vtRenderer.reset();
        return false;
    }
    _vtRenderer->setFrozen(_debugData.freezeLod && _ready);
    bindRuntimeTextures();
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::initDecalCenters() {
    _decalCenters.clear();
    const float finestNodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, 0);
    for (const auto &d : _asset->decals()) {
        const float x = d.x + d.size * 0.5F;
        const float z = d.z + d.size * 0.5F;
        float minY = _data.minHeight();
        float maxY = _data.maxHeight();
        _asset->getHeightRange(0, static_cast<uint32_t>((x + _data.worldWidth() * 0.5F) / finestNodeSize),
                             static_cast<uint32_t>((z + _data.worldDepth() * 0.5F) / finestNodeSize), minY, maxY);
        // A stable height reference keeps the WHOLE decal's fade independent of
        // selected LOD, streaming height pages and individual vertex positions.
        _decalCenters.emplace_back(x, (minY + maxY) * 0.5F, z);
    }
}

void LandscapeRenderer::bindRuntimeTextures() {
    // Bind full VT textures with trilinear mip filtering for terrain and decals.
    auto &vt = _vtRenderer->texture();
    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
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
        setRuntimeTexture("heightmap", _tilePages->heightArray(), _tilePages->heightSampler());
        setRuntimeTexture("terrainNormalMap", _tilePages->normalArray(), _tilePages->heightSampler());
        setRuntimeTexture("vtAlbedo", vt.albedo(), vt.sampler());
        if (material == _decalSolid.get() || material == _decalWire.get()) {
            setRuntimeTexture("decalHeightMap", _materialLibrary->decalHeight(), _tilePages->heightSampler());
        }
        setRuntimeTexture("vtNormalRoughnessAO", vt.normalRoughnessAO(), vt.sampler());
    }
}

void LandscapeRenderer::setGlobalColorMap(Texture2D *texture) {
    if (_vtRenderer) _vtRenderer->setGlobalColorMap(texture);
}

void LandscapeRenderer::setGlobalColorStrength(float strength) {
    if (_vtRenderer) _vtRenderer->setGlobalColorStrength(strength);
}

void LandscapeRenderer::setDebugData(const LandscapeDebugData &data) {
    // Freeze before changing compose modes; unfreeze only after recording all
    // requested modes so composition and surface decoding advance together.
    if (data.freezeLod) setFreezeLod(true);
    if (_debugData.lodColor != data.lodColor) setLodColor(data.lodColor);
    if (_debugData.showRanges != data.showRanges) setShowRanges(data.showRanges);
    if (_debugData.wireframe != data.wireframe) setWireframe(data.wireframe);
    if (_debugData.unlit != data.unlit) setUnlit(data.unlit);
    if (_debugData.heightBlendEnabled != data.heightBlendEnabled) setHeightBlendEnabled(data.heightBlendEnabled);
    if (_debugData.decal3DEnabled != data.decal3DEnabled) setDecal3DEnabled(data.decal3DEnabled);
    if (_debugData.bakeNormalEnabled != data.bakeNormalEnabled) setBakeNormalEnabled(data.bakeNormalEnabled);
    if (_debugData.cliffEnabled != data.cliffEnabled) setCliffEnabled(data.cliffEnabled);
    _debugData.showBox = data.showBox;
    _debugData.showVTAtlas = data.showVTAtlas;
    if (!data.freezeLod) setFreezeLod(false);
}

void LandscapeRenderer::setDecal3DEnabled(bool enabled) {
    _debugData.decal3DEnabled = enabled;
    updateMaterialProperties();
    // Decal visibility affects geometry only and remains immediate while residency is frozen.
    for (size_t i = 0; i < _decalDraws.size(); ++i) _decalDraws[i].model->setEnabled(enabled && i < _decalActive);
}

void LandscapeRenderer::setBakeNormalEnabled(bool enabled) {
    _debugData.bakeNormalEnabled = enabled;
    if ((_debugData.freezeLod && _ready) || !_vtRenderer) return;
    _vtRenderer->setBakeNormalEnabled(enabled);
    updateMaterialProperties();
}

void LandscapeRenderer::setHeightBlendEnabled(bool enabled) {
    _debugData.heightBlendEnabled = enabled;
    if (_vtRenderer) _vtRenderer->setHeightBlendEnabled(enabled);
}

void LandscapeRenderer::setCliffEnabled(bool enabled) {
    _debugData.cliffEnabled = enabled;
    if ((_debugData.freezeLod && _ready) || !_vtRenderer) return;
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

IntrusivePtr<scene::Model> LandscapeRenderer::createModel(uint32_t meshIndex) {
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
    model->initSubModel(0, _meshes[meshIndex], _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
    model->setEnabled(false);
    model->attachToScene(_scene);
    return model;
}

void LandscapeRenderer::updateMaterialProperties() {
    if (_materialSolid == nullptr || _materialWire == nullptr || _decalSolid == nullptr || _decalWire == nullptr) {
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

    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
        material->setPropertyVec4("terrainParams", terrainParams);
        material->setPropertyVec4("sectorParams", Vec4{_data.sectorSize,
            _data.worldWidth() * 0.5F, _data.worldDepth() * 0.5F, 0.0F});
        material->setPropertyVec4("decalControl", Vec4{_debugData.decal3DEnabled ? 1.0F : 0.0F, 0, 0, 0});
        material->setPropertyVec4("rvtNormalParams", Vec4{_vtRenderer && _vtRenderer->isBakeNormalEnabled() ? 1.0F : 0.0F, 0, 0, 0});
        material->setPropertyVec4("heightParams", Vec4{_heightSampleSpacing,
                                                        static_cast<float>(_data.tileResolution),
                                                        0.0F, 0.0F});
        material->setPropertyVec4Array("lodMorph", lodMorph);
        material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE),
            static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER), 0.0F});
    }
    updateMorphCameraProperty();
}

void LandscapeRenderer::updateMorphCameraProperty() {
    if (_materialSolid == nullptr || _materialWire == nullptr || _decalSolid == nullptr || _decalWire == nullptr) {
        return;
    }

    const Vec4 cameraPosition{
        _viewPosition.x,
        _viewPosition.y,
        _viewPosition.z,
        0.0F,
    };
    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
        material->setPropertyVec4("morphCameraPos", cameraPosition);
    }
}

void LandscapeRenderer::setInstanceAttribute(scene::Model *model, const char *name, const Vec4 &value) {
    // setInstancedAttribute copies into each submodel's own attribute block
    // synchronously, so this buffer can be reused across attributes and models.
    // Creating transient ArrayBuffers here also grows the external pointer
    // table in the bundled V8 build even after their JS objects are collected.
    auto &instance = ccstd::get<Float32Array>(_instanceAttributeScratch);
    instance[0] = value.x;
    instance[1] = value.y;
    instance[2] = value.z;
    instance[3] = value.w;
    model->setInstancedAttribute(name, _instanceAttributeScratch);
}

void LandscapeRenderer::updateInstanceData(scene::Model *model, const Patch &patch) {
    if (model == nullptr) {
        return;
    }

    const auto &node = patch.node;
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, std::min(node.level, _data.maxLevel));
    const float x = static_cast<float>(node.ix) * nodeSize - _data.worldWidth() * 0.5F;
    const float z = static_cast<float>(node.iz) * nodeSize - _data.worldDepth() * 0.5F;
    setInstanceAttribute(model, "a_gridInst", Vec4{x, z, nodeSize, static_cast<float>(node.level)});
    setInstanceAttribute(model, "a_quadrantInst", Vec4{patch.x / 16.0F, patch.z / 16.0F,
        static_cast<float>(1U << patch.meshIndex) / 16.0F, 0.0F});

    const auto tile = _sourceResolver->resolve(node);
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] missing resident root: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.level, tile.x, tile.z, tile.layer);
    // a_tileInst.xy = source tile origin in landscape-local meters,
    // z = source tile size in meters, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    setInstanceAttribute(model, "a_tileInst", _sourceResolver->params(tile));
    if (!patch.needsMaterial) return;
    setInstanceAttribute(model, "a_normalParentInst", _sourceResolver->params(_sourceResolver->normalParent(node, tile)));
    const int vtSlot = _vtRenderer->texture().findReadyPageOrRoot(patch.page, _vtRootLevel);
    CC_ASSERTF(vtSlot >= 0, "[Landscape] missing permanent root VT page");
    const auto &vtRegion = _vtRenderer->texture().page(static_cast<uint32_t>(vtSlot)).inputs.region;
    // A fallback covers an ancestor's region, not the fine node's region.
    setInstanceAttribute(model, "a_vtInst", Vec4{vtRegion.x, vtRegion.y, vtRegion.z, static_cast<float>(vtSlot)});
}

float LandscapeRenderer::patchDistance(const QuadNode &node, float x, float z, float size, bool farthest) const {
    // Match the translation-only terrain placement used by quadtree selection.
    const Vec3 camera = (farthest ? _viewPosition : _vtViewPosition) - _node->getWorldPosition();
    const auto axisDistance = [farthest](float p, float lo, float hi) {
        return farthest ? std::max(std::abs(p - lo), std::abs(p - hi)) : std::max({lo - p, p - hi, 0.0F});
    };
    const float dx = axisDistance(camera.x, x, x + size);
    const float dy = axisDistance(camera.y, node.minY, node.maxY);
    const float dz = axisDistance(camera.z, z, z + size);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void LandscapeRenderer::selectPatches(const QuadNode &node, uint32_t x, uint32_t z, uint32_t meshIndex,
                                      ccstd::vector<Patch> &patches) {
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
    const float cellSize = nodeSize / 16.0F;
    const uint32_t cells = 1U << meshIndex;
    const float size = cells * cellSize;
    const float minX = node.ix * nodeSize + x * cellSize;
    const float minZ = node.iz * nodeSize + z * cellSize;
    const float localX = minX - _data.worldWidth() * 0.5F;
    const float localZ = minZ - _data.worldDepth() * 0.5F;
    const float distance = patchDistance(node, localX, localZ, size, false);
    float density = 1.0F;
    if (_debugData.cliffEnabled) {
        float minY = node.minY, maxY = node.maxY;
        const uint32_t rangeLevel = node.level + meshIndex >= 4U ? node.level + meshIndex - 4U : 0U;
        const float rangeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, rangeLevel);
        const uint32_t rangeX = static_cast<uint32_t>(minX / rangeSize);
        const uint32_t rangeZ = static_cast<uint32_t>(minZ / rangeSize);
        _asset->getHeightRange(rangeLevel, rangeX, rangeZ, minY, maxY);
        density = cliffDensityScale(maxY - minY, size, _asset->getSurfaceStretch(rangeLevel, rangeX, rangeZ));
    }
    const uint32_t desired = vtDesiredLevel(_data.sectorSize, _vtRootLevel, distance / density);
    if (meshIndex > 0 && vtPageWorldSize(_data.sectorSize, _vtRootLevel, desired) < size) {
        const uint32_t half = cells / 2U;
        for (uint32_t q = 0; q < 4; ++q) {
            selectPatches(node, x + (q & 1U) * half, z + (q >> 1U) * half, meshIndex - 1U, patches);
        }
        return;
    }
    // An odd boundary vertex slides toward the previous even vertex. A page
    // must contain that entire trajectory, not just the unmorphed grid cell.
    const bool canMorph = node.level < _morphStart.size() &&
        patchDistance(node, localX, localZ, size, true) >= _morphStart[node.level];
    const float safeMinX = minX - (canMorph ? (x & 1U) * cellSize : 0.0F);
    const float safeMinZ = minZ - (canMorph ? (z & 1U) * cellSize : 0.0F);
    const auto page = vtCoveringPage(_data.sectorSize, _vtRootLevel, desired,
                                     safeMinX, safeMinZ, minX + size, minZ + size);
    patches.push_back(Patch{node, x, z, meshIndex, page, size * density / std::max(distance, 1.0F)});
}

void LandscapeRenderer::updateModel(ModelState &state, const Patch &patch) {
    auto *model = state.model.get();
    updateInstanceData(model, patch);
    state.patch = patch;
    model->setEnabled(true);
    updateModelBounds(model, patch);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const Patch &patch) {
    const auto &node = patch.node;
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
    const float cellSize = nodeSize / 16.0F;
    const float size = (1U << patch.meshIndex) * cellSize;
    const float x = node.ix * nodeSize - _data.worldWidth() * 0.5F + patch.x * cellSize;
    const float z = node.iz * nodeSize - _data.worldDepth() * 0.5F + patch.z * cellSize;
    model->createBoundingShape(Vec3{x - (patch.x & 1U) * cellSize, node.minY, z - (patch.z & 1U) * cellSize},
                                Vec3{x + size, node.maxY, z + size});
    model->updateWorldBound();
}

void LandscapeRenderer::preparePasses(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    if (!valid()) return;
    sync(geometryNodes, surfaceNodes);
    // All pass requests are now protected. Compose once before any pass consumes VT.
    _vtRenderer->render();
    if (!_ready) return;
    const auto stamp = Root::getInstance()->getFrameCount();
    for (const auto &entry : _active) {
        entry.second.model->updateTransform(stamp);
        entry.second.model->updateUBOs(stamp);
    }
    for (size_t i = 0; i < _decalActive; ++i) {
        _decalDraws[i].model->updateTransform(stamp);
        _decalDraws[i].model->updateUBOs(stamp);
    }
}

void LandscapeRenderer::rebuildNodeModels() {
    _nodeModels.clear();
    const auto add = [this](const Patch &patch, scene::Model *model) {
        const uint32_t quadrant = (patch.x / 8U) + (patch.z / 8U) * 2U;
        _nodeModels[makeNodeKey(patch.node)].push_back({static_cast<uint8_t>(1U << quadrant), model});
    };
    for (const auto &entry : _active) add(entry.second.patch, entry.second.model);
    for (size_t i = 0; i < _decalActive; ++i) add(_decalDraws[i].patch, _decalDraws[i].model);
}

void LandscapeRenderer::collectPassModels(const ccstd::vector<QuadNode> &selected, const geometry::Frustum &frustum,
                                          bool shadow, ccstd::vector<const scene::Model *> &models) const {
    if (!_ready) return;
    for (const auto &node : selected) {
        const auto it = _nodeModels.find(makeNodeKey(node));
        if (it == _nodeModels.end()) continue;
        for (const auto &entry : it->second) {
            const auto *model = entry.model;
            if (!(node.quadrantMask & entry.quadrantMask) || !model->isEnabled() || (shadow && !model->isCastShadow())) continue;
            // Material patches and raised decals may occupy only part of a selected node.
            if (model->getWorldBounds()->aabbFrustum(frustum)) models.push_back(model);
        }
    }
}

void LandscapeRenderer::onGlobalPipelineStateChanged() {
    for (const auto &entry : _active) entry.second.model->onGlobalPipelineStateChanged();
    for (const auto &pool : _pool) for (const auto &model : pool) model->onGlobalPipelineStateChanged();
    for (const auto &draw : _decalDraws) draw.model->onGlobalPipelineStateChanged();
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    if (!valid()) return;
    auto &vt = _vtRenderer->texture();
    const auto &origin = _node->getWorldPosition();
    const LandscapeSyncCache::Positions positions{
        _viewPosition.x, _viewPosition.y, _viewPosition.z,
        _vtViewPosition.x, _vtViewPosition.y, _vtViewPosition.z, origin.x, origin.y, origin.z};
    if (_debugData.freezeLod && _ready) {
        // Freeze LOD position and residency, but let each pass change visibility.
        if (_syncCache.matches(positions, _tilePages->updateRevision(), vt.contentRevision(), geometryNodes, surfaceNodes)) return;
        buildFramePlan(geometryNodes, surfaceNodes);
        syncTerrainModels();
        syncDecals(_patches);
        rebuildNodeModels();
        _syncCache.store(positions, _tilePages->updateRevision(), vt.contentRevision(), geometryNodes, surfaceNodes);
        return;
    }

    bool uploadsPolled = false;
    if (_ready && _syncCache.matches(positions, _tilePages->updateRevision(), vt.contentRevision(), geometryNodes, surfaceNodes)) {
        // Keep last frame's source protection while polling async completions.
        // A stationary camera must still advance loading and fine-page publication.
        _tilePages->update(config::PAGE_UPLOAD_BUDGET);
        uploadsPolled = true;
        if (_syncCache.matches(positions, _tilePages->updateRevision(), vt.contentRevision(), geometryNodes, surfaceNodes)) return;
    }

    buildFramePlan(geometryNodes, surfaceNodes);
    syncPageSources(geometryNodes, surfaceNodes, uploadsPolled);
    if (!_ready) {
        // Keep streaming/composition active, but publish no terrain or shadow
        // casters until the whole current plan has its final source precision.
        if (!initialDataReady()) return;
        _ready = true;
        _vtRenderer->setFrozen(_debugData.freezeLod);
        CC_LOG_INFO("[Landscape] Initial view ready: %zu geometry nodes, %zu composed VT pages",
                    geometryNodes.size(), _requestPlan.requests().size());
    }
    syncTerrainModels();
    syncDecals(_patches);
    rebuildNodeModels();
    // Snapshot before composition: publishing new pages changes the revision
    // and forces bindings to advance from ancestor pages on the next frame.
    _syncCache.store(positions, _tilePages->updateRevision(), vt.contentRevision(), geometryNodes, surfaceNodes);
}

void LandscapeRenderer::buildFramePlan(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    _patches.clear();
    _patches.reserve(geometryNodes.size() * 4U);
    for (const auto &node : surfaceNodes) {
        for (uint32_t q = 0; q < 4; ++q) {
            if ((node.quadrantMask & (1U << q)) != 0) selectPatches(node, (q & 1U) * 8U, (q >> 1U) * 8U, 3U, _patches);
        }
    }
    _requestPlan.begin(_vtRootLevel);
    for (const auto &patch : _patches) _requestPlan.addVisible(patch.page, patch.vtPriority);
    _requestPlan.finish(_data.sectorsX, _data.sectorsZ);

    // Only color passes drive VT subdivision/requests. Each shadow-only quadrant
    // uses one 8x8 grid with the same vertices, LOD and morph as the color pass.
    collectShadowOnlyNodes(geometryNodes, surfaceNodes, _shadowOnlyNodes);
    for (const auto &node : _shadowOnlyNodes) {
        for (uint32_t q = 0; q < 4; ++q) {
            if ((node.quadrantMask & (1U << q)) == 0) continue;
            _patches.push_back(Patch{node, (q & 1U) * 8U, (q >> 1U) * 8U, 3U, {}, 0.0F, false});
        }
    }
}

void LandscapeRenderer::syncPageSources(const ccstd::vector<QuadNode> &geometryNodes,
                                        const ccstd::vector<QuadNode> &surfaceNodes, bool uploadsPolled) {
    // 1. Protect the complete material working set before allocating VT slots.
    _vtRenderer->texture().beginFrame(_requestPlan.dynamicKeys());

    // 2. Request and protect ALL source consumers before uploads may recycle
    //    array layers: cliff references, geometry, and material composition.
    _sourceResolver->beginFrame();
    _vtRenderer->syncCliffSources(*_tilePages);
    _sourceResolver->protectGeometrySources(geometryNodes, surfaceNodes);
    prepareVTPageUpdates();

    // 3. Publish asynchronous source loads once per frame. The stationary-camera
    //    path may already have polled them using the previous frame's protection.
    const auto beforeUpload = _tilePages->updateRevision();
    if (!uploadsPolled) _tilePages->update(config::PAGE_UPLOAD_BUDGET);
    if (_tilePages->updateRevision() != beforeUpload) {
        _vtRenderer->syncCliffSources(*_tilePages);
        _sourceResolver->invalidate();
        // Newly resident geometry can introduce its real parent-normal source.
        _sourceResolver->protectGeometrySources(geometryNodes, surfaceNodes);
        refreshVTPageInputs();
    }

    // 4. Allocate/update material pages with the final resident source layers.
    //    Dirty pages are queued here; preparePasses composes them after sync.
    queueVTPageUpdates();
}

void LandscapeRenderer::prepareVTPageUpdates() {
    _vtPageUpdates.clear();
    _vtPageUpdates.reserve(_requestPlan.requests().size());
    for (const auto &request : _requestPlan.requests()) {
        _vtPageUpdates.push_back({request, _sourceResolver->resolvePage(request.page, _debugData.bakeNormalEnabled)});
    }
}

void LandscapeRenderer::refreshVTPageInputs() {
    for (auto &update : _vtPageUpdates) {
        update.inputs = _sourceResolver->resolvePage(update.request.page, _debugData.bakeNormalEnabled);
    }
}

void LandscapeRenderer::queueVTPageUpdates() {
    auto &vt = _vtRenderer->texture();
    for (const auto &update : _vtPageUpdates) {
        const bool sectorRoot = update.request.page.level == _vtRootLevel;
        vt.acquirePage(update.request, update.inputs, sectorRoot);
    }
}

bool LandscapeRenderer::initialDataReady() const {
    if (!_tilePages->requestsReady() || !_vtRenderer->sourcesReady()) return false;
    const auto &vt = _vtRenderer->texture();
    return std::all_of(_requestPlan.requests().begin(), _requestPlan.requests().end(),
        [&vt](const VTPageRequest &request) { return vt.findReadyPage(request.key) >= 0; });
}

void LandscapeRenderer::syncTerrainModels() {
    _visiblePatches.clear();
    _visiblePatches.reserve(_patches.size());
    for (const auto &patch : _patches) {
        const uint64_t key = makeNodeKey(patch.node.level * 4U + patch.meshIndex,
                                        patch.node.ix * 16U + patch.x, patch.node.iz * 16U + patch.z);
        _visiblePatches.insert(key);
    }
    // Retire old patches before creating replacements so zooming does not
    // unnecessarily grow the model pools.
    for (auto iter = _active.begin(); iter != _active.end();) {
        if (_visiblePatches.count(iter->first) == 0) {
            iter->second.model->setEnabled(false);
            _pool[iter->second.patch.meshIndex].emplace_back(iter->second.model);
            iter = _active.erase(iter);
        } else {
            ++iter;
        }
    }
    for (const auto &patch : _patches) {
        const uint64_t key = makeNodeKey(patch.node.level * 4U + patch.meshIndex,
                                        patch.node.ix * 16U + patch.x, patch.node.iz * 16U + patch.z);
        auto iter = _active.find(key);
        if (iter == _active.end()) {
            auto &pool = _pool[patch.meshIndex];
            IntrusivePtr<scene::Model> model;
            if (!pool.empty()) {
                model = pool.back();
                pool.pop_back();
            } else {
                model = createModel(patch.meshIndex);
            }
            if (model == nullptr) continue;
            iter = _active.emplace(key, ModelState{model, patch}).first;
            updateModel(iter->second, patch);
        } else {
            auto &state = iter->second;
            const bool boundsChanged = state.patch.node.minY != patch.node.minY || state.patch.node.maxY != patch.node.maxY;
            state.patch = patch;
            updateInstanceData(state.model, patch);
            if (boundsChanged) updateModelBounds(state.model, patch);
        }
    }
}

void LandscapeRenderer::updateDecalInstance(const DecalDraw &draw) {
    updateInstanceData(draw.model, draw.patch);
    const auto &d = _asset->decals()[draw.decal];
    const auto &layer = _asset->decalLayers()[d.layer];
    setInstanceAttribute(draw.model, "a_decalRegion", Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)});
    setInstanceAttribute(draw.model, "a_decalGrid", draw.grid);
    const float distance = (_viewPosition - (_node->getWorldPosition() + _decalCenters[draw.decal])).length();
    const float t = std::clamp((distance - d.nearDistance) / (d.farDistance - d.nearDistance), 0.0F, 1.0F);
    setInstanceAttribute(draw.model, "a_decalFade", Vec4{layer.heightScale, 1.0F - t * t * (3.0F - 2.0F * t), 0, 0});
    auto bounds = draw.patch;
    bounds.node.maxY += layer.heightScale;
    updateModelBounds(draw.model, bounds);
}

void LandscapeRenderer::appendDecalDraw(const Patch &patch, uint32_t decal, const Vec4 &grid) {
    const size_t at = _decalActive++;
    if (at == _decalDraws.size()) {
        IntrusivePtr<scene::Model> model = Root::getInstance()->createModel<scene::Model>();
        model->setNode(_node);
        model->setTransform(_node);
        model->setCastShadow(_castShadow);
        model->setReceiveShadow(_receiveShadow);
        model->initSubModel(0, _decalMesh, _debugData.wireframe ? _decalWire.get() : _decalSolid.get());
        model->attachToScene(_scene);
        _decalDraws.push_back(DecalDraw{model, patch, decal, {}});
    }
    auto &draw = _decalDraws[at];
    draw.patch = patch;
    draw.decal = decal;
    draw.grid = grid;
    updateDecalInstance(draw);
    draw.model->setEnabled(_debugData.decal3DEnabled);
}

void LandscapeRenderer::syncDecals(const ccstd::vector<Patch> &patches) {
    _decalActive = 0;
    // View eligibility belongs to the decal, independent of how terrain patches
    // split underneath it. Evaluate it once while preserving manifest draw order.
    std::array<uint32_t, config::DECAL_INSTANCE_MAX> candidates;
    size_t candidateCount = 0;
    for (uint32_t i = 0; i < _asset->decals().size(); ++i) {
        const auto &d = _asset->decals()[i];
        if (_asset->decalLayers()[d.layer].heightScale == 0.0F) continue;
        const float distance = (_viewPosition - (_node->getWorldPosition() + _decalCenters[i])).length();
        if (distance >= d.farDistance) continue;
        candidates[candidateCount++] = i;
    }
    // Every decal uses a fixed lattice at 1/16 of the finest terrain cell.
    // Split on finest-cell boundaries once conceptually: all possible terrain
    // patches align with them, so changing patch ownership cannot retessellate
    // the decal. Only its base height and resident material mapping may change.
    const float step = computeNodeSize(_data.sectorSize, _data.maxLevel, 0) / 16.0F;
    const float originX = -_data.worldWidth() * 0.5F;
    const float originZ = -_data.worldDepth() * 0.5F;
    for (const auto &patch : patches) {
        const auto &node = patch.node;
        const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
        const float cell = nodeSize / 16.0F;
        const float size = (1U << patch.meshIndex) * cell;
        const float x = node.ix * nodeSize - _data.worldWidth() * .5F + patch.x * cell;
        const float z = node.iz * nodeSize - _data.worldDepth() * .5F + patch.z * cell;
        for (size_t candidate = 0; candidate < candidateCount; ++candidate) {
            const uint32_t i = candidates[candidate];
            const auto &d = _asset->decals()[i];
            if (x + size <= d.x || z + size <= d.z || x >= d.x + d.size || z >= d.z + d.size) continue;
            const int firstX = static_cast<int>(std::floor((std::max(x, d.x) - originX) / step));
            const int firstZ = static_cast<int>(std::floor((std::max(z, d.z) - originZ) / step));
            const int endX = static_cast<int>(std::ceil((std::min(x + size, d.x + d.size) - originX) / step));
            const int endZ = static_cast<int>(std::ceil((std::min(z + size, d.z + d.size) - originZ) / step));
            for (int iz = firstZ; iz < endZ; ++iz) for (int ix = firstX; ix < endX; ++ix) {
                const float left = std::max(originX + ix * step, d.x);
                const float bottom = std::max(originZ + iz * step, d.z);
                const float right = std::min(originX + (ix + 1) * step, d.x + d.size);
                const float top = std::min(originZ + (iz + 1) * step, d.z + d.size);
                appendDecalDraw(patch, i, Vec4{left, bottom, right - left, top - bottom});
            }
        }
    }
    for (size_t i = _decalActive; i < _decalDraws.size(); ++i) _decalDraws[i].model->setEnabled(false);
}

void LandscapeRenderer::setFreezeLod(bool frozen) {
    if (_debugData.freezeLod == frozen) {
        return;
    }
    _debugData.freezeLod = frozen;
    if (_vtRenderer) _vtRenderer->setFrozen(frozen && _ready);
    for (size_t i = 0; i < _decalActive; ++i) updateDecalInstance(_decalDraws[i]);
    if (!frozen) setBakeNormalEnabled(_debugData.bakeNormalEnabled);
    if (!frozen) setCliffEnabled(_debugData.cliffEnabled);
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        updateModelBounds(state.model, state.patch);
    }
}

void LandscapeRenderer::setCastShadow(bool enabled) {
    if (_castShadow == enabled) return;
    _castShadow = enabled;
    for (const auto &entry : _active) entry.second.model->setCastShadow(enabled);
    for (const auto &pool : _pool) {
        for (const auto &model : pool) model->setCastShadow(enabled);
    }
    for (const auto &draw : _decalDraws) draw.model->setCastShadow(enabled);
}

void LandscapeRenderer::setReceiveShadow(bool enabled) {
    if (_receiveShadow == enabled) return;
    _receiveShadow = enabled;
    // Model refreshes CC_RECEIVE_SHADOW for every submodel. Include pooled
    // instances and decals so toggling works even while LOD is frozen.
    for (const auto &entry : _active) entry.second.model->setReceiveShadow(enabled);
    for (const auto &pool : _pool) {
        for (const auto &model : pool) model->setReceiveShadow(enabled);
    }
    for (const auto &draw : _decalDraws) draw.model->setReceiveShadow(enabled);
    // The receive variant adds an instanced shadow-bias attribute. Rebuild the
    // layouts and restore terrain attributes, including frozen active models.
    setWireframe(_debugData.wireframe);
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _debugData.wireframe = wireframe;
    for (size_t i = 0; i < _decalDraws.size(); ++i) {
        const auto &draw = _decalDraws[i];
        draw.model->setSubModelMaterial(0, wireframe ? _decalWire.get() : _decalSolid.get());
        if (i < _decalActive) updateDecalInstance(draw);
    }
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        state.model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
        updateInstanceData(state.model, state.patch);
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
        // Inactive models get fresh instance data when reused. Do not request
        // height/VT pages here: doing so would pin invisible nodes in the caches.
    }
}

void LandscapeRenderer::setViewPos(const Vec3 &position) {
    if (_debugData.freezeLod && _ready) return;
    _vtViewPosition = position;
    if (_viewPosition.x == position.x &&
        _viewPosition.y == position.y &&
        _viewPosition.z == position.z) {
        return;
    }
    _viewPosition = position;
    updateMorphCameraProperty();
}

RenderTexture *LandscapeRenderer::vtAtlas() const {
    return _vtRenderer ? _vtRenderer->texture().atlas() : nullptr;
}

void LandscapeRenderer::setUnlit(bool enabled) {
    if (_debugData.unlit == enabled || !valid()) return;
    // These passes belong exclusively to this renderer. Update their shader
    // variants in place, preserving texture bindings and instanced batching.
    const auto compile = [this](bool unlit) {
        bool success = true;
        for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
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
    setWireframe(_debugData.wireframe);
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _node != nullptr && _meshes[0] != nullptr &&
           _materialSolid != nullptr && _materialWire != nullptr &&
           _tilePages != nullptr && _tilePages->valid() && _asset != nullptr &&
           _materialLibrary != nullptr && _materialLibrary->valid() &&
           _vtRenderer != nullptr && _vtRenderer->valid();
}

void LandscapeRenderer::destroy() {
    _ready = false;
    _syncCache.invalidate();
    _sourceResolver.reset();
    _requestPlan = VTRequestPlan{};
    _patches.clear();
    _shadowOnlyNodes.clear();
    _vtPageUpdates.clear();
    _visiblePatches.clear();
    const auto removeModel = [](const IntrusivePtr<scene::Model> &model) {
        if (model == nullptr) {
            return;
        }
        model->detachFromScene();
        model->destroy();
    };

    for (const auto &draw : _decalDraws) removeModel(draw.model);
    _decalDraws.clear();
    _decalActive = 0;
    _decalCenters.clear();
    for (const auto &entry : _active) {
        removeModel(entry.second.model);
    }
    for (auto &pool : _pool) {
        for (const auto &model : pool) removeModel(model);
        pool.clear();
    }
    _active.clear();
    _nodeModels.clear();
    _instanceAttributeScratch = ccstd::monostate{};
    _vtRenderer.reset();

    _tilePages.reset();
    _asset = nullptr;
    for (auto &mesh : _meshes) mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _decalSolid = nullptr;
    _decalWire = nullptr;
    _decalMesh = nullptr;
    _materialLibrary.reset();
    _node = nullptr;
    _scene = nullptr;
    _debugData.unlit = false;
}

} // namespace landscape
} // namespace cc

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
    for (uint32_t i = 0; i < _meshes.size(); ++i) {
        _meshes[i] = GridMesh::create(device, 1U << i);
    }
    const bool heightUnorm = TilePagePool::supportsHeightUnorm(device);
    _materialSolid = createLandscapeMaterial(false, false, heightUnorm);
    _materialWire = createLandscapeMaterial(true, false, heightUnorm);
    _decals = std::make_unique<DecalRenderer>();
    const bool decalsReady = _decals->init(node, scene, heightUnorm);
    if (std::any_of(_meshes.begin(), _meshes.end(), [](const auto &mesh) { return mesh == nullptr; }) ||
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
        setRuntimeTexture("heightmap", _tilePages->heightArray(), _tilePages->heightSampler());
        setRuntimeTexture("terrainNormalMap", _tilePages->normalArray(), _tilePages->heightSampler());
        setRuntimeTexture("vtAlbedo", vt.albedo(), vt.sampler());
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
            static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER),
            static_cast<float>(config::VT_FILTER_MARGIN)});
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

LandscapeSurfaceInstance LandscapeRenderer::resolveSurface(const Patch &patch) {
    LandscapeSurfaceInstance surface;
    surface.needsMaterial = patch.needsMaterial;

    const auto &node = patch.node;
    const auto region = _data.nodeRegion(node.address());
    surface.grid = Vec4{region.x, region.z, region.size, static_cast<float>(node.level)};
    surface.quadrant = Vec4{patch.x / 16.0F, patch.z / 16.0F,
        static_cast<float>(1U << patch.meshIndex) / 16.0F, 0.0F};

    const auto tile = _sourceResolver->resolve(node.address());
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] missing resident root: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.address.level, tile.address.x, tile.address.z, tile.layer);
    // a_tileInst.xy = source tile origin in landscape-local meters,
    // z = source tile size in meters, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    surface.tile = _sourceResolver->shaderParams(tile);
    if (!patch.needsMaterial) {
        return surface;
    }
    surface.normalParent = _sourceResolver->shaderParams(_sourceResolver->normalParent(node.address(), tile));
    surface.vt = _vtRenderer->mapping(patch.page);
    return surface;
}

void LandscapeRenderer::updateInstanceData(scene::Model *model, const Patch &patch) {
    if (model) {
        resolveSurface(patch).apply(model, _instanceAttributeScratch);
    }
}

LandscapeLocalRegion LandscapeRenderer::patchRegion(const Patch &patch) const {
    auto region = _data.nodeRegion(patch.node.address());
    const float cell = region.size / 16.0F;
    region.x += patch.x * cell;
    region.z += patch.z * cell;
    region.size = (1U << patch.meshIndex) * cell;
    return region;
}

float LandscapeRenderer::patchDistance(const QuadNode &node, float x, float z, float size, bool farthest) const {
    // Match the translation-only terrain placement used by quadtree selection.
    const Vec3 camera = _viewPosition - _node->getWorldPosition();
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
    const float nodeSize = _data.nodeSize(node.level);
    const float cellSize = nodeSize / 16.0F;
    const uint32_t cells = 1U << meshIndex;
    const float size = cells * cellSize;
    const float minX = node.ix * nodeSize + x * cellSize;
    const float minZ = node.iz * nodeSize + z * cellSize;
    const auto local = _data.gridToLocal({minX, minZ});
    const float localX = static_cast<float>(local.x), localZ = static_cast<float>(local.z);
    const float distance = patchDistance(node, localX, localZ, size, false);
    float density = 1.0F;
    if (_debugData.cliffEnabled) {
        float minY = node.minY, maxY = node.maxY;
        const uint32_t rangeLevel = node.level + meshIndex >= 4U ? node.level + meshIndex - 4U : 0U;
        const auto address = _data.nodeAtGridClamped(rangeLevel, {minX, minZ});
        _asset->getHeightRange(rangeLevel, address.x, address.z, minY, maxY);
        density = cliffDensityScale(maxY - minY, size, _asset->getSurfaceStretch(rangeLevel, address.x, address.z));
    }
    const uint32_t desired = _vtLayout.levelForDistance(distance / density);
    if (meshIndex > 0 && _vtLayout.pageSize(desired) < size) {
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
    const auto page = _vtLayout.coveringPage(desired, {{safeMinX, safeMinZ}, {minX + size, minZ + size}});
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
    const float cellSize = _data.nodeSize(node.level) / 16.0F;
    const auto region = patchRegion(patch);
    const float x = region.x, z = region.z, size = region.size;
    model->createBoundingShape(Vec3{x - (patch.x & 1U) * cellSize, node.minY, z - (patch.z & 1U) * cellSize},
                                Vec3{x + size, node.maxY, z + size});
    model->updateWorldBound();
}

void LandscapeRenderer::preparePasses(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    if (!valid() || _initializationFailed) {
        return;
    }
    sync(geometryNodes, surfaceNodes);
    if (_initializationFailed) {
        return;
    }
    if (!_ready) {
        return;
    }
    const auto stamp = Root::getInstance()->getFrameCount();
    for (const auto &entry : _active) {
        entry.second.model->updateTransform(stamp);
        entry.second.model->updateUBOs(stamp);
    }
    _decals->preparePasses(stamp);
}

void LandscapeRenderer::rebuildNodeModels() {
    _nodeModels.clear();
    const auto add = [this](const Patch &patch, scene::Model *model) {
        const uint32_t quadrant = (patch.x / 8U) + (patch.z / 8U) * 2U;
        _nodeModels[makeNodeKey(patch.node)].push_back({static_cast<uint8_t>(1U << quadrant), model});
    };
    for (const auto &entry : _active) {
        add(entry.second.patch, entry.second.model);
    }
}

void LandscapeRenderer::collectPassModels(const ccstd::vector<QuadNode> &selected, const geometry::Frustum &frustum,
                                          bool shadow, ccstd::vector<const scene::Model *> &models) const {
    if (!_ready) {
        return;
    }
    for (const auto &node : selected) {
        const auto it = _nodeModels.find(makeNodeKey(node));
        if (it == _nodeModels.end()) {
            continue;
        }
        for (const auto &entry : it->second) {
            const auto *model = entry.model;
            if (!(node.quadrantMask & entry.quadrantMask) || !model->isEnabled() || (shadow && !model->isCastShadow())) {
                continue;
            }
            // Material patches and raised decals may occupy only part of a selected node.
            if (model->getWorldBounds()->aabbFrustum(frustum)) {
                models.push_back(model);
            }
        }
    }
    _decals->collectPassModels(selected, frustum, shadow, models);
}

void LandscapeRenderer::onGlobalPipelineStateChanged() {
    for (const auto &entry : _active) {
        entry.second.model->onGlobalPipelineStateChanged();
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) {
            model->onGlobalPipelineStateChanged();
        }
    }
    if (_decals) {
        _decals->onGlobalPipelineStateChanged();
    }
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    if (!valid()) {
        return;
    }
    auto &pages = _vtRenderer->pages();
    const bool frozen = _ready && _debugData.freezeLod;
    const auto &origin = _node->getWorldPosition();
    const SyncCache::Positions positions{
        _viewPosition.x, _viewPosition.y, _viewPosition.z, origin.x, origin.y, origin.z};

    // Poll once, while the previous frame's source set is still protected.
    // No drawing happens until changed source mappings have been resolved below.
    if (_ready && !frozen) {
        _tilePages->update(config::PAGE_UPLOAD_BUDGET);
    }
    if (_ready && (frozen || pages.ready()) &&
        _syncCache.matches(positions, _tilePages->updateRevision(), pages.revision(), geometryNodes, surfaceNodes)) {
        return;
    }

    buildFramePlan(geometryNodes, surfaceNodes);
    if (!frozen) {
        if (!preparePageSources(geometryNodes, surfaceNodes)) {
            _initializationFailed = true;
            CC_LOG_ERROR("[Landscape] Initial source warmup failed. Fix assets/capacity and re-enable Landscape.");
            return;
        }
        // Compose before resolving instance mappings: newly published fine pages
        // are usable in THIS frame, with no extra publication/rebinding frame.
        _vtRenderer->render(_ready ? config::VT_PAGE_UPDATE_BUDGET : config::VT_PAGE_COUNT);
        if (!_ready) {
            if (!pages.ready() || !_vtRenderer->sourcesReady()) {
                _initializationFailed = true;
                CC_LOG_ERROR("[Landscape] Initial VT warmup failed. Fix sources/capacity and re-enable Landscape.");
                return;
            }
            _ready = true;
            _vtRenderer->setFrozen(_debugData.freezeLod);
            CC_LOG_INFO("[Landscape] Initial view ready: %zu geometry nodes, %zu VT pages",
                        geometryNodes.size(), pages.activeSlots().size());
        }
    }
    syncTerrainModels();
    syncDecals(_patches);
    rebuildNodeModels();
    _syncCache.store(positions, _tilePages->updateRevision(), pages.revision(), geometryNodes, surfaceNodes);
}

void LandscapeRenderer::buildFramePlan(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
    _patches.clear();
    _patches.reserve(geometryNodes.size() * 4U);
    for (const auto &node : surfaceNodes) {
        for (uint32_t q = 0; q < 4; ++q) {
            if ((node.quadrantMask & (1U << q)) != 0) {
                selectPatches(node, (q & 1U) * 8U, (q >> 1U) * 8U, 3U, _patches);
            }
        }
    }
    if (!_ready || !_debugData.freezeLod) {
        auto &pages = _vtRenderer->pages();
        pages.beginRequests();
        for (const auto &patch : _patches) {
            pages.request(patch.page, patch.vtPriority);
        }
        pages.endRequests();
    }

    // Only color passes drive VT subdivision/requests. Each shadow-only quadrant
    // uses one 8x8 grid with the same vertices, LOD and morph as the color pass.
    collectShadowOnlyNodes(geometryNodes, surfaceNodes, _shadowOnlyNodes);
    for (const auto &node : _shadowOnlyNodes) {
        for (uint32_t q = 0; q < 4; ++q) {
            if ((node.quadrantMask & (1U << q)) == 0) {
                continue;
            }
            _patches.push_back(Patch{node, (q & 1U) * 8U, (q >> 1U) * 8U, 3U, {}, 0.0F, false});
        }
    }
}

bool LandscapeRenderer::preparePageSources(const ccstd::vector<QuadNode> &geometryNodes,
                                            const ccstd::vector<QuadNode> &surfaceNodes) {
    _sourceResolver->beginFrame(!_ready);
    resolveFrameSources(geometryNodes, surfaceNodes);
    if (_ready) {
        return true; // Normal streaming was polled once at frame entry.
    }

    // Warmup may reveal finer normal neighborhoods or geometry parent sources.
    // Slot admission is already final: these rounds only load/resolve inputs.
    for (uint32_t round = 0; round <= config::MAX_LOD_LEVELS; ++round) {
        if (!_tilePages->loadRequestedTiles()) {
            return false;
        }
        resolveFrameSources(geometryNodes, surfaceNodes);
        if (_tilePages->requestsReady()) {
            return true;
        }
    }
    return false;
}

void LandscapeRenderer::resolveFrameSources(const ccstd::vector<QuadNode> &geometryNodes,
                                             const ccstd::vector<QuadNode> &surfaceNodes) {
    _sourceResolver->invalidate();
    _sourceResolver->protectGeometrySources(geometryNodes, surfaceNodes);
    _vtRenderer->resolveSources(*_tilePages, *_sourceResolver);
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
            if (model == nullptr) {
                continue;
            }
            iter = _active.emplace(key, ModelState{model, patch}).first;
            updateModel(iter->second, patch);
        } else {
            auto &state = iter->second;
            const bool boundsChanged = state.patch.node.minY != patch.node.minY || state.patch.node.maxY != patch.node.maxY;
            state.patch = patch;
            updateInstanceData(state.model, patch);
            if (boundsChanged) {
                updateModelBounds(state.model, patch);
            }
        }
    }
}

void LandscapeRenderer::syncDecals(const ccstd::vector<Patch> &patches) {
    if (!_decals->beginSync(_viewPosition)) {
        return;
    }
    for (const auto &patch : patches) {
        const auto region = patchRegion(patch);
        const float cell = _data.nodeSize(patch.node.level) / 16.0F;
        const uint32_t quadrant = patch.x / 8U + (patch.z / 8U) * 2U;
        _decals->addPatch({makeNodeKey(patch.node), static_cast<uint8_t>(1U << quadrant), region,
            Vec3{region.x - (patch.x & 1U) * cell, patch.node.minY, region.z - (patch.z & 1U) * cell},
            Vec3{region.x + region.size, patch.node.maxY, region.z + region.size}, resolveSurface(patch)});
    }
    _decals->endSync();
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
    }
    if (!frozen) {
        setCliffEnabled(_debugData.cliffEnabled);
    }
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        updateModelBounds(state.model, state.patch);
    }
}

void LandscapeRenderer::setCastShadow(bool enabled) {
    if (_castShadow == enabled) {
        return;
    }
    _castShadow = enabled;
    for (const auto &entry : _active) {
        entry.second.model->setCastShadow(enabled);
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) {
            model->setCastShadow(enabled);
        }
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
    for (const auto &entry : _active) {
        entry.second.model->setReceiveShadow(enabled);
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) {
            model->setReceiveShadow(enabled);
        }
    }
    if (_decals) {
        _decals->setReceiveShadow(enabled);
    }
    // The receive variant adds an instanced shadow-bias attribute. Rebuild the
    // layouts and restore terrain attributes, including frozen active models.
    setWireframe(_debugData.wireframe);
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _debugData.wireframe = wireframe;
    if (_decals) {
        _decals->setWireframe(wireframe);
    }
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        state.model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
        updateInstanceData(state.model, state.patch);
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) {
            model->setSubModelMaterial(0, _debugData.wireframe ? _materialWire.get() : _materialSolid.get());
        }
        // Inactive models get fresh instance data when reused. Do not request
        // height/VT pages here: doing so would pin invisible nodes in the caches.
    }
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

RenderTexture *LandscapeRenderer::vtAtlas() const {
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
    _initializationFailed = false;
    _syncCache.invalidate();
    _sourceResolver.reset();
    _patches.clear();
    _shadowOnlyNodes.clear();
    _visiblePatches.clear();
    const auto removeModel = [](const IntrusivePtr<scene::Model> &model) {
        if (model == nullptr) {
            return;
        }
        model->detachFromScene();
        model->destroy();
    };

    for (const auto &entry : _active) {
        removeModel(entry.second.model);
    }
    for (auto &pool : _pool) {
        for (const auto &model : pool) {
            removeModel(model);
        }
        pool.clear();
    }
    _active.clear();
    _nodeModels.clear();
    _instanceAttributeScratch = ccstd::monostate{};
    _vtRenderer.reset();
    _decals.reset();

    _tilePages.reset();
    _asset = nullptr;
    for (auto &mesh : _meshes) {
        mesh = nullptr;
    }
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _materialLibrary.reset();
    _node = nullptr;
    _scene = nullptr;
    _debugData.unlit = false;
}

} // namespace landscape
} // namespace cc

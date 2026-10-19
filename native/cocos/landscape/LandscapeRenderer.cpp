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
#include <chrono>
#include <cmath>
#include <cstdint>

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
    for (uint32_t i = 0; i < _meshes.size(); ++i) {
        _meshes[i] = GridMesh::create(device, 1U << i);
    }
    const bool heightUnorm = TilePagePool::supportsHeightUnorm(device);
    _materialSolid = createLandscapeMaterial(false, heightUnorm);
    _materialWire = createLandscapeMaterial(true, heightUnorm);
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
        material->setPropertyVec4("holeParams", Vec4{_holeMode == 0U ? 0.0F : 1.0F, 0, 0, 0});
        material->setPropertyVec4("terrainParams", terrainParams);
        material->setPropertyVec4("sectorParams", Vec4{_data.sectorSize, 0, 0, 0});
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

LandscapeSurfaceInstance LandscapeRenderer::resolveSurface(const Patch &patch) {
    LandscapeSurfaceInstance surface;

    const auto &node = patch.node;
    const auto region = _data.nodeRegion(node.address());
    surface.grid = Vec4{region.x, region.z, region.size, static_cast<float>(node.level)};
    surface.quadrant = Vec4{patch.x / 16.0F, patch.z / 16.0F,
                            static_cast<float>(1U << patch.meshIndex) / 16.0F, static_cast<float>(_selection.edgeMask(node.address()))};

    const auto tile = _sourceResolver->resolve(node.address());
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] unprepared exact height source: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.address.level, tile.address.x, tile.address.z, tile.layer);
    // a_tileInst.xy = source tile origin in landscape-local coordinates,
    // z = source tile size, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    surface.tile = _sourceResolver->shaderParams(tile);
    surface.vt = _vtRenderer->mapping(patch.page);
    return surface;
}

void LandscapeRenderer::updateInstanceData(scene::Model *model, const Patch &patch) {
    if (model) {
        resolveSurface(patch).apply(model, _instanceAttributeScratch);
    }
}

LandscapeRegion LandscapeRenderer::patchRegion(const Patch &patch) const {
    auto region = _data.nodeRegion(patch.node.address());
    const float cell = region.size / 16.0F;
    region.x += patch.x * cell;
    region.z += patch.z * cell;
    region.size = (1U << patch.meshIndex) * cell;
    return region;
}

float LandscapeRenderer::patchDistance(const QuadNode &node, float x, float z, float size) const {
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

void LandscapeRenderer::selectPatches(const QuadNode &node, uint32_t x, uint32_t z, uint32_t meshIndex, uint32_t bias,
                                      ccstd::vector<Patch> &patches) {
    const float nodeSize = _data.nodeSize(node.level);
    const float cellSize = nodeSize / 16.0F;
    const uint32_t cells = 1U << meshIndex;
    const float size = cells * cellSize;
    const float minX = node.ix * nodeSize + x * cellSize;
    const float minZ = node.iz * nodeSize + z * cellSize;
    const float distance = patchDistance(node, minX, minZ, size);
    const uint32_t desired = std::min(_vtLayout.rootLevel(), _vtLayout.levelForDistance(distance) + bias);
    if (meshIndex > 0 && _vtLayout.pageSize(desired) < size) {
        const uint32_t half = cells / 2U;
        for (uint32_t q = 0; q < 4; ++q) {
            selectPatches(node, x + (q & 1U) * half, z + (q >> 1U) * half, meshIndex - 1U, bias, patches);
        }
        return;
    }
    // Stitching follows committed neighbors, including near-camera boundaries
    // delayed by streaming. Page coverage must not depend on distance morphing.
    const auto page = _vtLayout.coveringPatch(desired, {node.ix * nodeSize, node.iz * nodeSize}, cellSize, x, z, cells);
    patches.push_back(Patch{node, x, z, meshIndex, page, {}});
}

void LandscapeRenderer::updateModel(Patch &patch) {
    auto *model = patch.model.get();
    updateInstanceData(model, patch);
    model->setEnabled(true);
    updateModelBounds(model, patch);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const Patch &patch) {
    const auto &node = patch.node;
    const float cellSize = _data.nodeSize(node.level) / 16.0F;
    const auto region = patchRegion(patch);
    const float x = region.x;
    const float z = region.z;
    const float size = region.size;
    model->createBoundingShape(Vec3{x - (patch.x & 1U) * cellSize, node.minY, z - (patch.z & 1U) * cellSize},
                               Vec3{x + size, node.maxY, z + size});
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
        for (const auto &patch : entry.second) {
            if (patch.model) {
                patch.model->updateTransform(stamp);
                patch.model->updateUBOs(stamp);
            }
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
    geometry::AABB bounds;
    _selection.cull(
        [&](NodeAddress address) {
            const auto draw = _draws.find(address.key());
            const auto node = draw != _draws.end() ? draw->second.front().node : drawNode(address);
            const auto region = _data.nodeRegion(address);
            // Include the bound source's height range (also for shared sources
            // below minTileLevel) and raised decals before pruning a subtree.
            const float top = node.maxY + decalLift;
            bounds.setCenter(region.x + region.size * 0.5F + origin.x,
                             (node.minY + top) * 0.5F + origin.y,
                             region.z + region.size * 0.5F + origin.z);
            bounds.setHalfExtents(region.size * 0.5F, (top - node.minY) * 0.5F, region.size * 0.5F);
            return bounds.aabbFrustum(frustum);
        },
        [&](NodeAddress address) {
            const auto draw = _draws.find(address.key());
            if (draw == _draws.end() || draw->second.empty()) {
                return;
            }
            const auto &patches = draw->second;
            for (const auto &patch : patches) {
                const auto *model = patch.model.get();
                if (model && model->isEnabled() && (!shadow || model->isCastShadow()) && model->getWorldBounds()->aabbFrustum(frustum)) {
                    models.push_back(model);
                }
            }
            _decals->collectPassModels(patches.front().node, frustum, shadow, models);
        });
}

void LandscapeRenderer::onGlobalPipelineStateChanged() {
    for (const auto &entry : _draws) {
        for (const auto &patch : entry.second) {
            if (patch.model) {
                patch.model->onGlobalPipelineStateChanged();
            }
        }
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

void LandscapeRenderer::buildPatches(NodeAddress address, uint32_t bias, ccstd::vector<Patch> &patches) {
    patches.clear();
    // Visibility must not force a ready surface to sector-root resolution.
    // Distance and admission capacity determine quality, including cached
    // offscreen coverage that can become visible on the next camera turn.
    const auto node = drawNode(address);
    for (uint32_t q = 0; q < 4; ++q) {
        selectPatches(node, (q & 1U) * 8U, (q >> 1U) * 8U, 3, bias, patches);
    }
}

void LandscapeRenderer::updateReferences(uint64_t key, const ccstd::vector<Patch> &patches, bool add) {
    const auto update = [add](auto &refs, uint64_t address) {
        if (add) {
            ++refs[address];
        } else if (--refs.at(address) == 0) {
            refs.erase(address);
        }
    };
    const auto node = NodeAddress::fromKey(key);
    update(_heightRefs, node.ancestor(std::max(node.level, _data.minTileLevel)).key());
    for (const auto &patch : patches) {
        update(_pageRefs, patch.page.key());
    }
}

bool LandscapeRenderer::planResources(bool reserve) {
    auto &pages = _vtRenderer->pages();
    ccstd::unordered_map<uint64_t, int> heightDelta, pageDelta;
    for (const auto node : _change.removed) {
        const auto it = _draws.find(node.key());
        if (it == _draws.end()) {
            continue;
        }
        --heightDelta[node.ancestor(std::max(node.level, _data.minTileLevel)).key()];
        for (const auto &patch : it->second) {
            --pageDelta[patch.page.key()];
        }
    }
    _workingPages.clear();
    ccstd::vector<NodeAddress> baseline;
    for (const auto &entry : _heightRefs) {
        baseline.push_back(NodeAddress::fromKey(entry.first));
    }
    for (const auto &entry : _pageRefs) {
        const auto n = NodeAddress::fromKey(entry.first);
        _workingPages.push_back({n.level, n.x, n.z});
    }
    for (const auto &entry : _pendingDraws) {
        const auto node = NodeAddress::fromKey(entry.first);
        const auto source = node.ancestor(std::max(node.level, _data.minTileLevel));
        ++heightDelta[source.key()];
        baseline.push_back(source);
        for (const auto &patch : entry.second) {
            ++pageDelta[patch.page.key()];
            _workingPages.push_back(patch.page);
        }
    }
    const auto countAfter = [](const auto &refs, const auto &delta) {
        size_t count = refs.size();
        for (const auto &entry : delta) {
            const auto it = refs.find(entry.first);
            const int before = it == refs.end() ? 0 : static_cast<int>(it->second);
            if (before == 0 && entry.second > 0) {
                ++count;
            } else if (before > 0 && before + entry.second == 0) {
                --count;
            }
        }
        return count;
    };
    ccstd::vector<NodeAddress> fixedSources;
    _vtRenderer->requiredSources(fixedSources);
    ccstd::unordered_set<uint64_t> fixed;
    size_t currentHeights = _heightRefs.size();
    for (const auto n : fixedSources) {
        if (fixed.insert(n.key()).second && !_heightRefs.count(n.key())) {
            ++currentHeights;
        }
    }
    size_t nextHeights = currentHeights;
    for (const auto &entry : heightDelta) {
        const auto it = _heightRefs.find(entry.first);
        const int before = (it == _heightRefs.end() ? 0 : static_cast<int>(it->second)) + (fixed.count(entry.first) ? 1 : 0);
        if (before == 0 && entry.second > 0) {
            ++nextHeights;
        } else if (before > 0 && before + entry.second == 0) {
            --nextHeights;
        }
    }
    const auto nextPages = countAfter(_pageRefs, pageDelta);
    // Keep exchange space so a completely occupied displayed cut can coarsen
    // after the camera moves. Both old and new bindings are pinned until commit.
    const auto headroom = std::min(size_t{80}, static_cast<size_t>(_tilePages->layerCount()) / 4);
    if (_initialized && ((nextHeights > currentHeights && nextHeights + headroom > _tilePages->layerCount()) ||
                         (nextPages > _pageRefs.size() && nextPages + 4 > pages.capacity()))) {
        return false;
    }
    std::sort(_workingPages.begin(), _workingPages.end(), [](auto a, auto b) { return a.key() < b.key(); });
    _workingPages.erase(std::unique(_workingPages.begin(), _workingPages.end(), [](auto a, auto b) { return a.key() == b.key(); }), _workingPages.end());
    baseline.insert(baseline.end(), fixedSources.begin(), fixedSources.end());
    if (!pages.canReserve(_workingPages) || !_tilePages->canReserve(baseline)) {
        return false;
    }
    ccstd::unordered_set<uint64_t> baselineKeys;
    baselineKeys.reserve(baseline.size());
    _workingSources.clear();
    for (const auto source : baseline) {
        if (baselineKeys.insert(source.key()).second) {
            _workingSources.push_back(source);
        }
    }
    auto workingKeys = baselineKeys;
    for (const auto page : _workingPages) {
        if (!pages.needsComposition(page)) {
            continue;
        }
        ccstd::vector<NodeAddress> inputs;
        _sourceResolver->pageSources(page, _vtRenderer->isBakeNormalEnabled(), inputs);
        if (!_tilePages->canReserve(inputs)) {
            return false;
        }
        ccstd::unordered_set<uint64_t> uniqueInputs;
        size_t extraBaseline = 0;
        size_t extraWorking = 0;
        for (const auto source : inputs) {
            if (!uniqueInputs.insert(source.key()).second) {
                continue;
            }
            extraBaseline += baselineKeys.count(source.key()) == 0;
            extraWorking += workingKeys.count(source.key()) == 0;
        }
        if (baselineKeys.size() + extraBaseline > _tilePages->layerCount()) {
            return false;
        }
        if (workingKeys.size() + extraWorking <= _tilePages->layerCount()) {
            for (const auto source : inputs) {
                if (workingKeys.insert(source.key()).second) {
                    _workingSources.push_back(source);
                }
            }
        }
    }
    return !reserve || (pages.reserve(_workingPages) && _tilePages->reserve(_workingSources));
}

bool LandscapeRenderer::stage(const RenderSelection::Change &change, bool geometry, bool roots) {
    _change = change;
    _changingGeometry = geometry;
    const auto maxBias = _vtLayout.rootLevel();
    for (uint32_t bias = roots ? maxBias : 0; bias <= ((geometry || roots) ? maxBias : 0U); ++bias) {
        _pendingDraws.clear();
        bool changed = geometry || roots;
        for (const auto node : change.added) {
            auto &patches = _pendingDraws[node.key()];
            buildPatches(node, bias, patches);
            const auto old = _draws.find(node.key());
            if (old == _draws.end() || old->second.size() != patches.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < patches.size(); ++i) {
                    const auto &a = old->second[i];
                    const auto &b = patches[i];
                    if (a.x != b.x || a.z != b.z || a.meshIndex != b.meshIndex || a.page.key() != b.page.key()) {
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
        if (planResources(false)) {
            _reserved = false;
            return true;
        }
    }
    clearPending();
    return false;
}

void LandscapeRenderer::resetPlanning() {
    _requests = _selection.requests();
    _requestIndex = _materialIndex = 0;
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
        ordered.push_back({n, patchDistance(drawNode(n), region.x, region.z, region.size), colorRegion(n)});
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

bool LandscapeRenderer::planNext(uint32_t &attempts) {
    while (attempts > 0) {
        --attempts;
        RenderSelection::Change change;
        // Release geometry first, then alternate material and geometry work.
        // Newly visible materials must not wait for every geometry split.
        const bool geometryPending = _requestIndex < _requests.size();
        const bool materialPending = _materialIndex < _materialNodes.size();
        if (geometryPending && (!_requests[_requestIndex].split || !materialPending || !_preferMaterial)) {
            if (_selection.plan(_requests[_requestIndex++], change) && stage(change, true)) {
                return true;
            }
        } else if (materialPending) {
            const auto node = _materialNodes[_materialIndex++];
            change.removed.push_back(node);
            change.added.push_back(node);
            if (stage(change, false)) {
                return true;
            }
        } else {
            if (_planningInvalid) {
                resetPlanning();
            } else {
                _settled = true;
            }
            return false;
        }
    }
    return false;
}

void LandscapeRenderer::commitPending() {
    for (const auto node : _change.removed) {
        _dirtyNodes.insert(node.key());
        if (_changingGeometry) {
            _selection.neighbors(node, [this](NodeAddress n, uint32_t) { _dirtyNodes.insert(n.key()); });
        }
        const auto it = _draws.find(node.key());
        if (it != _draws.end()) {
            updateReferences(node.key(), it->second, false);
            recycleModels(it->second);
            _draws.erase(it);
        }
    }
    if (_changingGeometry) {
        _selection.commit(_change);
    }
    for (auto &entry : _pendingDraws) {
        const auto node = NodeAddress::fromKey(entry.first);
        _dirtyNodes.insert(entry.first);
        updateReferences(entry.first, entry.second, true);
        _draws[entry.first] = std::move(entry.second);
        if (_changingGeometry) {
            _selection.neighbors(node, [this](NodeAddress n, uint32_t) { _dirtyNodes.insert(n.key()); });
        }
    }
    clearPending();
    const bool first = !_initialized;
    _initialized = true;
    _preferMaterial = _changingGeometry;
    if (_changingGeometry || first) {
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
    _pendingDraws.clear();
    _change = {};
    _reserved = false;
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
    if (_change.empty() && pages.ready() && _reserved) {
        return true;
    }
    if (!_reserved || _preparedPageRevision != pages.revision()) {
        if (!planResources(true)) {
            const bool rejectedChange = !_change.empty();
            clearPending();
            // Capacity refusal cancels the complete candidate, including
            // any neighbor splits. Keep the committed cut and retry its
            // resources in both warmup and streaming. Do not turn a
            // refused detail upgrade into a permanent loading failure.
            if (!rejectedChange || !_initialized || !planResources(true)) {
                CC_LOG_WARNING("[Landscape] Resource reservation deferred; retaining committed bindings.");
                return false;
            }
        }
        _reserved = true;
        _preparedPageRevision = pages.revision();
    }
    const auto sourceRevision = _tilePages->updateRevision();
    const auto pageRevision = pages.revision();
    if (!_tilePages->loadReserved(synchronous)) {
        stopLoading();
        return false;
    }
    const auto composed = _vtRenderer->render(*_tilePages, *_sourceResolver, synchronous ? config::VT_PAGE_COUNT : _streamingBudget.pages);
    if (!synchronous) {
        _streamingBudget.pages -= composed;
    }
    if (_tilePages->ready() && pages.ready() && _vtRenderer->sourcesReady()) {
        if (!_change.empty()) {
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
    const auto nowMs = []() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const bool firstSync = _streamingBudget.begin(Root::getInstance()->getFrameCount(), nowMs());
    if (_ready && firstSync) {
        _tilePages->update(config::PAGE_UPLOAD_BUDGET);
    }
    if (_ready && _settled && _change.empty() && pages.ready() &&
        _syncCache.matches(positions, _tilePages->updateRevision(), pages.revision(), geometryNodes, surfaceNodes)) {
        return;
    }
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
        if (selectionChanged || (_change.empty() && (_settled || !_initialized))) {
            resetPlanning();
        } else {
            _planningInvalid = true;
        }
    }
    if (!_initialized && _change.empty()) {
        RenderSelection::Change roots;
        for (const auto key : _selection.leaves()) {
            roots.added.push_back(NodeAddress::fromKey(key));
        }
        if (!stage(roots, false, true)) {
            _loadingFailed = true;
            CC_LOG_ERROR("[Landscape] Initial coverage exceeds resource capacity.");
            return;
        }
    }
    // Warmup and streaming use the same capacity admission and atomic commits.
    // Warmup loads synchronously until no more changes fit; it need not reach
    // the requested detail. Runtime work additionally shares per-frame budgets.
    const bool warmingUp = !_ready;
    do {
        if (!warmingUp && !_streamingBudget.nextStep(nowMs())) {
            break;
        }
        if (_change.empty() && pages.ready()) {
            uint32_t warmupAttempts = 256;
            planNext(warmingUp ? warmupAttempts : _streamingBudget.attempts);
        }
        if (!updateResources(warmingUp)) {
            break;
        }
        if (_change.empty() && _settled && pages.ready()) {
            if (!_ready) {
                CC_LOG_INFO("[Landscape] Initial view ready: %zu draw nodes, %zu exact VT pages", _draws.size(), _pageRefs.size());
            }
            _ready = true;
            _vtRenderer->setFrozen(_debugData.freezeLod);
        }
        // Never spin waiting for IO, uploads or a partially composed transaction.
        // A ready commit can immediately hand the remaining budget to the next.
    } while (warmingUp ? !_ready : (_change.empty() && !_settled && pages.ready() && _streamingBudget.attempts > 0));
    if (!_dirtyNodes.empty()) {
        syncTerrainModels();
    }
    syncDecals();
    _syncCache.store(positions, _tilePages->updateRevision(), pages.revision(), geometryNodes, surfaceNodes);
}

void LandscapeRenderer::recycleModels(ccstd::vector<Patch> &patches) {
    for (auto &patch : patches) {
        if (patch.model) {
            patch.model->setEnabled(false);
            _pool[patch.meshIndex].push_back(std::move(patch.model));
        }
    }
}

void LandscapeRenderer::syncTerrainModels() {
    // Models live with the committed patches. Neighbors only refresh their stitch
    // attributes; replaced patches acquire models from the matching grid pool.
    for (const auto key : _dirtyNodes) {
        const auto draw = _draws.find(key);
        if (draw == _draws.end()) {
            continue;
        }
        for (auto &patch : draw->second) {
            if (!patch.model) {
                auto &pool = _pool[patch.meshIndex];
                if (pool.empty()) {
                    patch.model = createModel(patch.meshIndex);
                } else {
                    patch.model = std::move(pool.back());
                    pool.pop_back();
                }
            }
            if (patch.model) {
                updateModel(patch);
            }
        }
    }
    _dirtyNodes.clear();
}

void LandscapeRenderer::syncDecals() {
    if (!_decals->beginSync(_viewPosition)) {
        return;
    }
    for (const auto &entry : _draws) {
        for (const auto &patch : entry.second) {
            const auto region = patchRegion(patch);
            const float cell = _data.nodeSize(patch.node.level) / 16.0F;
            const uint32_t quadrant = patch.x / 8U + (patch.z / 8U) * 2U;
            _decals->addPatch({patch.node.address().key(), static_cast<uint8_t>(1U << quadrant), region,
                               Vec3{region.x - (patch.x & 1U) * cell, patch.node.minY, region.z - (patch.z & 1U) * cell},
                               Vec3{region.x + region.size, patch.node.maxY, region.z + region.size}, resolveSurface(patch)});
        }
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
        setCliffEnabled(_debugData.cliffEnabled);
    }
    for (const auto &entry : _draws) {
        for (const auto &patch : entry.second) {
            if (patch.model) {
                updateModelBounds(patch.model, patch);
            }
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
        for (const auto &patch : entry.second) {
            if (patch.model) {
                patch.model->setCastShadow(enabled);
            }
        }
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
    for (const auto &entry : _draws) {
        for (const auto &patch : entry.second) {
            if (patch.model) {
                patch.model->setReceiveShadow(enabled);
            }
        }
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
    for (const auto &entry : _draws) {
        for (const auto &patch : entry.second) {
            if (patch.model) {
                patch.model->setSubModelMaterial(0, wireframe ? _materialWire.get() : _materialSolid.get());
                updateInstanceData(patch.model, patch);
            }
        }
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
    _loadingFailed = false;
    _syncCache.clear();
    _streamingBudget = {};
    _planningInvalid = false;
    _preferMaterial = true;
    clearPending();
    _heightRefs.clear();
    _pageRefs.clear();
    _dirtyNodes.clear();
    _colorPaths.clear();
    _colorSelected.clear();
    _requests.clear();
    _materialNodes.clear();
    _workingSources.clear();
    _workingPages.clear();
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
        for (const auto &patch : entry.second) {
            removeModel(patch.model);
        }
    }
    _draws.clear();
    for (auto &pool : _pool) {
        for (const auto &model : pool) {
            removeModel(model);
        }
        pool.clear();
    }
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

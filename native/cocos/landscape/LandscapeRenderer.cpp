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
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/RenderScene.h"
#include "scene/Octree.h"
#include "scene/Pass.h"

namespace cc {
namespace landscape {

namespace {
Material *createLandscapeMaterial(bool wireframe) {
    auto *material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-landscape"};
    MacroRecord defines;
    defines["USE_INSTANCING"] = true;
    defines["LANDSCAPE_DEBUG_UNLIT"] = false;
    info.defines = IMaterialInfo::DefinesType{defines};

    if (wireframe) {
        RasterizerStateInfo rasterizer;
        rasterizer.polygonMode = gfx::PolygonMode::LINE;
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
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (node == nullptr || scene == nullptr || device == nullptr) {
        return false;
    }

    destroy();
    _node = node;
    _scene = scene;
    for (uint32_t i = 0; i < _meshes.size(); ++i) _meshes[i] = GridMesh::create(device, 1U << i);
    _materialSolid = createLandscapeMaterial(false);
    _materialWire = createLandscapeMaterial(true);
    if (std::any_of(_meshes.begin(), _meshes.end(), [](const auto &mesh) { return mesh == nullptr; }) ||
        _materialSolid == nullptr || _materialWire == nullptr) {
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
    if (asset == nullptr || !asset->valid() || Root::getInstance() == nullptr) {
        return false;
    }
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (device == nullptr) {
        return false;
    }
    _vtRenderer.reset();
    _asset = asset;
    _data = asset->data();
    _vtRootLevel = vtRootLevel(_data.sectorSize);
    _heightSampleSpacing = computeNodeSize(_data.sectorSize, _data.maxLevel, _data.minTileLevel) /
                           static_cast<float>(_data.tileResolution - 1U);
    _tilePages = std::make_unique<TilePagePool>();
    if (!_tilePages->init(device, asset, config::PAGE_POOL_LAYERS)) {
        _tilePages.reset();
        return false;
    }
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
    _vtRenderer->setFrozen(_freezeLod);
    // Resource initialization succeeded. Bind the textures once; subsequent
    // parameter updates do not change these textures or their samplers.
    auto &vt = _vtRenderer->texture();
    for (auto *material : {_materialSolid.get(), _materialWire.get()}) {
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
        setRuntimeTexture("vtNormalRoughnessAO", vt.normalRoughnessAO(), vt.sampler());
    }
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::setGlobalColorStrength(float strength) {
    if (_vtRenderer) _vtRenderer->setGlobalColorStrength(strength);
}

void LandscapeRenderer::setDebugFlags(bool lodColor, bool showRanges) {
    _lodColor = lodColor;
    _showRanges = showRanges;
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
    model->initSubModel(0, _meshes[meshIndex], _wireframe ? _materialWire.get() : _materialSolid.get());
    model->setEnabled(false);
    _scene->addModel(model);
    return model;
}

void LandscapeRenderer::updateMaterialProperties() {
    if (_materialSolid == nullptr || _materialWire == nullptr) {
        return;
    }

    const Vec4 terrainParams{
        _data.heightScale,
        _data.heightBias,
        _lodColor ? 1.0F : 0.0F,
        _showRanges ? 1.0F : 0.0F,
    };
    ccstd::vector<Vec4> lodMorph(config::MAX_LOD_LEVELS);
    for (uint32_t i = 0; i < config::MAX_LOD_LEVELS; ++i) {
        const float start = i < _morphStart.size() ? _morphStart[i] : 0.0F;
        const float end = i < _morphEnd.size() ? _morphEnd[i] : 0.0F;
        lodMorph[i] = Vec4{start, end, 0.0F, 0.0F};
    }

    for (auto *material : {_materialSolid.get(), _materialWire.get()}) {
        material->setPropertyVec4("terrainParams", terrainParams);
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
    if (_materialSolid == nullptr || _materialWire == nullptr) {
        return;
    }

    const Vec4 cameraPosition{
        _viewPosition.x,
        _viewPosition.y,
        _viewPosition.z,
        0.0F,
    };
    for (auto *material : {_materialSolid.get(), _materialWire.get()}) {
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

    const TilePage tile = resolveTilePage(node);
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] missing resident root: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.level, tile.x, tile.z, tile.layer);
    const float sourceNodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, tile.level);
    const Vec4 tileParams{
        static_cast<float>(tile.x) * sourceNodeSize - _data.worldWidth() * 0.5F,
        static_cast<float>(tile.z) * sourceNodeSize - _data.worldDepth() * 0.5F,
        sourceNodeSize,
        static_cast<float>(tile.layer),
    };
    // a_tileInst.xy = source tile origin in landscape-local meters,
    // z = source tile size in meters, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    setInstanceAttribute(model, "a_tileInst", tileParams);
    setInstanceAttribute(model, "a_normalParentInst", this->tileParams(resolveNormalParent(node, tile)));
    const int vtSlot = resolveVTPage(patch.page);
    CC_ASSERTF(vtSlot >= 0, "[Landscape] missing permanent root VT page");
    const auto &vtRegion = _vtRenderer->texture().page(static_cast<uint32_t>(vtSlot)).region;
    // A fallback covers an ancestor's region, not the fine node's region.
    setInstanceAttribute(model, "a_vtInst", Vec4{vtRegion.x, vtRegion.y, vtRegion.z, static_cast<float>(vtSlot)});
}

int LandscapeRenderer::resolveVTPage(VTPageAddress page) const {
    auto &vt = _vtRenderer->texture();
    for (; page.level < _vtRootLevel; ++page.level, page.x >>= 1U, page.z >>= 1U) {
        const int slot = vt.findReadyPage(makeNodeKey(page.level, page.x, page.z));
        if (slot >= 0 && vt.requested(makeNodeKey(page.level, page.x, page.z))) return slot;
    }
    // Roots are pinned. On startup/invalidation their writes are guaranteed by
    // VTRenderer::render before the Base Pass; no unrendered fine page is sampled.
    return vt.findPage(makeNodeKey(_vtRootLevel, page.x, page.z));
}

Vec4 LandscapeRenderer::tileParams(const TilePage &tile) const {
    const float size = computeNodeSize(_data.sectorSize, _data.maxLevel, tile.level);
    return Vec4{tile.x * size - _data.worldWidth() * 0.5F,
                tile.z * size - _data.worldDepth() * 0.5F, size, static_cast<float>(tile.layer)};
}

LandscapeRenderer::TilePage LandscapeRenderer::resolveVTSource(const VTPageAddress &page) {
    // A material page can be finer than every height/splat tile. Choose the
    // containing source independently of the selected geometry node's LOD.
    const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxLevel && computeNodeSize(_data.sectorSize, _data.maxLevel, level) < size) ++level;
    const float sourceSize = computeNodeSize(_data.sectorSize, _data.maxLevel, level);
    return resolveTilePage(QuadNode{level, static_cast<uint32_t>((page.x + 0.5F) * size / sourceSize),
                                   static_cast<uint32_t>((page.z + 0.5F) * size / sourceSize)});
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
    const uint32_t desired = vtDesiredLevel(_data.sectorSize, _vtRootLevel, distance);
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
    patches.push_back(Patch{node, x, z, meshIndex, page});
}

LandscapeRenderer::TilePage LandscapeRenderer::resolveNormalParent(const QuadNode &node, const TilePage &tile) {
    // Finer geometry already shares the finest source normal map. A streaming
    // fallback likewise must not morph towards an extra-coarse level again.
    if (node.level != tile.level || node.level >= _data.maxLevel) return tile;
    return resolveTilePage(QuadNode{node.level + 1U, node.ix >> 1U, node.iz >> 1U});
}

LandscapeRenderer::TilePage LandscapeRenderer::resolveTilePage(const QuadNode &node) {
    if (_tilePages == nullptr || _asset == nullptr) {
        return {};
    }
    TilePage tile;
    tile.level = std::max(node.level, _data.minTileLevel);
    const uint32_t shift = tile.level - node.level;
    tile.x = node.ix >> shift;
    tile.z = node.iz >> shift;
    // Only the desired tile starts an asynchronous request. Ancestors are
    // queried without loading and remain protected while used as fallbacks.
    tile.layer = _tilePages->query(tile.level, tile.x, tile.z);
    while (tile.layer < 0 && tile.level < _data.maxLevel) {
        ++tile.level;
        tile.x >>= 1U;
        tile.z >>= 1U;
        tile.layer = _tilePages->peekResident(makeNodeKey(tile.level, tile.x, tile.z));
    }
    return tile; // initialization guarantees a resident root for every sector
}

void LandscapeRenderer::updateModel(ModelState &state, const Patch &patch) {
    auto *model = state.model.get();
    updateInstanceData(model, patch);
    state.patch = patch;
    model->setEnabled(true);
    updateModelBounds(model, patch);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const Patch &patch) {
    if (_freezeLod) {
        // The frozen selection already passed the terrain frustum test. Models
        // without world bounds bypass both forward and custom pipeline culling.
        // Remove any octree entry before clearing bounds to avoid stale queries.
        if (_scene != nullptr && _scene->getOctree() != nullptr) {
            _scene->getOctree()->remove(model);
        }
        model->setWorldBounds(nullptr);
        model->updateOctree();
        return;
    }

    const auto &node = patch.node;
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
    const float cellSize = nodeSize / 16.0F;
    const float size = (1U << patch.meshIndex) * cellSize;
    const float x = node.ix * nodeSize - _data.worldWidth() * 0.5F + patch.x * cellSize;
    const float z = node.iz * nodeSize - _data.worldDepth() * 0.5F + patch.z * cellSize;
    model->createBoundingShape(Vec3{x - (patch.x & 1U) * cellSize, node.minY, z - (patch.z & 1U) * cellSize},
                                Vec3{x + size, node.maxY, z + size});
    model->updateWorldBound();
    model->updateOctree();
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &selected) {
    if (_freezeLod || !valid()) {
        return;
    }

    ccstd::vector<Patch> patches;
    patches.reserve(selected.size() * 4U);
    for (const auto &node : selected) {
        for (uint32_t q = 0; q < 4; ++q) {
            if ((node.quadrantMask & (1U << q)) != 0) selectPatches(node, (q & 1U) * 8U, (q >> 1U) * 8U, 3U, patches);
        }
    }

    struct Request { VTPageAddress page; float priority; uint64_t key; };
    ccstd::unordered_map<uint64_t, Request> unique;
    for (const auto &patch : patches) {
        auto page = patch.page;
        for (; page.level < _vtRootLevel; ++page.level, page.x >>= 1U, page.z >>= 1U) {
            const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
            const float distance = patchDistance(patch.node, page.x * size - _data.worldWidth() * 0.5F,
                                                page.z * size - _data.worldDepth() * 0.5F, size, false);
            const float priority = size / std::max(distance, 1.0F);
            const uint64_t key = makeNodeKey(page.level, page.x, page.z);
            auto inserted = unique.emplace(key, Request{page, priority, key});
            inserted.first->second.priority = std::max(inserted.first->second.priority, priority);
        }
    }
    ccstd::vector<Request> requests;
    requests.reserve(unique.size());
    for (const auto &entry : unique) requests.push_back(entry.second);
    std::sort(requests.begin(), requests.end(), [](const Request &a, const Request &b) {
        return a.priority != b.priority ? a.priority > b.priority : a.key < b.key;
    });
    // Bound the working set BEFORE protecting pages. Keeping all desired keys
    // pinned would prevent near pages from replacing distant pages after moving.
    const size_t capacity = config::VT_PAGE_COUNT - static_cast<size_t>(_data.sectorsX) * _data.sectorsZ;
    if (requests.size() > capacity) requests.resize(capacity);
    ccstd::vector<uint64_t> keys;
    keys.reserve(requests.size());
    for (const auto &request : requests) keys.push_back(request.key);
    auto &vt = _vtRenderer->texture();
    vt.beginFrame(keys);
    _tilePages->beginFrame();
    // Protect both current and parent normals before recycling any tile layer.
    for (const auto &node : selected) resolveNormalParent(node, resolveTilePage(node));
    for (const auto &request : requests) resolveVTSource(request.page);
    _tilePages->update(config::PAGE_UPLOAD_BUDGET);
    for (const auto &request : requests) {
        const auto &page = request.page;
        const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
        const Vec4 region{page.x * size - _data.worldWidth() * 0.5F,
                          page.z * size - _data.worldDepth() * 0.5F, size, 0.0F};
        vt.acquirePage(request.key, region, tileParams(resolveVTSource(page)), false, request.priority);
    }

    ccstd::unordered_set<uint64_t> visible;
    visible.reserve(patches.size());
    for (const auto &patch : patches) {
        const uint64_t key = makeNodeKey(patch.node.level * 4U + patch.meshIndex,
                                        patch.node.ix * 16U + patch.x, patch.node.iz * 16U + patch.z);
        visible.insert(key);
    }
    // Retire old patches before creating replacements so zooming does not
    // unnecessarily grow the model pools.
    for (auto iter = _active.begin(); iter != _active.end();) {
        if (visible.count(iter->first) == 0) {
            iter->second.model->setEnabled(false);
            _pool[iter->second.patch.meshIndex].emplace_back(iter->second.model);
            iter = _active.erase(iter);
        } else {
            ++iter;
        }
    }
    for (const auto &patch : patches) {
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

void LandscapeRenderer::setFreezeLod(bool frozen) {
    if (_freezeLod == frozen) {
        return;
    }
    _freezeLod = frozen;
    if (_vtRenderer) _vtRenderer->setFrozen(frozen);
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        updateModelBounds(state.model, state.patch);
    }
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        state.model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        updateInstanceData(state.model, state.patch);
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        // Inactive models get fresh instance data when reused. Do not request
        // height/VT pages here: doing so would pin invisible nodes in the caches.
    }
}

void LandscapeRenderer::setViewPos(const Vec3 &position) {
    if (_freezeLod) return;
    _vtViewPosition = position;
    if (_viewPosition.x == position.x &&
        _viewPosition.y == position.y &&
        _viewPosition.z == position.z) {
        return;
    }
    _viewPosition = position;
    updateMorphCameraProperty();
}

RenderTexture *LandscapeRenderer::debugAtlas() const {
    return _vtRenderer ? _vtRenderer->texture().atlas() : nullptr;
}

void LandscapeRenderer::setUnlit(bool enabled) {
    if (_unlit == enabled || !valid()) return;
    // These passes belong exclusively to this renderer. Update their shader
    // variants in place, preserving texture bindings and instanced batching.
    const auto compile = [this](bool unlit) {
        bool success = true;
        for (auto *material : {_materialSolid.get(), _materialWire.get()}) {
            for (const auto &pass : *material->getPasses()) {
                pass->getDefines()["LANDSCAPE_DEBUG_UNLIT"] = unlit;
                success = pass->tryCompile() && success;
            }
        }
        return success;
    };
    if (compile(enabled)) {
        _unlit = enabled;
    } else {
        compile(_unlit);
        CC_LOG_ERROR("[Landscape] failed to switch unlit shader variant");
    }
    // Refresh cached submodel shaders and instance attributes, including pooled
    // models. This is also required while the LOD selection is frozen.
    setWireframe(_wireframe);
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _node != nullptr && _meshes[0] != nullptr &&
           _materialSolid != nullptr && _materialWire != nullptr &&
           _tilePages != nullptr && _tilePages->valid() && _asset != nullptr &&
           _materialLibrary != nullptr && _materialLibrary->valid() &&
           _vtRenderer != nullptr && _vtRenderer->valid();
}

void LandscapeRenderer::destroy() {
    auto removeModel = [this](const IntrusivePtr<scene::Model> &model) {
        if (model == nullptr) {
            return;
        }
        if (_scene != nullptr && model->getScene() == _scene) {
            _scene->removeModel(model);
        }
        model->destroy();
    };

    for (const auto &entry : _active) {
        removeModel(entry.second.model);
    }
    for (auto &pool : _pool) {
        for (const auto &model : pool) removeModel(model);
        pool.clear();
    }
    _active.clear();
    _instanceAttributeScratch = ccstd::monostate{};
    _vtRenderer.reset();

    if (_tilePages != nullptr) {
        _tilePages->destroy();
    }
    _tilePages.reset();
    _asset = nullptr;
    for (auto &mesh : _meshes) mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _materialLibrary.reset();
    _node = nullptr;
    _scene = nullptr;
    _unlit = false;
}

} // namespace landscape
} // namespace cc

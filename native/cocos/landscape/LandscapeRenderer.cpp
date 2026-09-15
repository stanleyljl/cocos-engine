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
    _mesh = GridMesh::create(device);
    _materialSolid = createLandscapeMaterial(false);
    _materialWire = createLandscapeMaterial(true);
    if (_mesh == nullptr || _materialSolid == nullptr || _materialWire == nullptr) {
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
        setRuntimeTexture("vtAlbedo", vt.albedo(), vt.sampler());
        setRuntimeTexture("vtNormalRoughnessAO", vt.normalRoughnessAO(), vt.sampler());
    }
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::setDebugFlags(bool lodColor, bool showRanges) {
    _lodColor = lodColor;
    _showRanges = showRanges;
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
    model->initSubModel(0, _mesh, _wireframe ? _materialWire.get() : _materialSolid.get());
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

void LandscapeRenderer::updateInstanceData(scene::Model *model, const QuadNode &node, uint32_t quadrant) {
    if (model == nullptr) {
        return;
    }

    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, std::min(node.level, _data.maxLevel));
    const float x = static_cast<float>(node.ix) * nodeSize - _data.worldWidth() * 0.5F;
    const float z = static_cast<float>(node.iz) * nodeSize - _data.worldDepth() * 0.5F;
    setInstanceAttribute(model, "a_gridInst", Vec4{x, z, nodeSize, static_cast<float>(node.level)});
    setInstanceAttribute(model, "a_quadrantInst", Vec4{static_cast<float>(quadrant), 0.0F, 0.0F, 0.0F});

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
    const int vtSlot = resolveVTPage(node, Vec4{x, z, nodeSize, 0.0F}, tileParams);
    CC_ASSERTF(vtSlot >= 0, "[Landscape] missing permanent root VT page");
    const auto &vtRegion = _vtRenderer->texture().page(static_cast<uint32_t>(vtSlot)).region;
    // A fallback covers an ancestor's region, not the fine node's region.
    setInstanceAttribute(model, "a_vtInst", Vec4{vtRegion.x, vtRegion.y, vtRegion.z, static_cast<float>(vtSlot)});
}

int LandscapeRenderer::resolveVTPage(const QuadNode &node, const Vec4 &region, const Vec4 &source) {
    auto &vt = _vtRenderer->texture();
    const int slot = vt.acquirePage(makeNodeKey(node), region, source);
    if (slot >= 0 && !vt.page(static_cast<uint32_t>(slot)).dirty) return slot;

    uint32_t x = node.ix;
    uint32_t z = node.iz;
    for (uint32_t level = node.level; level < _data.maxLevel;) {
        ++level;
        x >>= 1U;
        z >>= 1U;
        const int ancestor = vt.findReadyPage(makeNodeKey(level, x, z));
        if (ancestor >= 0) return ancestor;
    }
    // Roots are pinned. On startup/invalidation their writes are guaranteed by
    // VTRenderer::render before the Base Pass; no unrendered fine page is sampled.
    return vt.findPage(makeNodeKey(_data.maxLevel, x, z));
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

void LandscapeRenderer::updateModel(ModelState &state, const QuadNode &node, uint32_t quadrant) {
    auto *model = state.model.get();
    updateInstanceData(model, node, quadrant);
    state.node = node;
    state.quadrant = quadrant;
    model->setEnabled(true);
    updateModelBounds(model, node, quadrant);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const QuadNode &node, uint32_t quadrant) {
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

    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, std::min(node.level, _data.maxLevel));
    const float quadrantSize = nodeSize * 0.5F;
    const float x = static_cast<float>(node.ix) * nodeSize - _data.worldWidth() * 0.5F +
                    static_cast<float>(quadrant & 1U) * quadrantSize;
    const float z = static_cast<float>(node.iz) * nodeSize - _data.worldDepth() * 0.5F +
                    static_cast<float>(quadrant >> 1U) * quadrantSize;

    model->createBoundingShape(Vec3{x, node.minY, z},
                                Vec3{x + quadrantSize, node.maxY, z + quadrantSize});
    model->updateWorldBound();
    model->updateOctree();
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &selected) {
    if (!valid()) {
        return;
    }

    ccstd::unordered_set<uint64_t> visible;
    visible.reserve(selected.size() * 4U);
    _tilePages->beginFrame();
    // Mark all visible pages before uploading staged tiles so LRU eviction never
    // removes a tile needed by the current view.
    for (const auto &node : selected) {
        resolveTilePage(node);
    }
    _tilePages->update(config::PAGE_UPLOAD_BUDGET);
    ccstd::vector<uint64_t> keys;
    keys.reserve(selected.size() * (_data.maxLevel + 1U));
    for (const auto &node : selected) {
        uint32_t x = node.ix;
        uint32_t z = node.iz;
        // Protect every possible fallback before allocation starts, including
        // ancestors that another selected node also updates this frame.
        for (uint32_t level = node.level; level <= _data.maxLevel; ++level) {
            keys.push_back(makeNodeKey(level, x, z));
            x >>= 1U;
            z >>= 1U;
        }
    }
    _vtRenderer->texture().beginFrame(keys);
    for (const auto &node : selected) {
        for (uint32_t quadrant = 0U; quadrant < 4U; ++quadrant) {
            if ((node.quadrantMask & (1U << quadrant)) == 0U) {
                continue;
            }
            const uint64_t key = makeQuadrantKey(node, quadrant);
            visible.insert(key);
            auto iter = _active.find(key);
            if (iter == _active.end()) {
                IntrusivePtr<scene::Model> model;
                if (!_pool.empty()) {
                    model = _pool.back();
                    _pool.pop_back();
                } else {
                    model = createModel();
                }
                if (model == nullptr) {
                    continue;
                }
                auto inserted = _active.emplace(key, ModelState{model, node, quadrant});
                updateModel(inserted.first->second, node, quadrant);
            } else {
                auto &state = iter->second;
                if (state.node.level != node.level ||
                    state.node.ix != node.ix ||
                    state.node.iz != node.iz ||
                    state.node.minY != node.minY ||
                    state.node.maxY != node.maxY ||
                    state.quadrant != quadrant) {
                    updateModel(state, node, quadrant);
                } else {
                    updateInstanceData(state.model, node, quadrant);
                }
            }
        }
    }

    for (auto iter = _active.begin(); iter != _active.end();) {
        if (visible.find(iter->first) == visible.end()) {
            iter->second.model->setEnabled(false);
            _pool.emplace_back(iter->second.model);
            iter = _active.erase(iter);
        } else {
            ++iter;
        }
    }
}

void LandscapeRenderer::setFreezeLod(bool frozen) {
    if (_freezeLod == frozen) {
        return;
    }
    _freezeLod = frozen;
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        updateModelBounds(state.model, state.node, state.quadrant);
    }
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        state.model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        updateInstanceData(state.model, state.node, state.quadrant);
    }
    for (const auto &model : _pool) {
        model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        // Inactive models get fresh instance data when reused. Do not request
        // height/VT pages here: doing so would pin invisible nodes in the caches.
    }
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
    return _scene != nullptr && _node != nullptr && _mesh != nullptr &&
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
    for (const auto &model : _pool) {
        removeModel(model);
    }
    _active.clear();
    _pool.clear();
    _instanceAttributeScratch = ccstd::monostate{};
    _vtRenderer.reset();

    if (_tilePages != nullptr) {
        _tilePages->destroy();
    }
    _tilePages.reset();
    _asset = nullptr;
    _mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _materialLibrary.reset();
    _node = nullptr;
    _scene = nullptr;
    _unlit = false;
}

} // namespace landscape
} // namespace cc

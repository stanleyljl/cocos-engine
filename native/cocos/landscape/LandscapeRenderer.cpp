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
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/scene-graph/Node.h"
#include "core/TypedArray.h"
#include "landscape/GridMesh.h"
#include "landscape/TilePagePool.h"
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
    _asset = asset;
    const auto &data = asset->data();
    _dataDir = asset->dataDir();
    _maxLevel = data.maxLevel;
    _minTileLevel = data.minTileLevel;
    _tileResolution = data.tileResolution;
    _sectorSize = data.sectorSize;
    _worldWidth = data.worldWidth();
    _worldDepth = data.worldDepth();
    _heightScale = data.heightScale;
    _heightBias = data.heightBias;
    const uint32_t tileNodeDivisions = 1U << (_maxLevel - _minTileLevel);
    _heightSampleSpacing = (_sectorSize / static_cast<float>(tileNodeDivisions)) /
                           static_cast<float>(_tileResolution - 1U);
    _heightPages = std::make_unique<TilePagePool>();
    if (!_heightPages->init(device, gfx::Format::RG8, _tileResolution, config::PAGE_POOL_LAYERS)) {
        _heightPages.reset();
        return false;
    }
    _heightPages->setAsset(asset);
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
        _heightScale,
        _heightBias,
        _lodColor ? 1.0F : 0.0F,
        _showRanges ? 1.0F : 0.0F,
    };
    const Vec4 terrainWorld{
        _worldWidth,
        _worldDepth,
        _worldWidth > 0.0F ? 1.0F / _worldWidth : 0.0F,
        _worldDepth > 0.0F ? 1.0F / _worldDepth : 0.0F,
    };
    ccstd::vector<Vec4> lodMorph(config::MAX_LOD_LEVELS);
    for (uint32_t i = 0; i < config::MAX_LOD_LEVELS; ++i) {
        const float start = i < _morphStart.size() ? _morphStart[i] : 0.0F;
        const float end = i < _morphEnd.size() ? _morphEnd[i] : 0.0F;
        lodMorph[i] = Vec4{start, end, 0.0F, 0.0F};
    }

    for (auto *material : {_materialSolid.get(), _materialWire.get()}) {
        material->setPropertyVec4("terrainParams", terrainParams);
        material->setPropertyVec4("terrainWorld", terrainWorld);
        material->setPropertyVec4("heightParams", Vec4{_heightSampleSpacing,
                                                        static_cast<float>(_tileResolution),
                                                        0.0F, 0.0F});
        material->setPropertyVec4Array("lodMorph", lodMorph);
        if (_heightPages != nullptr && _heightPages->array() != nullptr) {
            material->setPropertyGFXTexture("heightmap", _heightPages->array());
            for (const auto &pass : *material->getPasses()) {
                const uint32_t handle = pass->getHandle("heightmap");
                if (handle != 0U) {
                    pass->bindSampler(scene::Pass::getBindingFromHandle(handle), _heightPages->sampler());
                }
            }
        }
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

void LandscapeRenderer::updateInstanceData(scene::Model *model, const QuadNode &node) {
    if (model == nullptr) {
        return;
    }

    const uint32_t shift = _maxLevel - std::min(node.level, _maxLevel);
    const float nodeSize = _sectorSize / static_cast<float>(1U << shift);
    const float x = static_cast<float>(node.ix) * nodeSize - _worldWidth * 0.5F;
    const float z = static_cast<float>(node.iz) * nodeSize - _worldDepth * 0.5F;

    Float32Array instance(4);
    instance[0] = x;
    instance[1] = z;
    instance[2] = nodeSize;
    instance[3] = static_cast<float>(node.level);
    model->setInstancedAttribute("a_gridInst", instance);

    uint32_t sourceLevel = 0U;
    uint32_t sourceX = 0U;
    uint32_t sourceZ = 0U;
    const int layer = resolveHeightPage(node, sourceLevel, sourceX, sourceZ);
    const float sourceNodeSize = _sectorSize / static_cast<float>(1U << (_maxLevel - sourceLevel));
    const Vec4 tileParams{
        static_cast<float>(sourceX) * sourceNodeSize / _worldWidth,
        static_cast<float>(sourceZ) * sourceNodeSize / _worldDepth,
        sourceNodeSize / _worldWidth,
        static_cast<float>(layer < 0 ? TilePagePool::FLAT_LAYER : layer),
    };
    // a_heightInst.xy = source tile origin in global [0,1] terrain UV,
    // z = source tile UV size, w = array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's global UV selects the corresponding subregion of that tile.
    Float32Array heightInstance(4);
    heightInstance[0] = tileParams.x;
    heightInstance[1] = tileParams.y;
    heightInstance[2] = tileParams.z;
    heightInstance[3] = tileParams.w;
    model->setInstancedAttribute("a_heightInst", heightInstance);
}

int LandscapeRenderer::resolveHeightPage(const QuadNode &node, uint32_t &sourceLevel,
                                         uint32_t &sourceX, uint32_t &sourceZ) {
    if (_heightPages == nullptr || _asset == nullptr) {
        return TilePagePool::FLAT_LAYER;
    }
    sourceLevel = std::max(node.level, _minTileLevel);
    const uint32_t shift = sourceLevel > node.level ? sourceLevel - node.level : 0U;
    sourceX = node.ix >> shift;
    sourceZ = node.iz >> shift;
    const uint64_t key = makeNodeKey(sourceLevel, sourceX, sourceZ);
    const ccstd::string path = _dataDir + "/nodes/L" + std::to_string(sourceLevel) +
                               "/h_" + std::to_string(sourceX) + "_" + std::to_string(sourceZ) + ".png";
    int layer = _heightPages->query(key, path);
    if (layer >= 0) {
        return layer;
    }
    for (uint32_t level = sourceLevel + 1U; level <= _maxLevel; ++level) {
        const uint32_t ancestorShift = level - sourceLevel;
        const uint32_t ancestorX = sourceX >> ancestorShift;
        const uint32_t ancestorZ = sourceZ >> ancestorShift;
        layer = _heightPages->peekResident(makeNodeKey(level, ancestorX, ancestorZ));
        if (layer >= 0) {
            sourceLevel = level;
            sourceX = ancestorX;
            sourceZ = ancestorZ;
            return layer;
        }
    }
    return TilePagePool::FLAT_LAYER;
}

void LandscapeRenderer::updateModel(scene::Model *model, const QuadNode &node) {
    if (model == nullptr) {
        return;
    }

    updateInstanceData(model, node);
    _modelNodes[model] = node;
    model->setEnabled(true);
    updateModelBounds(model, node);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const QuadNode &node) {
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

    const uint32_t shift = _maxLevel - std::min(node.level, _maxLevel);
    const float nodeSize = _sectorSize / static_cast<float>(1U << shift);
    const float x = static_cast<float>(node.ix) * nodeSize - _worldWidth * 0.5F;
    const float z = static_cast<float>(node.iz) * nodeSize - _worldDepth * 0.5F;

    model->createBoundingShape(Vec3{x, node.minY, z},
                                Vec3{x + nodeSize, node.maxY, z + nodeSize});
    model->updateWorldBound();
    model->updateOctree();
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &selected) {
    if (!valid()) {
        return;
    }

    ccstd::unordered_map<uint64_t, bool> visible;
    visible.reserve(selected.size());
    _heightPages->beginFrame();
    // Mark all visible pages before uploading staged tiles so LRU eviction never
    // removes a tile needed by the current view.
    for (const auto &node : selected) {
        uint32_t sourceLevel = 0U;
        uint32_t sourceX = 0U;
        uint32_t sourceZ = 0U;
        resolveHeightPage(node, sourceLevel, sourceX, sourceZ);
    }
    _heightPages->update(config::PAGE_UPLOAD_BUDGET);
    for (const auto &node : selected) {
        const uint64_t key = makeNodeKey(node);
        visible[key] = true;
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
            _active.emplace(key, model);
            updateModel(model, node);
        } else {
            const auto state = _modelNodes.find(iter->second.get());
            if (state == _modelNodes.end() ||
                state->second.level != node.level ||
                state->second.ix != node.ix ||
                state->second.iz != node.iz ||
                state->second.minY != node.minY ||
                state->second.maxY != node.maxY) {
                updateModel(iter->second, node);
            } else {
                updateInstanceData(iter->second, node);
            }
        }
    }

    for (auto iter = _active.begin(); iter != _active.end();) {
        if (visible.find(iter->first) == visible.end()) {
            iter->second->setEnabled(false);
            _pool.emplace_back(iter->second);
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
        const auto state = _modelNodes.find(entry.second.get());
        if (state != _modelNodes.end()) {
            updateModelBounds(entry.second, state->second);
        }
    }
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    for (const auto &entry : _active) {
        entry.second->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        const auto state = _modelNodes.find(entry.second.get());
        if (state != _modelNodes.end()) {
            updateInstanceData(entry.second, state->second);
        }
    }
    for (const auto &model : _pool) {
        model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        const auto state = _modelNodes.find(model.get());
        if (state != _modelNodes.end()) {
            updateInstanceData(model, state->second);
        }
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
    return nullptr;
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _node != nullptr && _mesh != nullptr &&
           _materialSolid != nullptr && _materialWire != nullptr &&
           _heightPages != nullptr && _heightPages->valid() && _asset != nullptr;
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
        removeModel(entry.second);
    }
    for (const auto &model : _pool) {
        removeModel(model);
    }
    _active.clear();
    _modelNodes.clear();
    _pool.clear();

    if (_heightPages != nullptr) {
        _heightPages->destroy();
    }
    _heightPages.reset();
    _asset = nullptr;
    _mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _node = nullptr;
    _scene = nullptr;
}

} // namespace landscape
} // namespace cc

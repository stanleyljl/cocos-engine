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
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXBuffer.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/RenderScene.h"

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

IntrusivePtr<RenderingSubMesh> LandscapeRenderer::createGridMesh(gfx::Device *device) const {
    if (device == nullptr) {
        return nullptr;
    }

    // Keep this mesh local to the active CDLOD path. GridMesh is intentionally
    // dormant and must not be pulled into the renderer.
    const int side = config::VERTS_PER_GRID_SIDE;
    const float step = 1.0F / static_cast<float>(side - 1);
    ccstd::vector<float> vertices;
    vertices.reserve(static_cast<size_t>(side) * side * 3U);
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            vertices.emplace_back(static_cast<float>(x) * step);
            vertices.emplace_back(0.0F);
            vertices.emplace_back(static_cast<float>(z) * step);
        }
    }

    ccstd::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(side - 1) * (side - 1) * 6U);
    for (int z = 0; z < side - 1; ++z) {
        for (int x = 0; x < side - 1; ++x) {
            const auto a = static_cast<uint16_t>(z * side + x);
            const auto b = static_cast<uint16_t>(a + 1U);
            const auto c = static_cast<uint16_t>((z + 1) * side + x);
            const auto d = static_cast<uint16_t>(c + 1U);
            indices.emplace_back(a);
            indices.emplace_back(c);
            indices.emplace_back(b);
            indices.emplace_back(b);
            indices.emplace_back(c);
            indices.emplace_back(d);
        }
    }

    auto *vertexBuffer = device->createBuffer({
        gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
        gfx::MemoryUsageBit::DEVICE,
        static_cast<uint32_t>(vertices.size() * sizeof(float)),
        static_cast<uint32_t>(3U * sizeof(float)),
    });
    auto *indexBuffer = device->createBuffer({
        gfx::BufferUsageBit::INDEX | gfx::BufferUsageBit::TRANSFER_DST,
        gfx::MemoryUsageBit::DEVICE,
        static_cast<uint32_t>(indices.size() * sizeof(uint16_t)),
        static_cast<uint32_t>(sizeof(uint16_t)),
    });
    if (vertexBuffer == nullptr || indexBuffer == nullptr) {
        CC_SAFE_RELEASE(vertexBuffer);
        CC_SAFE_RELEASE(indexBuffer);
        return nullptr;
    }

    vertexBuffer->update(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(float)));
    indexBuffer->update(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));

    gfx::BufferList vertexBuffers;
    vertexBuffers.emplace_back(vertexBuffer);
    ccstd::vector<gfx::Attribute> attributes{
        gfx::Attribute{gfx::ATTR_NAME_POSITION, gfx::Format::RGB32F},
    };
    auto *mesh = ccnew RenderingSubMesh(vertexBuffers, attributes,
                                        gfx::PrimitiveMode::TRIANGLE_LIST, indexBuffer);
    mesh->setSubMeshIdx(0);
    return mesh;
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
    _mesh = createGridMesh(device);
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

void LandscapeRenderer::setWorld(uint32_t sectorsX, uint32_t sectorsZ,
                                 float sectorSize, uint32_t maxLevel) {
    if (sectorsX == 0U || sectorsZ == 0U || sectorSize <= 0.0F ||
        maxLevel >= config::MAX_LOD_LEVELS) {
        return;
    }
    _sectorsX = sectorsX;
    _sectorsZ = sectorsZ;
    _sectorSize = sectorSize;
    _maxLevel = maxLevel;
    _worldWidth = sectorSize * static_cast<float>(sectorsX);
    _worldDepth = sectorSize * static_cast<float>(sectorsZ);
    updateMaterialProperties();
}

void LandscapeRenderer::setHeightRange(float scale, float bias) {
    if (scale <= 0.0F) {
        return;
    }
    _heightScale = scale;
    _heightBias = bias;
    updateMaterialProperties();
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

void LandscapeRenderer::setProceduralHeightmap(const ccstd::vector<float> &heightmap,
                                               uint32_t size) {
    if (heightmap.empty() || size == 0U || heightmap.size() != static_cast<size_t>(size) * size) {
        return;
    }
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (device == nullptr) {
        return;
    }

    auto *texture = device->createTexture({
        gfx::TextureType::TEX2D,
        gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST,
        gfx::Format::R32F,
        size,
        size,
    });
    if (texture == nullptr) {
        CC_LOG_WARNING("[Landscape] failed to create the R32F heightmap texture");
        return;
    }

    const auto *data = reinterpret_cast<const uint8_t *>(heightmap.data());
    gfx::BufferDataList buffers{data};
    gfx::BufferTextureCopyList regions{{0U,
                                        0U,
                                        0U,
                                        {0, 0, 0},
                                        {size, size, 1U},
                                        {0U, 0U, 1U}}};
    device->copyBuffersToTexture(buffers, texture, regions);
    _heightmap = texture;
    updateMaterialProperties();
}

bool LandscapeRenderer::hasProceduralHeightmap() const {
    return _heightmap != nullptr;
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
        material->setPropertyVec4Array("lodMorph", lodMorph);
        if (_heightmap != nullptr) {
            material->setPropertyGFXTexture("heightmap", _heightmap);
        }
    }
    updateMorphCameraProperty();
}

void LandscapeRenderer::updateMorphCameraProperty() {
    if (_materialSolid == nullptr || _materialWire == nullptr) {
        return;
    }

    const Vec4 cameraPosition{
        _morphCameraPosition.x,
        _morphCameraPosition.y,
        _morphCameraPosition.z,
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
}

void LandscapeRenderer::updateModel(scene::Model *model, const QuadNode &node) {
    if (model == nullptr) {
        return;
    }

    const uint32_t shift = _maxLevel - std::min(node.level, _maxLevel);
    const float nodeSize = _sectorSize / static_cast<float>(1U << shift);
    const float x = static_cast<float>(node.ix) * nodeSize - _worldWidth * 0.5F;
    const float z = static_cast<float>(node.iz) * nodeSize - _worldDepth * 0.5F;

    updateInstanceData(model, node);
    _modelNodes[model] = node;
    model->createBoundingShape(Vec3{x, node.minY, z},
                                Vec3{x + nodeSize, node.maxY, z + nodeSize});
    model->setEnabled(true);
    model->updateWorldBound();
    model->updateOctree();
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &selected) {
    if (!valid()) {
        return;
    }

    ccstd::unordered_map<uint64_t, bool> visible;
    visible.reserve(selected.size());
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

void LandscapeRenderer::setMorphCameraPosition(const Vec3 &position) {
    if (_morphCameraPosition.x == position.x &&
        _morphCameraPosition.y == position.y &&
        _morphCameraPosition.z == position.z) {
        return;
    }
    _morphCameraPosition = position;
    updateMorphCameraProperty();
}

RenderTexture *LandscapeRenderer::debugAtlas() const {
    return nullptr;
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _node != nullptr && _mesh != nullptr &&
           _materialSolid != nullptr && _materialWire != nullptr;
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

    if (_heightmap != nullptr) {
        _heightmap->destroy();
    }
    _heightmap = nullptr;
    _mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _node = nullptr;
    _scene = nullptr;
}

} // namespace landscape
} // namespace cc

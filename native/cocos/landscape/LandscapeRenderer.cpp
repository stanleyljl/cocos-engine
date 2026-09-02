/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

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

#include <cstdint>

#include "base/Log.h"
#include "base/std/container/vector.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/scene-graph/Node.h"
#include "math/Vec3.h"
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXBuffer.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "scene/Model.h"
#include "scene/RenderScene.h"

namespace cc {
namespace landscape {

namespace {
Material *createLandscapeMaterial(bool wireframe) {
    auto *material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-unlit"};
    if (wireframe) {
        RasterizerStateInfo rasterizer;
        rasterizer.polygonMode = gfx::PolygonMode::LINE;
        rasterizer.cullMode = gfx::CullMode::NONE;
        PassOverrides overrides;
        overrides.rasterizerState = rasterizer;
        info.states = IMaterialInfo::PassOverridesType{overrides};
    }
    material->initialize(info);
    material->setProperty("mainColor", Vec4{0.30F, 0.52F, 0.30F, 1.0F});
    return material;
}
} // namespace

LandscapeRenderer::LandscapeRenderer() = default;

LandscapeRenderer::~LandscapeRenderer() {
    destroy();
}

IntrusivePtr<RenderingSubMesh> LandscapeRenderer::createPlaneMesh(gfx::Device *device, float width, float depth) const {
    if (device == nullptr || width <= 0.0F || depth <= 0.0F) {
        return nullptr;
    }

    const float halfWidth = width * 0.5F;
    const float halfDepth = depth * 0.5F;
    const ccstd::vector<float> vertices{
        -halfWidth, 0.0F, -halfDepth,
        halfWidth, 0.0F, -halfDepth,
        -halfWidth, 0.0F, halfDepth,
        halfWidth, 0.0F, halfDepth,
    };
    const ccstd::vector<uint16_t> indices{0, 2, 1, 1, 2, 3};

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
    const gfx::AttributeList attributes{gfx::Attribute{gfx::ATTR_NAME_POSITION, gfx::Format::RGB32F}};
    return ccnew RenderingSubMesh(vertexBuffers, attributes, gfx::PrimitiveMode::TRIANGLE_LIST, indexBuffer);
}

bool LandscapeRenderer::init(Node *node, scene::RenderScene *scene) {
    auto *root = Root::getInstance();
    auto *device = root ? root->getDevice() : nullptr;
    if (node == nullptr || scene == nullptr || device == nullptr) {
        return false;
    }

    _node = node;
    _scene = scene;
    _mesh = createPlaneMesh(device, _width, _depth);
    _materialSolid = createLandscapeMaterial(false);
    _materialWire = createLandscapeMaterial(true);
    if (_mesh == nullptr || _materialSolid == nullptr || _materialWire == nullptr) {
        destroy();
        return false;
    }
    const auto passes = _materialSolid->getPasses();
    if (passes == nullptr || passes->empty()) {
        CC_LOG_WARNING("[Landscape] builtin-unlit material has no passes");
        destroy();
        return false;
    }

    auto *model = root->createModel<scene::Model>();
    if (model == nullptr) {
        destroy();
        return false;
    }
    _model = model;
    _model->setNode(_node);
    _model->setTransform(_node);
    _model->initSubModel(0, _mesh, _materialSolid);
    _model->createBoundingShape(Vec3{-_width * 0.5F, 0.0F, -_depth * 0.5F},
                                Vec3{_width * 0.5F, 0.0F, _depth * 0.5F});
    _model->updateWorldBound();
    _scene->addModel(_model);
    return true;
}

void LandscapeRenderer::setWorldSize(float width, float depth) {
    if (width <= 0.0F || depth <= 0.0F) {
        return;
    }
    _width = width;
    _depth = depth;
    if (_model == nullptr) {
        return;
    }
    auto *root = Root::getInstance();
    auto *device = root ? root->getDevice() : nullptr;
    auto mesh = createPlaneMesh(device, _width, _depth);
    if (mesh == nullptr) {
        return;
    }
    _mesh = mesh;
    _model->setSubModelMesh(0, _mesh);
    _model->createBoundingShape(Vec3{-_width * 0.5F, 0.0F, -_depth * 0.5F},
                                Vec3{_width * 0.5F, 0.0F, _depth * 0.5F});
    _model->updateWorldBound();
    _model->updateOctree();
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    if (_model != nullptr) {
        _model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
    }
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _model != nullptr && _mesh != nullptr && _materialSolid != nullptr;
}

void LandscapeRenderer::destroy() {
    if (_scene != nullptr && _model != nullptr) {
        _scene->removeModel(_model);
    }
    _model = nullptr;
    _mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _node = nullptr;
    _scene = nullptr;
}

} // namespace landscape
} // namespace cc

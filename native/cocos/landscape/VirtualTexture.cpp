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

#include "landscape/VirtualTexture.h"

#include "base/Log.h"
#include "core/Root.h"
#include "core/TypedArray.h"
#include "core/assets/Material.h"
#include "core/assets/RenderTexture.h"
#include "core/scene-graph/Node.h"
#include "landscape/LandscapeConfig.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "scene/Camera.h"
#include "scene/Model.h"
#include "scene/RenderScene.h"
#include "scene/SubModel.h"

namespace cc {
namespace landscape {

namespace {
constexpr uint64_t INVALID_KEY = ~static_cast<uint64_t>(0);
} // namespace

VirtualTexture::VirtualTexture() = default;

VirtualTexture::~VirtualTexture() {
    destroy();
}

bool VirtualTexture::init(scene::RenderScene *scene, RenderingSubMesh *quadMesh) {
    auto *root = Root::getInstance();
    if (root == nullptr || scene == nullptr || quadMesh == nullptr) {
        return false;
    }
    _scene = scene;

    // 1) Physical atlas as a RenderTexture (so a 2D Sprite can display it).
    _atlas = ccnew RenderTexture();
    IRenderTextureCreateInfo rtInfo;
    rtInfo.name = "landscape-vt-atlas";
    rtInfo.width = config::VT_ATLAS_SIZE;
    rtInfo.height = config::VT_ATLAS_SIZE;
    _atlas->initialize(rtInfo);

    // 2) Compose material (instanced: one quad per page slot).
    _material = ccnew Material();
    IMaterialInfo mi;
    mi.effectName = ccstd::string{"builtin-landscape-vt-compose"};
    MacroRecord defines;
    defines["USE_INSTANCING"] = true;
    mi.defines = IMaterialInfo::DefinesType{defines};
    _material->initialize(mi);
    auto passes = _material->getPasses();
    if (!passes || passes->empty()) {
        CC_LOG_WARNING("[Landscape] VT compose material has no passes (effect not imported?), VT disabled");
        destroy();
        return false;
    }

    // 3) One compose model with VT_PAGE_COUNT instanced page quads, on the
    //    dedicated VT layer (rendered only by the compose camera).
    _quadNode = ccnew Node("landscape-vt-compose-quad");
    _quadNode->setLayer(config::VT_COMPOSE_LAYER);
    _model = root->createModel<scene::Model>();
    _model->setNode(_quadNode);
    _model->setTransform(_quadNode);
    for (uint32_t i = 0; i < config::VT_PAGE_COUNT; ++i) {
        _model->initSubModel(static_cast<index_t>(i), quadMesh, _material);
    }
    _model->setEnabled(true);
    _scene->addModel(_model);

    // 4) Orthographic compose camera targeting the atlas, seeing only the VT layer.
    _cameraNode = ccnew Node("landscape-vt-compose-camera");
    _camera = root->createCamera();
    scene::ICameraInfo ci;
    ci.name = "landscape-vt-compose";
    ci.node = _cameraNode;
    ci.projection = scene::CameraProjection::ORTHO;
    ci.window = _atlas->getWindow();
    ci.priority = 0;
    ci.cameraType = scene::CameraType::DEFAULT;
    ci.trackingType = scene::TrackingType::NO_TRACKING;
    ci.usage = scene::CameraUsage::GAME;
    _camera->initialize(ci);
    _camera->setVisibility(config::VT_COMPOSE_LAYER);
    _camera->setClearFlag(gfx::ClearFlagBit::COLOR | gfx::ClearFlagBit::DEPTH);
    _camera->setClearColor(gfx::Color{0.0F, 0.0F, 0.0F, 1.0F});
    _camera->setClearDepth(1.0F);
    _camera->setNearClip(0.0F);
    _camera->setFarClip(1.0F);
    _camera->setOrthoHeight(1.0F);
    _scene->addCamera(_camera);
    _camera->attachToScene(_scene);

    // 5) Page-slot bookkeeping (stable per-node slots).
    _slotKey.assign(config::VT_PAGE_COUNT, INVALID_KEY);
    _slotParams.assign(config::VT_PAGE_COUNT, PageParams{});
    _keyToSlot.clear();
    _inUse.clear();
    _warnedFull = false;
    commit(); // all slots inactive (degenerate) until the first frame assigns pages

    CC_LOG_INFO("[Landscape] VT atlas %ux%u, %u pages (%ux%u each)", config::VT_ATLAS_SIZE, config::VT_ATLAS_SIZE,
                config::VT_PAGE_COUNT, config::VT_PAGE_RES, config::VT_PAGE_RES);
    return true;
}

void VirtualTexture::beginFrame() {
    _inUse.clear();
}

int VirtualTexture::acquireSlot() {
    // Lowest free slot (keeps in-use pages packed toward the top of the atlas).
    for (uint32_t slot = 0; slot < config::VT_PAGE_COUNT; ++slot) {
        if (_slotKey[slot] == INVALID_KEY) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

int VirtualTexture::acquirePage(uint64_t key, int splatLayer, float uvScale, float uvOffX, float uvOffZ, float nodeWorldSize) {
    if (_atlas == nullptr) {
        return -1;
    }
    _inUse.insert(key);
    const PageParams params{splatLayer, uvScale, uvOffX, uvOffZ, nodeWorldSize};

    // Keep the node's existing slot if it already has one (stable -> no flicker).
    const auto it = _keyToSlot.find(key);
    int slot = (it != _keyToSlot.end()) ? it->second : acquireSlot();
    if (slot < 0) {
        if (!_warnedFull) {
            _warnedFull = true;
            CC_LOG_WARNING("[Landscape] VT page pool full (%u pages); extra tiles skip compose. Increase VT_ATLAS_SIZE/VT_PAGE_RES.", config::VT_PAGE_COUNT);
        }
        return -1;
    }
    _slotKey[slot] = key;
    _keyToSlot[key] = slot;
    _slotParams[slot] = params;
    return slot;
}

void VirtualTexture::commit() {
    if (_model == nullptr) {
        return;
    }
    const auto &subModels = _model->getSubModels();
    for (uint32_t slot = 0; slot < config::VT_PAGE_COUNT; ++slot) {
        if (slot >= subModels.size()) {
            break;
        }
        // Free slots whose node was not requested this frame (it left the view).
        const uint64_t key = _slotKey[slot];
        if (key != INVALID_KEY && _inUse.find(key) == _inUse.end()) {
            _keyToSlot.erase(key);
            _slotKey[slot] = INVALID_KEY;
        }
        const bool active = _slotKey[slot] != INVALID_KEY;
        const PageParams &p = _slotParams[slot];

        Float32Array inst(4);
        inst[0] = static_cast<float>(slot);
        inst[1] = static_cast<float>(p.splatLayer);
        inst[2] = p.uvScale;
        inst[3] = active ? 1.0F : 0.0F;
        subModels[slot]->setInstancedAttribute("a_composeInst", inst);

        Float32Array inst2(4);
        inst2[0] = p.uvOffX;
        inst2[1] = p.uvOffZ;
        inst2[2] = p.nodeWorldSize;
        inst2[3] = 0.0F;
        subModels[slot]->setInstancedAttribute("a_composeInst2", inst2);
    }
}

void VirtualTexture::destroy() {
    if (_scene != nullptr) {
        if (_model) {
            _scene->removeModel(_model);
        }
        if (_camera) {
            _scene->removeCamera(_camera);
        }
    }
    if (_camera) {
        _camera->detachFromScene();
        _camera->destroy();
        _camera = nullptr;
    }
    _model = nullptr;
    _quadNode = nullptr;
    _cameraNode = nullptr;
    _material = nullptr;
    if (_atlas) {
        _atlas->destroy();
        _atlas = nullptr;
    }
    _slotKey.clear();
    _keyToSlot.clear();
    _inUse.clear();
    _slotParams.clear();
    _scene = nullptr;
}

} // namespace landscape
} // namespace cc

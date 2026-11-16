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

#include "landscape/LandscapeInstance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "base/Log.h"
#include "base/std/container/unordered_set.h"
#include "core/Root.h"
#include "core/assets/Texture2D.h"
#include "core/geometry/AABB.h"
#include "core/geometry/Intersect.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "landscape/Asset.h"
#include "landscape/Config.h"
#include "landscape/Renderer.h"
#include "landscape/Schedule.h"
#include "landscape/Query.h"
#include "math/Mat4.h"
#include "math/Vec3.h"
#include "renderer/pipeline/GeometryRenderer.h"
#include "renderer/pipeline/PipelineSceneData.h"
#include "renderer/pipeline/shadow/CSMLayers.h"
#include "scene/Camera.h"
#include "scene/DirectionalLight.h"
#include "scene/RenderScene.h"
#include "scene/Shadow.h"
#include "scene/SpotLight.h"

namespace cc {
namespace landscape {
LandscapeInstance::LandscapeInstance() = default;

LandscapeInstance::~LandscapeInstance() {
    destroy();
}

bool LandscapeInstance::onEnable(Node *node, float lodQualityScale, uint32_t queryCacheCapacity) {
    if (node == nullptr || _renderer != nullptr || lodQualityScale <= 0.0F ||
        queryCacheCapacity == 0 || queryCacheCapacity > 4096) {
        return false;
    }
    if (!isTranslationOnly(node)) {
        CC_LOG_ERROR("[Landscape] Only translation is supported; initialization failed");
        return false;
    }
    _lodQualityScale = lodQualityScale;
    _node = node;
    initializeRuntime(queryCacheCapacity);
    return isInitialized();
}

void LandscapeInstance::initializeRuntime(uint32_t queryCacheCapacity) {
    if (_node == nullptr || _renderer != nullptr) {
        return;
    }
    auto *ccScene = _node->getScene();
    _scene = ccScene ? ccScene->getRenderScene() : nullptr;
    if (_scene == nullptr) {
        return;
    }

    if (_assetPath.empty()) {
        return;
    }
    // Each enable creates fresh objects and initializes them once. Failed
    // initialization releases these locals; retrying creates a new instance.
    IntrusivePtr<LandscapeAsset> asset = ccnew LandscapeAsset();
    if (!asset->load(_assetPath)) {
        return;
    }
    auto renderer = std::make_unique<LandscapeRenderer>();
    if (!renderer->init(_node.get(), _scene, asset.get())) {
        return;
    }
    auto schedule = std::make_unique<LandscapeSchedule>(*asset, *renderer, _lodQualityScale);
    // Create the empty CPU cache with the other runtime objects. Tile I/O
    // starts when addSource() registers the first reference to a tile.
    auto query = std::make_unique<LandscapeQuery>(asset->data(),
        [this](uint32_t x, uint32_t z, LandscapeQueryCompletion completion) {
            _asset->requestQueryTile(x, z, std::move(completion));
        }, queryCacheCapacity);
    applyRendererSettings(*renderer);

    _asset = std::move(asset);
    _schedule = std::move(schedule);
    _renderer = std::move(renderer);
    _query = std::move(query);
    _scene->addLandscape(this);
}

void LandscapeInstance::applyRendererSettings(LandscapeRenderer &renderer) const {
    renderer.setDebugData(_debugData);
    renderer.setHoleMode(_holeMode);
    renderer.setGlobalColorMap(_globalColorMap);
    renderer.setGlobalColorStrength(_globalColorStrength);
    renderer.setCastShadow(_castShadow);
    renderer.setReceiveShadow(_receiveShadow);
}

void LandscapeInstance::setGlobalColorMap(Texture2D *texture) {
    _globalColorMap = texture;
    if (_renderer) {
        _renderer->setGlobalColorMap(texture);
    }
}

void LandscapeInstance::setGlobalColorStrength(float strength) {
    _globalColorStrength = std::clamp(strength, 0.0F, 1.0F);
    if (_renderer) {
        _renderer->setGlobalColorStrength(_globalColorStrength);
    }
}

void LandscapeInstance::onDisable() {
    if (_scene) {
        _scene->removeLandscape(this);
    }
    _passes.clear();
    _passCount = 0;
    _schedule = nullptr;
    _renderer = nullptr;
    _query = nullptr;
    _asset = nullptr;
    _scene = nullptr;
    _node = nullptr;
}

void LandscapeInstance::destroy() {
    onDisable();
    _querySources.clear();
}

uint32_t LandscapeInstance::addDecal(float x, float z, float size, uint32_t layer) {
    const uint32_t id = _renderer ? _renderer->addDecal(x, z, size, layer) : 0;
    if (id == 0) {
        CC_LOG_WARNING("[Landscape] Cannot add decal: initialize terrain, use a valid preloaded layer and region, and keep at most %u instances.",
                       config::DECAL_INSTANCE_MAX);
    }
    return id;
}

bool LandscapeInstance::removeDecal(uint32_t id) {
    return _renderer && _renderer->removeDecal(id);
}

void LandscapeInstance::update() {
    if (!_renderer) {
        return;
    }
    for (auto source = _querySources.begin(); source != _querySources.end();) {
        const uint32_t id = source->first;
        ++source;
        getQuerySourceStatus(id);
    }
    _query->update();
    auto *camera = pickMainCamera();
    if (!camera) {
        return;
    }
    camera->update();
    _schedule->update(camera->getPosition(), _node->getWorldPosition());
}

void LandscapeInstance::addPass(const geometry::Frustum &frustum, bool shadow) {
    if (_passCount == _passes.size()) {
        _passes.emplace_back();
    }
    auto &pass = _passes[_passCount++];
    pass.frustum = &frustum;
    pass.shadow = shadow;
    pass.models.clear();
    _schedule->addPass(frustum, !shadow);
}

bool LandscapeInstance::isTranslationOnly(Node *node) {
    const auto &m = node->getWorldMatrix();
    for (uint32_t i = 0; i < 12; ++i) {
        const float expected = (i == 0 || i == 5 || i == 10) ? 1.0F : 0.0F;
        if (std::abs(m.m[i] - expected) > 1e-5F) {
            return false;
        }
    }
    return true;
}

uint32_t LandscapeInstance::addQuerySource(Node *node, float radius) {
    if (!isObjectValid(node, true) || radius < 0.0F || _nextQuerySource > UINT32_MAX) {
        return 0;
    }
    const auto id = static_cast<uint32_t>(_nextQuerySource++);
    _querySources.emplace(id, QuerySource{IntrusivePtr<Node>(node), radius});
    return id;
}

bool LandscapeInstance::setQuerySourceRadius(uint32_t id, float radius) {
    const auto source = _querySources.find(id);
    if (source == _querySources.end() || radius < 0.0F) {
        return false;
    }
    source->second.radius = radius;
    return true;
}

void LandscapeInstance::removeQuerySource(uint32_t id) {
    _querySources.erase(id);
    if (_query) {
        _query->removeSource(id);
    }
}

uint32_t LandscapeInstance::getQuerySourceStatus(uint32_t id) {
    const auto source = _querySources.find(id);
    if (source == _querySources.end()) {
        return static_cast<uint32_t>(LandscapeQueryStatus::MISS);
    }
    auto *node = source->second.node.get();
    if (!isObjectValid(node, true)) {
        removeQuerySource(id);
        return static_cast<uint32_t>(LandscapeQueryStatus::MISS);
    }
    if (!_renderer || !node->isActiveInHierarchy()) {
        if (_query) {
            _query->removeSource(id);
        }
        return static_cast<uint32_t>(LandscapeQueryStatus::NOT_READY);
    }
    if (!isTranslationOnly(_node.get())) {
        _query->removeSource(id);
        return static_cast<uint32_t>(LandscapeQueryStatus::ERROR);
    }
    const auto &position = node->getWorldPosition();
    const auto &origin = _node->getWorldPosition();
    return static_cast<uint32_t>(_query->addSource(id, {position.x - origin.x, position.z - origin.z}, source->second.radius));
}

uint32_t LandscapeInstance::sampleSurface(float worldX, float worldZ, Float32Array output) {
    if (output.length() < 8) {
        return static_cast<uint32_t>(LandscapeQueryStatus::ERROR);
    }
    if (!_asset || !_node) {
        return static_cast<uint32_t>(LandscapeQueryStatus::NOT_READY);
    }
    if (!isTranslationOnly(_node.get())) {
        return static_cast<uint32_t>(LandscapeQueryStatus::ERROR);
    }
    const auto &origin = _node->getWorldPosition();
    const auto &data = _asset->data();
    const LandscapePoint local{worldX - origin.x, worldZ - origin.z};
    if (!data.contains(local)) {
        return static_cast<uint32_t>(LandscapeQueryStatus::MISS);
    }
    if (!_query) {
        return static_cast<uint32_t>(LandscapeQueryStatus::NOT_READY);
    }
    const auto result = _query->sample(local);
    if (result.status == LandscapeQueryStatus::HIT) {
        output[0] = worldX; output[1] = result.position.y + origin.y; output[2] = worldZ;
        output[3] = result.normal.x; output[4] = result.normal.y; output[5] = result.normal.z;
        output[6] = static_cast<float>(result.surfaceType); output[7] = result.surfaceWeight;
    }
    return static_cast<uint32_t>(result.status);
}

void LandscapeInstance::preparePasses(const scene::Camera &camera, const pipeline::PipelineSceneData &sceneData) {
    _passCount = 0;
    if (!_renderer) {
        return;
    }
    const auto layer = _node->getLayer();
    // UI/other cameras that hide terrain must not prepare its rendering resources.
    if ((camera.getVisibility() & layer) != layer) {
        return;
    }
    _schedule->prepareRenderData(Root::getInstance()->getFrameCount());
    _schedule->beginPasses();
    addPass(camera.getFrustum(), false);
    const uint32_t cascadesToDeduplicate = addShadowPasses(sceneData);
    collectPassModels(cascadesToDeduplicate);
}

uint32_t LandscapeInstance::addShadowPasses(const pipeline::PipelineSceneData &sceneData) {
    uint32_t cascadesToDeduplicate = 0;
    const auto *shadows = sceneData.getShadows();
    if (_castShadow && shadows && shadows->isEnabled() && shadows->getType() == scene::ShadowType::SHADOW_MAP) {
        const auto *mainLight = _scene->getMainLight();
        const auto *csmLayers = sceneData.getCSMLayers();
        if (mainLight && mainLight->isShadowEnabled()) {
            if (mainLight->isShadowFixedArea()) {
                addPass(csmLayers->getSpecialLayer()->getValidFrustum(), true);
            } else {
                const uint32_t count = sceneData.getCSMSupported() ? static_cast<uint32_t>(mainLight->getCSMLevel()) : 1U;
                if (mainLight->getCSMOptimizationMode() == scene::CSMOptimizationMode::REMOVE_DUPLICATES) {
                    cascadesToDeduplicate = count;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    addPass(csmLayers->getLayers()[i]->getValidFrustum(), true);
                }
            }
        }
        for (const auto *light : sceneData.getValidPunctualLights()) {
            if (light->getType() != scene::LightType::SPOT) {
                continue;
            }
            const auto *spot = static_cast<const scene::SpotLight *>(light);
            if (spot->isShadowEnabled()) {
                addPass(spot->getFrustum(), true);
            }
        }
    }
    return cascadesToDeduplicate;
}

void LandscapeInstance::collectPassModels(uint32_t cascadesToDeduplicate) {
    for (size_t i = 0; i < _passCount; ++i) {
        auto &pass = _passes[i];
        _schedule->collectPassModels(*pass.frustum, pass.shadow, pass.models);
    }
    removeCSMDuplicates(cascadesToDeduplicate);
    _schedule->endPasses();
}

void LandscapeInstance::removeCSMDuplicates(uint32_t cascadeCount) {
    if (cascadeCount < 2U) {
        return;
    }
    ccstd::unordered_set<const scene::Model *> coveredShadowModels;
    // Match legacy shadowCulling: a model completely inside an earlier cascade
    // is omitted from later cascades. Intersecting the frustum is not sufficient.
    // Each pass has independently culled the shared committed Quadtree.
    // Directional cascades follow the color pass; spot passes are never deduplicated.
    for (uint32_t i = 1; i <= cascadeCount; ++i) {
        auto &pass = _passes[i];
        size_t count = 0;
        for (const auto *model : pass.models) {
            if (coveredShadowModels.count(model) != 0) {
                continue;
            }
            pass.models[count++] = model;
            if (i < cascadeCount && geometry::aabbFrustumCompletelyInside(*model->getWorldBounds(), *pass.frustum)) {
                coveredShadowModels.insert(model);
            }
        }
        pass.models.resize(count);
    }
}

const ccstd::vector<const scene::Model *> &LandscapeInstance::getPassModels(const geometry::Frustum &frustum, bool shadow) const {
    for (size_t i = 0; i < _passCount; ++i) {
        const auto &pass = _passes[i];
        if (pass.frustum == &frustum && pass.shadow == shadow) {
            return pass.models;
        }
    }
    static const ccstd::vector<const scene::Model *> empty;
    return empty;
}

void LandscapeInstance::onGlobalPipelineStateChanged() {
    if (_renderer) {
        _renderer->onGlobalPipelineStateChanged();
    }
}

bool LandscapeInstance::isReady() const {
    return _schedule != nullptr && _schedule->isReady();
}


void LandscapeInstance::setHoleMode(uint32_t mode) {
    _holeMode = mode == 0U ? 0U : 1U;
    if (_renderer) {
        _renderer->setHoleMode(_holeMode);
    }
}

void LandscapeInstance::setDebugData(const LandscapeDebugData &data) {
    _debugData = data;
    if (_renderer) {
        _renderer->setDebugData(data);
    }
}

void LandscapeInstance::setCastShadow(bool enabled) {
    _castShadow = enabled;
    if (_renderer) {
        _renderer->setCastShadow(enabled);
    }
}

void LandscapeInstance::setReceiveShadow(bool enabled) {
    _receiveShadow = enabled;
    if (_renderer) {
        _renderer->setReceiveShadow(enabled);
    }
}

void LandscapeInstance::setAssetPath(const ccstd::string &manifestPath) {
    _assetPath = manifestPath;
}

scene::Camera *LandscapeInstance::pickMainCamera() const {
    if (_scene == nullptr) {
        return nullptr;
    }
    for (const auto &camera : _scene->getCameras()) {
        if (camera && camera->getProjectionType() == scene::CameraProjection::PERSPECTIVE) {
            return camera.get();
        }
    }
    return nullptr;
}

void LandscapeInstance::drawDebugBounds() {
#if CC_USE_GEOMETRY_RENDERER
    if (_scene == nullptr || _node == nullptr || !_schedule) {
        return;
    }
    auto *camera = pickMainCamera();
    if (camera == nullptr) {
        return;
    }
    // Debug bounds follow the same committed coverage as actual rendering.
    camera->initGeometryRenderer();
    auto *geometry = camera->getGeometryRenderer();
    if (geometry == nullptr) {
        return;
    }

    static const gfx::Color colors[] = {
        {1.0F, 0.15F, 0.15F, 1.0F}, {0.15F, 0.90F, 0.20F, 1.0F},
        {0.20F, 0.45F, 1.00F, 1.0F}, {1.0F, 0.90F, 0.10F, 1.0F},
        {1.0F, 0.15F, 0.90F, 1.0F}, {0.10F, 0.90F, 0.95F, 1.0F},
        {0.65F, 0.35F, 0.85F, 1.0F}, {0.95F, 0.55F, 0.20F, 1.0F},
        {0.90F, 0.90F, 0.90F, 1.0F},
    };
    static_assert(sizeof(colors) / sizeof(colors[0]) == config::MAX_LOD_LEVELS,
                  "Provide a debug color for every supported LOD level");
    const Mat4 &world = _node->getWorldMatrix();
    _schedule->visitDebugNodes(camera->getFrustum(), _node->getWorldPosition(), [&](const QuadNode &node, const LandscapeRegion &region) {
        const float quadrantSize = region.size * 0.5F;
        const float nodeX = region.x;
        const float nodeZ = region.z;
        for (uint32_t quadrant = 0U; quadrant < 4U; ++quadrant) {
            const float x = nodeX + static_cast<float>(quadrant & 1U) * quadrantSize;
            const float z = nodeZ + static_cast<float>(quadrant >> 1U) * quadrantSize;
            geometry::AABB box{
                x + quadrantSize * 0.5F,
                (node.minY + node.maxY) * 0.5F,
                z + quadrantSize * 0.5F,
                quadrantSize * 0.5F,
                (node.maxY - node.minY) * 0.5F,
                quadrantSize * 0.5F,
            };
            // Debug bounds must remain visible even where the terrain occludes or
            // shares an edge with the box.
            geometry->addBoundingBox(box, colors[node.level], true, false, false, true, world);
        }
    });
#endif
}

TextureBase *LandscapeInstance::getVTAtlas() const {
    return _renderer ? _renderer->getVTAtlas() : nullptr;
}

} // namespace landscape
} // namespace cc

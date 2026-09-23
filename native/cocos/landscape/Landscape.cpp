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

#include "landscape/Landscape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "base/Log.h"
#include "core/Root.h"
#include "core/assets/Texture2D.h"
#include "core/geometry/AABB.h"
#include "core/geometry/Intersect.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/LandscapeRenderer.h"
#include "landscape/Quadtree.h"
#include "landscape/LandscapeQuery.h"
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

Landscape::Landscape() = default;

Landscape::~Landscape() {
    onDisable();
}

void Landscape::onEnable(Node *node, float lodQualityScale) {
    if (node == nullptr || _renderer != nullptr ||
        !std::isfinite(lodQualityScale) || lodQualityScale <= 0.0F) {
        return;
    }
    _lodQualityScale = lodQualityScale;
    _node = node;
    initializeRenderer();
}

void Landscape::initializeRenderer() {
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
    auto quadtree = std::make_unique<Quadtree>();
    if (!quadtree->init(*asset, _lodQualityScale)) {
        return;
    }
    auto renderer = std::make_unique<LandscapeRenderer>();
    if (!renderer->init(_node.get(), _scene)) {
        return;
    }
    if (!renderer->setAsset(asset.get())) {
        return;
    }
    renderer->setLodRanges(quadtree->lodMorphStart(), quadtree->lodMorphEnd());
    renderer->setDebugData(_debugData);
    renderer->setGlobalColorMap(_globalColorMap);
    renderer->setGlobalColorStrength(_globalColorStrength);
    renderer->setCastShadow(_castShadow);
    renderer->setReceiveShadow(_receiveShadow);

#if CC_LANDSCAPE_DEBUG
    const auto &data = asset->data();
    CC_LOG_INFO("[Landscape] LOD ranges:");
    for (uint32_t level = 0; level <= data.maxLevel; ++level) {
        CC_LOG_INFO("[Landscape]   LOD%u: morphStart=%.3f morphEnd=%.3f",
                    level, quadtree->lodMorphStart()[level], quadtree->lodMorphEnd()[level]);
    }
#endif

    _asset = std::move(asset);
    _quadtree = std::move(quadtree);
    _renderer = std::move(renderer);
    _scene->addLandscape(this);
}

void Landscape::setGlobalColorMap(Texture2D *texture) {
    _globalColorMap = texture;
    if (_renderer) _renderer->setGlobalColorMap(texture);
}

void Landscape::setGlobalColorStrength(float strength) {
    if (!std::isfinite(strength)) return;
    _globalColorStrength = std::clamp(strength, 0.0F, 1.0F);
    if (_renderer) _renderer->setGlobalColorStrength(_globalColorStrength);
}

void Landscape::onDisable() {
    if (_scene) _scene->removeLandscape(this);
    _passes.clear();
    _coveredShadowModels.clear();
    _passCount = 0;
    _geometryNodes.clear();
    _renderer = nullptr;
    _quadtree = nullptr;
    _query = nullptr;
    _asset = nullptr;
    _debugNodes.clear();
    _lastVisibilityDistanceWarning = false;
    _scene = nullptr;
    _node = nullptr;
}

void Landscape::update() {
    if (_query) _query->update(); // No camera or render-readiness dependency.
    if (!_node || !_renderer || !_quadtree || !_renderer->valid()) return;
    if (_debugData.freezeLod && _renderer->isReady()) return;
    auto *camera = pickMainCamera();
    if (!camera) return;
    camera->update();
    _lodViewPosition = camera->getPosition();
    _renderer->setViewPos(_lodViewPosition);
}

void Landscape::selectPass(const geometry::Frustum &frustum, bool shadow) {
    if (_passCount == _passes.size()) _passes.emplace_back();
    auto &pass = _passes[_passCount++];
    pass.frustum = &frustum;
    pass.shadow = shadow;
    pass.nodes.clear();
    pass.models.clear();
    _visibilityDistanceWarning |= selectNodes(frustum, pass.nodes);
}

bool Landscape::queryTransformValid() const {
    if (!_node) return false;
    const auto &m = _node->getWorldMatrix();
    for (uint32_t i = 0; i < 12; ++i) {
        const float expected = (i == 0 || i == 5 || i == 10) ? 1.0F : 0.0F;
        if (!std::isfinite(m.m[i]) || std::abs(m.m[i] - expected) > 1e-5F) return false;
    }
    return std::isfinite(m.m[12]) && std::isfinite(m.m[13]) && std::isfinite(m.m[14]);
}

void Landscape::setQueryCacheCapacity(uint32_t capacity) {
    if (!_query && capacity > 0 && capacity <= 4096) _queryCacheCapacity = capacity;
}

uint32_t Landscape::setQuerySource(uint32_t id, float worldX, float worldZ, float radius) {
    if (!_asset || !_node) return static_cast<uint32_t>(LandscapeQueryStatus::NOT_READY);
    if (!queryTransformValid()) {
        removeQuerySource(id);
        return static_cast<uint32_t>(LandscapeQueryStatus::ERROR);
    }
    if (!_query) {
        _query = std::make_unique<LandscapeQuery>(_asset->data(),
            [this](uint32_t x, uint32_t z, LandscapeQuery::Completion completion) {
                _asset->requestQueryTile(x, z, std::move(completion));
            }, _queryCacheCapacity);
    }
    const auto &origin = _node->getWorldPosition();
    return static_cast<uint32_t>(_query->setSource(id, worldX - origin.x, worldZ - origin.z, radius));
}

void Landscape::removeQuerySource(uint32_t id) {
    if (_query) _query->removeSource(id);
}

uint32_t Landscape::getQuerySourceStatus(uint32_t id) const {
    return static_cast<uint32_t>(_query ? _query->sourceStatus(id) : LandscapeQueryStatus::NOT_READY);
}

uint32_t Landscape::sampleSurface(float worldX, float worldZ, Float32Array output) {
    if (output.length() < 8 || !std::isfinite(worldX) || !std::isfinite(worldZ))
        return static_cast<uint32_t>(LandscapeQueryStatus::ERROR);
    if (!_asset || !_node) return static_cast<uint32_t>(LandscapeQueryStatus::NOT_READY);
    if (!queryTransformValid()) return static_cast<uint32_t>(LandscapeQueryStatus::ERROR);
    const auto &origin = _node->getWorldPosition();
    const auto &data = _asset->data();
    const float x = worldX - origin.x, z = worldZ - origin.z;
    if (std::abs(x) > data.worldWidth() * 0.5F || std::abs(z) > data.worldDepth() * 0.5F)
        return static_cast<uint32_t>(LandscapeQueryStatus::MISS);
    if (!_query) return static_cast<uint32_t>(LandscapeQueryStatus::NOT_READY);
    const auto result = _query->sample(x, z);
    if (result.status == LandscapeQueryStatus::HIT) {
        output[0] = worldX; output[1] = result.position.y + origin.y; output[2] = worldZ;
        output[3] = result.normal.x; output[4] = result.normal.y; output[5] = result.normal.z;
        output[6] = static_cast<float>(result.surfaceType); output[7] = result.surfaceWeight;
    }
    return static_cast<uint32_t>(result.status);
}

bool Landscape::selectNodes(const geometry::Frustum &frustum, ccstd::vector<QuadNode> &nodes) {
    nodes.clear();
    const Vec3 base = _node->getWorldPosition();
    const auto &data = _asset->data();
    const float halfWorldX = data.worldWidth() * 0.5F;
    const float halfWorldZ = data.worldDepth() * 0.5F;
    bool visibilityDistanceWarning = false;
    for (uint32_t sectorZ = 0; sectorZ < data.sectorsZ; ++sectorZ) {
        for (uint32_t sectorX = 0; sectorX < data.sectorsX; ++sectorX) {
            const Vec3 origin{
                base.x + (static_cast<float>(sectorX) + 0.5F) * data.sectorSize - halfWorldX,
                base.y,
                base.z + (static_cast<float>(sectorZ) + 0.5F) * data.sectorSize - halfWorldZ,
            };
            const auto &local = _quadtree->select(_lodViewPosition, frustum,
                                                  origin, sectorX, sectorZ);
            visibilityDistanceWarning |= _quadtree->visibilityDistanceTooSmall();
            for (const auto &node : local) {
                const uint32_t scale = computeNodesPerSide(data.maxLevel, node.level);
                nodes.push_back(QuadNode{
                    node.level,
                    sectorX * scale + node.ix,
                    sectorZ * scale + node.iz,
                    node.minY,
                    node.maxY,
                    node.quadrantMask,
                });
            }
        }
    }
    return visibilityDistanceWarning;
}

void Landscape::preparePasses(const scene::Camera &camera, const pipeline::PipelineSceneData &sceneData) {
    _passCount = 0;
    if (!_renderer || !_quadtree || !_renderer->valid()) return;
    const auto layer = _node->getLayer();
    // UI/other cameras that hide terrain must not rebuild its models or VT plan.
    if ((camera.getVisibility() & layer) != layer) return;
    update();
    _visibilityDistanceWarning = false;

    // The first selection is the only color pass in this forward camera batch.
    selectPass(camera.getFrustum(), false);
    uint32_t cascadesToDeduplicate = 0;
    const auto *shadows = sceneData.getShadows();
    if (_castShadow && shadows && shadows->isEnabled() && shadows->getType() == scene::ShadowType::SHADOW_MAP) {
        const auto *mainLight = _scene->getMainLight();
        const auto *csmLayers = sceneData.getCSMLayers();
        if (mainLight && mainLight->isShadowEnabled()) {
            if (mainLight->isShadowFixedArea()) {
                selectPass(csmLayers->getSpecialLayer()->getValidFrustum(), true);
            } else {
                const uint32_t count = sceneData.getCSMSupported() ? static_cast<uint32_t>(mainLight->getCSMLevel()) : 1U;
                if (mainLight->getCSMOptimizationMode() == scene::CSMOptimizationMode::REMOVE_DUPLICATES) {
                    cascadesToDeduplicate = count;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    selectPass(csmLayers->getLayers()[i]->getValidFrustum(), true);
                }
            }
        }
        for (const auto *light : sceneData.getValidPunctualLights()) {
            if (light->getType() != scene::LightType::SPOT) continue;
            const auto *spot = static_cast<const scene::SpotLight *>(light);
            if (spot->isShadowEnabled()) selectPass(spot->getFrustum(), true);
        }
    }
    if (_visibilityDistanceWarning != _lastVisibilityDistanceWarning) {
        if (_visibilityDistanceWarning) {
            CC_LOG_WARNING("[Landscape] CDLOD granularity check: visibility ranges are too small for a guaranteed transition");
        }
#if CC_LANDSCAPE_DEBUG
        else {
            CC_LOG_INFO("[Landscape] CDLOD granularity check: OK");
        }
#endif
        _lastVisibilityDistanceWarning = _visibilityDistanceWarning;
    }
    // Only residency/model storage is shared. Each pass keeps its own root traversal result.
    auto &surfaceNodes = _passes.front().nodes;
    mergeNodeSelections(surfaceNodes);
    _geometryNodes.clear();
    for (size_t i = 0; i < _passCount; ++i) {
        const auto &nodes = _passes[i].nodes;
        _geometryNodes.insert(_geometryNodes.end(), nodes.begin(), nodes.end());
    }
    mergeNodeSelections(_geometryNodes);
    _renderer->preparePasses(_geometryNodes, surfaceNodes);
    for (size_t i = 0; i < _passCount; ++i) {
        auto &pass = _passes[i];
        _renderer->collectPassModels(pass.nodes, *pass.frustum, pass.shadow, pass.models);
    }
    removeCSMDuplicates(cascadesToDeduplicate);
}

void Landscape::removeCSMDuplicates(uint32_t cascadeCount) {
    _coveredShadowModels.clear();
    if (cascadeCount < 2U) return;
    // Match legacy shadowCulling: a model completely inside an earlier cascade
    // is omitted from later cascades. Intersecting the frustum is not sufficient.
    // All passes have already traversed their own quadtrees independently.
    // Directional cascades follow the color pass; spot passes are never deduplicated.
    for (uint32_t i = 1; i <= cascadeCount; ++i) {
        auto &pass = _passes[i];
        size_t count = 0;
        for (const auto *model : pass.models) {
            if (_coveredShadowModels.count(model) != 0) continue;
            pass.models[count++] = model;
            if (i < cascadeCount && geometry::aabbFrustumCompletelyInside(*model->getWorldBounds(), *pass.frustum)) {
                _coveredShadowModels.insert(model);
            }
        }
        pass.models.resize(count);
    }
}

const ccstd::vector<const scene::Model *> &Landscape::getPassModels(const geometry::Frustum &frustum, bool shadow) const {
    for (size_t i = 0; i < _passCount; ++i) {
        const auto &pass = _passes[i];
        if (pass.frustum == &frustum && pass.shadow == shadow) return pass.models;
    }
    static const ccstd::vector<const scene::Model *> empty;
    return empty;
}

void Landscape::onGlobalPipelineStateChanged() {
    if (_renderer) _renderer->onGlobalPipelineStateChanged();
}

bool Landscape::isReady() const {
    return _renderer != nullptr && _renderer->isReady();
}

void Landscape::setDebugData(const LandscapeDebugData &data) {
    _debugData = data;
    if (_renderer) _renderer->setDebugData(data);
}

void Landscape::setCastShadow(bool enabled) {
    _castShadow = enabled;
    if (_renderer) _renderer->setCastShadow(enabled);
}

void Landscape::setReceiveShadow(bool enabled) {
    _receiveShadow = enabled;
    if (_renderer) _renderer->setReceiveShadow(enabled);
}

void Landscape::setAssetPath(const ccstd::string &manifestPath) {
    _assetPath = manifestPath;
}

scene::Camera *Landscape::pickMainCamera() const {
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

void Landscape::drawDebugBounds() {
#if CC_USE_GEOMETRY_RENDERER
    if (_scene == nullptr || _node == nullptr || !_quadtree) {
        return;
    }
    auto *camera = pickMainCamera();
    if (camera == nullptr) {
        return;
    }
    // Debug drawing precedes pass collection. Traverse the current camera view
    // here instead of drawing last frame's selection after a camera rotation.
    selectNodes(camera->getFrustum(), _debugNodes);
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
    const auto &data = _asset->data();
    const float halfWorldX = data.worldWidth() * 0.5F;
    const float halfWorldZ = data.worldDepth() * 0.5F;
    for (const auto &node : _debugNodes) {
        const float nodeSize = computeNodeSize(data.sectorSize, data.maxLevel, node.level);
        const float quadrantSize = nodeSize * 0.5F;
        const float nodeX = static_cast<float>(node.ix) * nodeSize - halfWorldX;
        const float nodeZ = static_cast<float>(node.iz) * nodeSize - halfWorldZ;
        for (uint32_t quadrant = 0U; quadrant < 4U; ++quadrant) {
            if ((node.quadrantMask & (1U << quadrant)) == 0U) {
                continue;
            }
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
    }
#endif
}

RenderTexture *Landscape::getVTAtlas() const {
    return _renderer ? _renderer->vtAtlas() : nullptr;
}

} // namespace landscape
} // namespace cc

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
#include "core/geometry/AABB.h"
#include "core/geometry/Sphere.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/LandscapeRenderer.h"
#include "landscape/Quadtree.h"
#include "math/Mat4.h"
#include "math/Vec3.h"
#include "renderer/pipeline/GeometryRenderer.h"
#include "renderer/pipeline/PipelineSceneData.h"
#include "renderer/pipeline/custom/RenderInterfaceTypes.h"
#include "renderer/pipeline/shadow/CSMLayers.h"
#include "scene/Camera.h"
#include "scene/DirectionalLight.h"
#include "scene/RenderScene.h"
#include "scene/Shadow.h"
#include "scene/SpotLight.h"

namespace cc {
namespace landscape {

namespace {

void appendShadowFrusta(scene::RenderScene *scene, scene::Camera *camera,
                        ccstd::vector<const geometry::Frustum *> &frusta) {
    auto *root = Root::getInstance();
    auto *pipeline = root ? root->getPipeline() : nullptr;
    auto *sceneData = pipeline ? pipeline->getPipelineSceneData() : nullptr;
    auto *shadows = sceneData ? sceneData->getShadows() : nullptr;
    if (!shadows || !shadows->isEnabled() || shadows->getType() != scene::ShadowType::SHADOW_MAP) {
        return;
    }

    auto *sun = scene->getMainLight();
    if (sun && sun->getNode() && sun->isShadowEnabled()) {
        // BEFORE_DRAW precedes pipeline culling. Update light frusta here so
        // moving the camera/light never selects casters using last frame's CSM.
        sun->update();
        auto *csm = sceneData->getCSMLayers();
        csm->update(sceneData, camera);
        if (sun->isShadowFixedArea()) {
            frusta.push_back(&csm->getSpecialLayer()->getValidFrustum());
        } else {
            const auto count = sceneData->getCSMSupported() ? static_cast<uint32_t>(sun->getCSMLevel()) : 1U;
            for (uint32_t i = 0; i < count; ++i) {
                frusta.push_back(&csm->getLayers()[i]->getValidFrustum());
            }
        }
    }
    for (const auto &light : scene->getSpotLights()) {
        if (!light->getNode() || light->isBaked() || !light->isShadowEnabled()) continue;
        light->update();
        geometry::Sphere bounds;
        bounds.setCenter(light->getPosition());
        bounds.setRadius(light->getRange());
        if (bounds.sphereFrustum(camera->getFrustum())) {
            frusta.push_back(&light->getFrustum());
        }
    }
}

} // namespace

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
    renderer->setDebugFlags(_lodColor, _showRanges);
    renderer->setUnlit(_unlit);
    renderer->setVTMipEnabled(_vtMipEnabled);
    renderer->setHeightBlendEnabled(_heightBlendEnabled);
    renderer->setDecal3DEnabled(_decal3DEnabled);
    renderer->setRVTNormalEnabled(_rvtNormalEnabled);
    renderer->setCliffEnabled(_cliffEnabled);
    renderer->setGlobalColorStrength(_globalColorStrength);
    renderer->setWireframe(_wireframe);
    renderer->setCastShadow(_castShadow);
    renderer->setReceiveShadow(_receiveShadow);
    renderer->setFreezeLod(_freezeLod);

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
}

void Landscape::setGlobalColorStrength(float strength) {
    if (!std::isfinite(strength)) return;
    _globalColorStrength = std::clamp(strength, 0.0F, 1.0F);
    if (_renderer) _renderer->setGlobalColorStrength(_globalColorStrength);
}

void Landscape::onDisable() {
    _renderer = nullptr;
    _quadtree = nullptr;
    _asset = nullptr;
    _selected.clear();
#if CC_LANDSCAPE_DEBUG
    _lastLodNodeCounts.clear();
#endif
    _lastVisibilityDistanceWarning = false;
    _scene = nullptr;
    _node = nullptr;
}

void Landscape::update() {
    if (_node == nullptr) {
        return;
    }
    if (_renderer == nullptr || _quadtree == nullptr || !_renderer->valid()) {
        return;
    }
    if (_freezeLod && _renderer->isReady()) {
        return; // Keep both geometry selection and VT residency unchanged.
    }
    auto *camera = pickMainCamera();
    if (camera == nullptr) {
        return;
    }
    // Selection runs before the render window updates its cameras. Refresh the
    // view/projection here so culling and geomorph use this frame's camera pose.
    camera->update();
    _renderer->setViewPos(camera->getPosition());

    // Select once using the MAIN camera's distance for both LOD and morph.
    // Light frusta only retain offscreen casters; shadow passes reuse these models.
    ccstd::vector<const geometry::Frustum *> frusta{&camera->getFrustum()};
    if (_castShadow) appendShadowFrusta(_scene, camera, frusta);

    _selected.clear();
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
            const auto &local = _quadtree->select(camera->getPosition(), frusta,
                                                  origin, sectorX, sectorZ);
            visibilityDistanceWarning |= _quadtree->visibilityDistanceTooSmall();
            for (const auto &node : local) {
                const uint32_t scale = computeNodesPerSide(data.maxLevel, node.level);
                _selected.push_back(QuadNode{
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
    if (visibilityDistanceWarning != _lastVisibilityDistanceWarning) {
        if (visibilityDistanceWarning) {
            CC_LOG_WARNING("[Landscape] CDLOD granularity check: visibility ranges are too small for a guaranteed transition");
        }
#if CC_LANDSCAPE_DEBUG
        else {
            CC_LOG_INFO("[Landscape] CDLOD granularity check: OK");
        }
#endif
        _lastVisibilityDistanceWarning = visibilityDistanceWarning;
    }
#if CC_LANDSCAPE_DEBUG
    ccstd::vector<uint32_t> lodNodeCounts(data.maxLevel + 1U, 0U);
    for (const auto &node : _selected) {
        if (node.level <= data.maxLevel) {
            for (uint32_t quadrant = 0U; quadrant < 4U; ++quadrant) {
                lodNodeCounts[node.level] += (node.quadrantMask >> quadrant) & 1U;
            }
        }
    }

    if (lodNodeCounts != _lastLodNodeCounts) {
        constexpr uint32_t quadsPerQuadrantSide = (config::VERTS_PER_NODE_SIDE - 1U) / 2U;
        constexpr uint32_t trianglesPerQuadrant = quadsPerQuadrantSide * quadsPerQuadrantSide * 2U;
        uint32_t totalNodeCount = 0U;
        uint32_t totalTriangleCount = 0U;
        CC_LOG_INFO("[Landscape] LOD stats:");
        for (uint32_t level = 0; level <= data.maxLevel; ++level) {
            const uint32_t triangles = lodNodeCounts[level] * trianglesPerQuadrant;
            totalNodeCount += lodNodeCounts[level];
            totalTriangleCount += triangles;
            CC_LOG_INFO("[Landscape]   LOD%u: quadrantInstances=%u triangles=%u",
                        level, lodNodeCounts[level], triangles);
        }
        CC_LOG_INFO("[Landscape]   total: quadrantInstances=%u triangles=%u",
                    totalNodeCount, totalTriangleCount);
        _lastLodNodeCounts = lodNodeCounts;
    }
#endif
    _renderer->sync(_selected);
}

bool Landscape::isReady() const {
    return _renderer != nullptr && _renderer->isReady();
}

void Landscape::setFreezeLod(bool frozen) {
    _freezeLod = frozen;
    if (_renderer != nullptr) {
        _renderer->setFreezeLod(frozen);
    }
}

void Landscape::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    if (_renderer != nullptr) {
        _renderer->setWireframe(wireframe);
    }
}

void Landscape::setCastShadow(bool enabled) {
    _castShadow = enabled;
    if (_renderer) _renderer->setCastShadow(enabled);
}

void Landscape::setReceiveShadow(bool enabled) {
    _receiveShadow = enabled;
    if (_renderer) _renderer->setReceiveShadow(enabled);
}

void Landscape::setLodColor(bool enabled) {
    _lodColor = enabled;
    if (_renderer != nullptr) {
        _renderer->setDebugFlags(_lodColor, _showRanges);
    }
}

void Landscape::setShowRanges(bool enabled) {
    _showRanges = enabled;
    if (_renderer != nullptr) {
        _renderer->setDebugFlags(_lodColor, _showRanges);
    }
}

void Landscape::setAssetPath(const ccstd::string &manifestPath) {
    _assetPath = manifestPath;
}

void Landscape::setUnlit(bool enabled) {
    _unlit = enabled;
    if (_renderer != nullptr) {
        _renderer->setUnlit(enabled);
    }
}

void Landscape::setVTMipEnabled(bool enabled) {
    _vtMipEnabled = enabled;
    if (_renderer != nullptr) {
        _renderer->setVTMipEnabled(enabled);
    }
}

void Landscape::setDecal3DEnabled(bool enabled) {
    _decal3DEnabled = enabled;
    if (_renderer) _renderer->setDecal3DEnabled(enabled);
}

void Landscape::setRVTNormalEnabled(bool enabled) {
    _rvtNormalEnabled = enabled;
    if (_renderer) _renderer->setRVTNormalEnabled(enabled);
}

void Landscape::setCliffEnabled(bool enabled) {
    _cliffEnabled = enabled;
    if (_renderer) _renderer->setCliffEnabled(enabled);
}

void Landscape::setHeightBlendEnabled(bool enabled) {
    _heightBlendEnabled = enabled;
    if (_renderer != nullptr) {
        _renderer->setHeightBlendEnabled(enabled);
    }
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
    if (_scene == nullptr || _node == nullptr || _selected.empty()) {
        return;
    }
    auto *camera = pickMainCamera();
    if (camera == nullptr) {
        return;
    }
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
    for (const auto &node : _selected) {
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
            // shares an edge with the box. Sector seams below remain depth-tested.
            geometry->addBoundingBox(box, colors[node.level], true, false, false, true, world);
        }
    }
#endif
}

void Landscape::drawDebugSectors() {
#if CC_USE_GEOMETRY_RENDERER
    if (_scene == nullptr || _node == nullptr || _asset == nullptr) {
        return;
    }
    auto *camera = pickMainCamera();
    if (camera == nullptr) {
        return;
    }
    camera->initGeometryRenderer();
    auto *geometry = camera->getGeometryRenderer();
    if (geometry == nullptr) {
        return;
    }
    static const gfx::Color color{1.0F, 0.15F, 0.90F, 1.0F};
    const Mat4 &world = _node->getWorldMatrix();
    const auto &data = _asset->data();
    const float halfWorldX = data.worldWidth() * 0.5F;
    const float halfWorldZ = data.worldDepth() * 0.5F;
    const uint32_t segments = 256U;
    const float lift = 3.0F;
    auto emitSeamLine = [&](float x0, float z0, float x1, float z1) {
        Vec3 previous{x0, data.minHeight() + lift, z0};
        for (uint32_t i = 1U; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            Vec3 current{
                x0 + (x1 - x0) * t,
                0.0F,
                z0 + (z1 - z0) * t,
            };
            current.y = data.minHeight() + lift;

            Vec3 worldPrevious;
            Vec3 worldCurrent;
            Vec3::transformMat4(previous, world, &worldPrevious);
            Vec3::transformMat4(current, world, &worldCurrent);
            geometry->addLine(worldPrevious, worldCurrent, color, true);
            previous = current;
        }
    };

    for (uint32_t i = 0U; i <= data.sectorsX; ++i) {
        const float p = static_cast<float>(i) * data.sectorSize - halfWorldX;
        emitSeamLine(p, -halfWorldZ, p, halfWorldZ);
    }
    for (uint32_t i = 0U; i <= data.sectorsZ; ++i) {
        const float p = static_cast<float>(i) * data.sectorSize - halfWorldZ;
        emitSeamLine(-halfWorldX, p, halfWorldX, p);
    }
#endif
}

RenderTexture *Landscape::getDebugAtlas() const {
    return _renderer ? _renderer->debugAtlas() : nullptr;
}

} // namespace landscape
} // namespace cc

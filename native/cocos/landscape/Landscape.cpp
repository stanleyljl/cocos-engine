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

#include <cstdio>

#include "base/Log.h"
#include "core/geometry/AABB.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "landscape/LandscapeRenderer.h"
#include "landscape/Quadtree.h"
#include "math/Mat4.h"
#include "math/Vec3.h"
#include "renderer/pipeline/GeometryRenderer.h"
#include "scene/Camera.h"
#include "scene/RenderScene.h"

namespace cc {
namespace landscape {

Landscape::Landscape() = default;

Landscape::~Landscape() {
    onDisable();
}

void Landscape::onEnable(Node *node) {
    if (node == nullptr) {
        return;
    }
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

    auto quadtree = std::make_unique<Quadtree>();
    quadtree->setConfig(config::SECTOR_SIZE, config::MAX_LEVEL,
                        config::TERRAIN_MIN_Y, config::TERRAIN_MAX_Y);
    quadtree->setProceduralWorld(config::DEMO_SECTORS, config::DEMO_SECTORS);

    auto renderer = std::make_unique<LandscapeRenderer>();
    if (!renderer->init(_node.get(), _scene)) {
        return;
    }
    renderer->setWorld(config::DEMO_SECTORS, config::DEMO_SECTORS,
                       config::SECTOR_SIZE, config::MAX_LEVEL);
    renderer->setHeightRange(config::HEIGHT_SCALE, config::HEIGHT_BIAS);
    renderer->setProceduralHeightmap(quadtree->proceduralHeightmap(),
                                     quadtree->proceduralHeightmapSize());
    renderer->setLodRanges(quadtree->lodMorphStart(), quadtree->lodMorphEnd());
    renderer->setDebugFlags(_lodColor, _showRanges);
    renderer->setWireframe(_wireframe);

    _quadtree = std::move(quadtree);
    _renderer = std::move(renderer);
}

void Landscape::onDisable() {
    _renderer = nullptr;
    _quadtree = nullptr;
    _selected.clear();
    _scene = nullptr;
    _node = nullptr;
}

void Landscape::update() {
    if (_node == nullptr) {
        return;
    }
    if (_renderer == nullptr || _quadtree == nullptr) {
        initializeRenderer();
    }
    if (_renderer == nullptr || _quadtree == nullptr || !_renderer->valid()) {
        return;
    }

    auto *camera = pickMainCamera();
    if (camera == nullptr) {
        return;
    }
    _renderer->setMorphCameraPosition(camera->getPosition());

    _selected.clear();
    const Vec3 base = _node->getWorldPosition();
    const float halfWorldX = config::SECTOR_SIZE * static_cast<float>(config::DEMO_SECTORS) * 0.5F;
    const float halfWorldZ = halfWorldX;
    for (uint32_t sectorZ = 0; sectorZ < config::DEMO_SECTORS; ++sectorZ) {
        for (uint32_t sectorX = 0; sectorX < config::DEMO_SECTORS; ++sectorX) {
            const Vec3 origin{
                base.x + (static_cast<float>(sectorX) + 0.5F) * config::SECTOR_SIZE - halfWorldX,
                base.y,
                base.z + (static_cast<float>(sectorZ) + 0.5F) * config::SECTOR_SIZE - halfWorldZ,
            };
            const auto &local = _quadtree->select(camera->getPosition(), camera->getFrustum(),
                                                  origin, sectorX, sectorZ);
            for (const auto &node : local) {
                const uint32_t scale = 1U << (config::MAX_LEVEL - node.level);
                _selected.push_back(QuadNode{
                    node.level,
                    sectorX * scale + node.ix,
                    sectorZ * scale + node.iz,
                    node.minY,
                    node.maxY,
                });
            }
        }
    }
    _renderer->sync(_selected);
}

void Landscape::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    if (_renderer != nullptr) {
        _renderer->setWireframe(wireframe);
    }
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

void Landscape::setDataDir(const ccstd::string &dir) {
    // The current parity path uses the cdlod demo's procedural source.
    _dataDir = dir;
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
    };
    const Mat4 &world = _node->getWorldMatrix();
    const float halfWorld = config::WORLD_SIZE * 0.5F;
    for (const auto &node : _selected) {
        const float size = config::SECTOR_SIZE /
                           static_cast<float>(1U << (config::MAX_LEVEL - node.level));
        const float x = static_cast<float>(node.ix) * size - halfWorld;
        const float z = static_cast<float>(node.iz) * size - halfWorld;
        geometry::AABB box{
            x + size * 0.5F,
            (node.minY + node.maxY) * 0.5F,
            z + size * 0.5F,
            size * 0.5F,
            (node.maxY - node.minY) * 0.5F,
            size * 0.5F,
        };
        geometry->addBoundingBox(box, colors[node.level % 8U], true, true, false, true, world);
    }
#endif
}

void Landscape::drawDebugSectors() {
#if CC_USE_GEOMETRY_RENDERER
    if (_scene == nullptr || _node == nullptr) {
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
    const float minY = config::TERRAIN_MIN_Y;
    const float maxY = config::TERRAIN_MAX_Y;
    const float halfWorld = config::WORLD_SIZE * 0.5F;
    for (uint32_t z = 0; z < config::DEMO_SECTORS; ++z) {
        for (uint32_t x = 0; x < config::DEMO_SECTORS; ++x) {
            const float x0 = static_cast<float>(x) * config::SECTOR_SIZE - halfWorld;
            const float z0 = static_cast<float>(z) * config::SECTOR_SIZE - halfWorld;
            geometry::AABB box{
                x0 + config::SECTOR_SIZE * 0.5F,
                (minY + maxY) * 0.5F,
                z0 + config::SECTOR_SIZE * 0.5F,
                config::SECTOR_SIZE * 0.5F,
                (maxY - minY) * 0.5F,
                config::SECTOR_SIZE * 0.5F,
            };
            geometry->addBoundingBox(box, color, true, true, false, true, world);
        }
    }
#endif
}

RenderTexture *Landscape::getDebugAtlas() const {
    return _renderer ? _renderer->debugAtlas() : nullptr;
}

} // namespace landscape
} // namespace cc

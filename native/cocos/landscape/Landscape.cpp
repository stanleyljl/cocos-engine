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

#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/LandscapeRenderer.h"

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

    auto renderer = std::make_unique<LandscapeRenderer>();
    if (!renderer->init(_node.get(), _scene)) {
        return;
    }
    renderer->setWorldSize(config::WORLD_SIZE, config::WORLD_SIZE);
    renderer->setWireframe(_wireframe);
    _renderer = std::move(renderer);
}

void Landscape::onDisable() {
    _renderer = nullptr;
    _scene = nullptr;
    _node = nullptr;
}

void Landscape::update() {
    // The flat implementation has no per-frame selection or streaming pass.
    if (_renderer == nullptr && _node != nullptr) {
        initializeRenderer();
    }
}

void Landscape::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    if (_renderer != nullptr) {
        _renderer->setWireframe(wireframe);
    }
}

void Landscape::setLodColor(bool enabled) {
    _lodColor = enabled;
}

void Landscape::setShowRanges(bool enabled) {
    _showRanges = enabled;
}

void Landscape::setDataDir(const ccstd::string &dir) {
    // Kept for scene compatibility. The flat path does not load terrain tiles.
    _dataDir = dir;
}

void Landscape::drawDebugBounds() {
    // Quadtree debug bounds are intentionally unavailable in the flat path.
}

void Landscape::drawDebugSectors() {
    // Sector debug bounds are intentionally unavailable in the flat path.
}

RenderTexture *Landscape::getDebugAtlas() const {
    return nullptr;
}

} // namespace landscape
} // namespace cc

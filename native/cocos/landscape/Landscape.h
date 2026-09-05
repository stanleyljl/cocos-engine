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

#pragma once

#include <memory>

#include "base/Ptr.h"
#include "base/RefCounted.h"
#include "base/std/container/array.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeConfig.h"

namespace cc {

class Node;
class RenderTexture;
namespace scene {
class Camera;
class RenderScene;
} // namespace scene

namespace landscape {

class LandscapeAsset;
class LandscapeRenderer;
class Quadtree;

class Landscape : public RefCounted {
public:
    Landscape();
    ~Landscape() override;

    void onEnable(Node *node);
    void onDisable();
    void update();

    void setWireframe(bool wireframe);
    void setFreezeLod(bool frozen);
    void setLodColor(bool enabled);
    void setShowRanges(bool enabled);
    void setDataDir(const ccstd::string &dir);

    void drawDebugBounds();
    void drawDebugSectors();

    inline bool isInitialized() const { return _renderer != nullptr; }
    RenderTexture *getDebugAtlas() const;

private:
    void initializeRenderer();
    scene::Camera *pickMainCamera() const;

    bool _wireframe{false};
    bool _freezeLod{false};
    bool _lodColor{false};
    bool _showRanges{false};
    ccstd::string _dataDir;
    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<Quadtree> _quadtree;
    std::unique_ptr<LandscapeRenderer> _renderer;
    ccstd::vector<QuadNode> _selected;
    ccstd::vector<uint32_t> _lastLodNodeCounts;
};

} // namespace landscape
} // namespace cc

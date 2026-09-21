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
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeConfig.h"
#include "math/Vec3.h"

namespace cc {

class Node;
class RenderTexture;
class Texture2D;
namespace geometry {
class Frustum;
}
namespace pipeline {
class PipelineSceneData;
}
namespace scene {
class Camera;
class Model;
class RenderScene;
} // namespace scene

namespace landscape {

class LandscapeAsset;
class LandscapeRenderer;
class Quadtree;

struct LandscapeDebugData {
    bool wireframe{false};
    bool lodColor{false};
    bool showRanges{false};
    bool showBox{false};
    bool freezeLod{false};
    bool unlit{false};
    bool showVTAtlas{false};
    bool cliffEnabled{true};
    bool heightBlendEnabled{true};
    bool bakeNormalEnabled{true};
    bool decal3DEnabled{true};
};

class Landscape : public RefCounted {
public:
    Landscape();
    ~Landscape() override;

    // LOD quality is fixed for the lifetime of the loaded terrain.
    void onEnable(Node *node, float lodQualityScale = 1.0F);
    void onDisable();
    void update();

    // Legacy forward entry, called after the pipeline has updated its light frusta.
    void preparePasses(const scene::Camera &camera, const pipeline::PipelineSceneData &sceneData);
    const ccstd::vector<const scene::Model *> &getPassModels(const geometry::Frustum &frustum, bool shadow) const;
    void onGlobalPipelineStateChanged();

    void setDebugData(const LandscapeDebugData &data);
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setAssetPath(const ccstd::string &manifestPath);

    void drawDebugBounds();

    inline bool isInitialized() const { return _renderer != nullptr; }
    bool isReady() const;
    RenderTexture *getVTAtlas() const;

private:
    void initializeRenderer();
    scene::Camera *pickMainCamera() const;
    void selectPass(const geometry::Frustum &frustum, bool shadow);
    void removeCSMDuplicates(uint32_t cascadeCount);
    bool selectNodes(const geometry::Frustum &frustum, ccstd::vector<QuadNode> &nodes);

    struct PassSelection {
        const geometry::Frustum *frustum{nullptr};
        bool shadow{false};
        ccstd::vector<QuadNode> nodes;
        ccstd::vector<const scene::Model *> models;
    };
    ccstd::vector<PassSelection> _passes;
    ccstd::unordered_set<const scene::Model *> _coveredShadowModels;
    size_t _passCount{0};
    bool _visibilityDistanceWarning{false};
    Vec3 _lodViewPosition;
    ccstd::vector<QuadNode> _geometryNodes;

    LandscapeDebugData _debugData;
    bool _castShadow{true};
    bool _receiveShadow{true};
    float _lodQualityScale{1.0F};
    float _globalColorStrength{0.1F};
    IntrusivePtr<Texture2D> _globalColorMap;
    ccstd::string _assetPath;
    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<Quadtree> _quadtree;
    std::unique_ptr<LandscapeRenderer> _renderer;
    ccstd::vector<QuadNode> _debugNodes;
    bool _lastVisibilityDistanceWarning{false};
};

} // namespace landscape
} // namespace cc

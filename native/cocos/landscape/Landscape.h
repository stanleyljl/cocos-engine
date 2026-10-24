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
#include "landscape/Data.h"
#include "math/Vec3.h"
#include "core/TypedArray.h"

namespace cc {

class Node;
class TextureBase;
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
class LandscapeQuery;
class Quadtree;

class Landscape : public RefCounted {
public:
    Landscape();
    ~Landscape() override;

    // LOD quality is fixed for the lifetime of the loaded terrain.
    void onEnable(Node *node, float lodQualityScale, uint32_t queryCacheCapacity);
    void onDisable();
    // Once per tick, after camera controllers: advance queries and snapshot the LOD view.
    void update();

    // Render preparation only, after the pipeline has updated its light frusta; does not tick.
    void preparePasses(const scene::Camera &camera, const pipeline::PipelineSceneData &sceneData);
    const ccstd::vector<const scene::Model *> &getPassModels(const geometry::Frustum &frustum, bool shadow) const;
    void onGlobalPipelineStateChanged();

    void setDebugData(const LandscapeDebugData &data);
    void setHoleMode(uint32_t mode); // 0: fast vertex NaN, 1: fragment discard
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setAssetPath(const ccstd::string &manifestPath);

    void drawDebugBounds();

    inline bool isInitialized() const { return _renderer != nullptr; }
    bool isReady() const;
    TextureBase *getVTAtlas() const;

    // Main-thread runtime instances, using preloaded asset layers and local XZ.
    // Returns a nonzero handle on success, zero when unavailable/invalid/full.
    // Instances are released on disable/reload; stale handles cannot remove new instances.
    uint32_t addDecal(float x, float z, float size, uint32_t layer);
    bool removeDecal(uint32_t id);

    // World-space, translation-only placement, matching landscape selection.
    // Query result buffer: position XYZ, normal XYZ, material ID, weight.
    uint32_t sampleSurface(float worldX, float worldZ, Float32Array output);
    uint32_t setQuerySource(uint32_t id, float worldX, float worldZ, float radius);
    void removeQuerySource(uint32_t id);
    uint32_t getQuerySourceStatus(uint32_t id) const;

private:
    void initializeRenderer(uint32_t queryCacheCapacity);
    void applyRendererSettings(LandscapeRenderer &renderer) const;
    void logLodRanges(const LandscapeAsset &asset, const Quadtree &quadtree) const;
    scene::Camera *pickMainCamera() const;
    void addPass(const geometry::Frustum &frustum, bool shadow);
    uint32_t collectShadowPasses(const pipeline::PipelineSceneData &sceneData);
    void reportVisibilityDistanceStatus();
    void preparePassModels(uint32_t cascadesToDeduplicate);
    void removeCSMDuplicates(uint32_t cascadeCount);
    bool queryTransformValid() const;

    struct PassSelection {
        const geometry::Frustum *frustum{nullptr};
        bool shadow{false};
        ccstd::vector<const scene::Model *> models;
    };
    ccstd::vector<PassSelection> _passes;
    size_t _passCount{0};
    bool _visibilityDistanceWarning{false};
    Vec3 _lodViewPosition;

    LandscapeDebugData _debugData;
    uint32_t _holeMode{0};
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
    std::unique_ptr<LandscapeQuery> _query;
    uint32_t _nextDecalId{1}; // Never reset when reloading this Landscape object.
    bool _lastVisibilityDistanceWarning{false};
};

} // namespace landscape
} // namespace cc

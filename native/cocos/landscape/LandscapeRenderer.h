/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

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
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/LandscapeAsset.h"
#include "math/Vec3.h"
#include "scene/Model.h"

namespace cc {

class Node;
class RenderTexture;
class Material;
class RenderingSubMesh;

namespace gfx {
class Texture;
} // namespace gfx

namespace scene {
class RenderScene;
} // namespace scene

namespace landscape {

class TilePagePool;
class MaterialLibrary;

/**
 * Cocos render-side implementation of the cdlod_opengl grid renderer.
 *
 * The quadtree owns visibility and LOD selection. This class only turns the
 * selected nodes into Cocos models and supplies the per-node instance data
 * consumed by builtin-landscape.effect.
 */
class LandscapeRenderer {
public:
    LandscapeRenderer();
    ~LandscapeRenderer();

    bool init(Node *node, scene::RenderScene *scene);
    void destroy();
    bool valid() const;

    void setLodRanges(const ccstd::vector<float> &morphStart,
                      const ccstd::vector<float> &morphEnd);
    bool setAsset(LandscapeAsset *asset);
    void setDebugFlags(bool lodColor, bool showRanges);
    void sync(const ccstd::vector<QuadNode> &selected);
    void setWireframe(bool wireframe);
    void setFreezeLod(bool frozen);
    void setDetailHeightEnabled(bool enabled);
    void setViewPos(const Vec3 &position);
    RenderTexture *debugAtlas() const;

private:
    IntrusivePtr<scene::Model> createModel();
    void updateMaterialProperties();
    void updateMorphCameraProperty();
    void updateInstanceData(scene::Model *model, const QuadNode &node);
    void updateModel(scene::Model *model, const QuadNode &node);
    void updateModelBounds(scene::Model *model, const QuadNode &node);
    int resolveTilePage(const QuadNode &node, uint32_t &sourceLevel,
                          uint32_t &sourceX, uint32_t &sourceZ);

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<TilePagePool> _tilePages;
    std::unique_ptr<MaterialLibrary> _materialLibrary;

    ccstd::unordered_map<uint64_t, IntrusivePtr<scene::Model>> _active;
    ccstd::unordered_map<scene::Model *, QuadNode> _modelNodes;
    ccstd::vector<IntrusivePtr<scene::Model>> _pool;

    uint32_t _maxLevel{0};
    uint32_t _minTileLevel{0};
    uint32_t _tileResolution{0};
    float _sectorSize{0.0F};
    float _worldWidth{0.0F};
    float _worldDepth{0.0F};
    float _heightScale{0.0F};
    float _heightBias{0.0F};
    float _heightSampleSpacing{1.0F};
    Vec3 _viewPosition;
    ccstd::vector<float> _morphStart;
    ccstd::vector<float> _morphEnd;
    bool _lodColor{false};
    bool _showRanges{false};
    bool _wireframe{false};
    bool _freezeLod{false};
    bool _detailHeightEnabled{false};
};

} // namespace landscape
} // namespace cc

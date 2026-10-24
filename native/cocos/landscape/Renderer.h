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

#include <array>
#include <memory>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "core/TypedArray.h"
#include "landscape/Data.h"
#include "math/Vec3.h"
#include "math/Vec4.h"

namespace cc {

class Node;
class Material;
class RenderingSubMesh;
class Texture2D;
class TextureBase;

namespace geometry {
class Frustum;
} // namespace geometry

namespace gfx {
class Texture;
} // namespace gfx

namespace scene {
class RenderScene;
class Model;
} // namespace scene

namespace landscape {

class MaterialLibrary;
class VTRenderer;
class DecalRenderer;
class LandscapeAsset;
class LandscapeSchedule;
class Quadtree;
class LandscapeResidency;
struct LandscapeGridParams;

/**
 * Owns terrain models and material bindings. Landscape owns the single
 * Quadtree; LandscapeSchedule prepares its resource-backed changes.
 * Each committed node draws one shared 16 x 16 grid and selects material pages through a local mapping texture.
 */
class LandscapeRenderer {
public:
    explicit LandscapeRenderer(Quadtree &quadtree);
    ~LandscapeRenderer();

    // Runtime methods require successful initialization of all owned modules.
    bool init(Node *node, scene::RenderScene *scene, LandscapeAsset *asset);
    void destroy();
    // Latched after the initial view settles within budget with all exact bindings ready.
    bool isReady() const;

    void setLodRanges(const ccstd::vector<float> &morphStart, const ccstd::vector<float> &morphEnd);
    bool addDecal(uint32_t id, float x, float z, float size, uint32_t layer);
    bool removeDecal(uint32_t id);
    void setDebugData(const LandscapeDebugData &data);
    void preparePasses();
    void collectPassModels(const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void onGlobalPipelineStateChanged();
    void setHoleMode(uint32_t mode);
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setViewPos(const Vec3 &position);
    TextureBase *getVTAtlas() const;

private:
    struct GridDraw {
        // GPU ownership only. Geometry/material decisions live in Quadtree.
        int mapping{-1};
        IntrusivePtr<scene::Model> model;
    };

    void setUnlit(bool enabled);
    void syncTerrainModels();
    void bindRuntimeTextures();
    void recycleModel(GridDraw &draw);
    IntrusivePtr<scene::Model> createModel();
    void updateMaterialProperties();
    void refreshTerrainMaterials();
    void updateMorphCameraProperty();
    LandscapeGridParams resolveGridParams(const QuadNode &node, uint32_t mapping);
    std::array<Material *, 4> surfaceMaterials() const;
    void updateModel(scene::Model *model, const QuadNode &node, const LandscapeGridParams &gridParams);
    void updateModelBounds(scene::Model *model, const QuadNode &node);

    // Borrowed from Landscape; it outlives this renderer and its schedule.
    Quadtree &_quadtree;
    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;
    std::unique_ptr<DecalRenderer> _decals;
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<LandscapeResidency> _residency;
    std::unique_ptr<MaterialLibrary> _materialLibrary;
    std::unique_ptr<VTRenderer> _vtRenderer;
    std::unique_ptr<LandscapeSchedule> _schedule;

    ccstd::vector<IntrusivePtr<scene::Model>> _pool;
    ccstd::unordered_map<uint64_t, GridDraw> _draws;

    // Stored as TypedArray to avoid both per-update ArrayBuffers and implicit
    // Float32Array-to-TypedArray copies when setting instance attributes.
    TypedArray _instanceAttributeScratch;

    LandscapeDebugData _debugData;
    LandscapeData _data;
    uint32_t _holeMode{0};
    bool _heightUnorm{false};
    Vec3 _viewPosition;
    ccstd::vector<float> _morphStart;
    ccstd::vector<float> _morphEnd;
    bool _castShadow{true};
    bool _receiveShadow{true};
};

} // namespace landscape
} // namespace cc

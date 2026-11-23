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
class LandscapeResidency;

// Borrowed inputs are consumed synchronously; the renderer retains only GPU data.
struct LandscapeRenderNode {
    const QuadNode *geometry;
    const VTGridMapping *material;
    uint8_t edgeMask;
};

class LandscapeRenderer {
public:
    LandscapeRenderer();
    ~LandscapeRenderer();
    // Runtime methods require successful initialization of all owned modules.
    bool init(Node *node, scene::RenderScene *scene, LandscapeAsset *asset);
    void destroy();

    // Render data and pass collection.
    void prepareRenderData(uint32_t frame, gfx::Texture *previousMappings, const ccstd::vector<LandscapeRenderNode> &added, const ccstd::vector<LandscapeRenderNode> &updated, const ccstd::vector<uint64_t> &removed);
    bool collectNodeModels(const QuadNode &node, const LandscapeRegion &region, bool renderable, const geometry::Frustum &frustum, bool shadow, ccstd::vector<const scene::Model *> &models) const;

    // Runtime settings.
    void setViewPosition(const Vec3 &position);
    void setLodMorphRanges(const ccstd::vector<Vec4> &ranges);
    void setDebugData(const LandscapeDebugData &data);
    void setHoleMode(uint32_t mode);
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorMap(Texture2D *texture);
    void setGlobalColorStrength(float strength);
    void onGlobalPipelineStateChanged();

    // Runtime decals.
    uint32_t addDecal(float x, float z, float size, uint32_t layer);
    bool removeDecal(uint32_t id);

    LandscapeResidency &residency() const { return *_residency; }
    VTRenderer &vtRenderer() const { return *_vtRenderer; }
    TextureBase *getVTAtlas() const;

private:
    struct GridDraw {
        // GPU state; draw snapshots come from RenderTree, resource mappings from Schedule.
        int pageTableSlot{-1}; // -1 means unallocated.
        IntrusivePtr<scene::Model> model;
        LandscapeGridParams params; // Uploaded instance data, restored after material variants change.
    };

    // Terrain models.
    void syncTerrainModels(const ccstd::vector<LandscapeRenderNode> &added, const ccstd::vector<LandscapeRenderNode> &updated, const ccstd::vector<uint64_t> &removed);
    LandscapeGridParams resolveGridParams(const QuadNode &node, uint32_t pageTableSlot, uint8_t edgeMask);
    void updateModel(scene::Model *model, const QuadNode &node, const LandscapeGridParams &gridParams);
    void updateModelBounds(scene::Model *model, const QuadNode &node);
    IntrusivePtr<scene::Model> createModel();
    void recycleModel(GridDraw &draw);

    // Materials and bindings.
    std::array<Material *, 4> surfaceMaterials() const;
    void bindRuntimeTextures();
    void updateMaterialProperties();
    void updateMorphCameraProperty();
    void refreshTerrainMaterials();
    void setUnlit(bool enabled);

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    LandscapeData _data;

    std::unique_ptr<DecalRenderer> _decals;
    std::unique_ptr<LandscapeResidency> _residency;
    std::unique_ptr<MaterialLibrary> _materialLibrary;
    std::unique_ptr<VTRenderer> _vtRenderer;

    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;

    ccstd::unordered_map<uint64_t, GridDraw> _draws;
    ccstd::vector<IntrusivePtr<scene::Model>> _pool;
    // TypedArray avoids allocation and conversion when setting instance attributes.
    TypedArray _instanceAttributeScratch;

    Vec3 _viewPosition;
    LandscapeDebugData _debugData;
    uint32_t _holeMode{0};
    bool _heightUnorm{false};
    bool _castShadow{true};
    bool _receiveShadow{true};
    bool _materialsDirty{false};
};

} // namespace landscape
} // namespace cc

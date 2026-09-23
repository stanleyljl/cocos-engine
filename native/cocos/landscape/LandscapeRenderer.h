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
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "core/TypedArray.h"
#include "landscape/Landscape.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/VTPaging.h"
#include "landscape/TilePagePool.h"
#include "math/Vec3.h"
#include "math/Vec4.h"
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

class MaterialLibrary;
class VTRenderer;

/**
 * Coordinates the frame plan, source residency, terrain models and 3D decals.
 * The quadtree owns visibility/LOD; VTRequestPlan owns material-page budgeting;
 * TilePageResolver owns source lookup. Four shared grids split selected node
 * quadrants into material-page patches without adding triangles.
 */
class LandscapeRenderer {
public:
    LandscapeRenderer();
    ~LandscapeRenderer();

    bool init(Node *node, scene::RenderScene *scene);
    void destroy();
    bool valid() const;
    // Latched after the initial view's source and composed pages reach target LOD.
    bool isReady() const { return _ready; }

    void setLodRanges(const ccstd::vector<float> &morphStart,
                      const ccstd::vector<float> &morphEnd);
    bool setAsset(LandscapeAsset *asset);
    void setDebugData(const LandscapeDebugData &data);
    void preparePasses(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
    void collectPassModels(const ccstd::vector<QuadNode> &selected, const geometry::Frustum &frustum,
                           bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void onGlobalPipelineStateChanged();
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setViewPos(const Vec3 &position);
    RenderTexture *vtAtlas() const;

private:
    void sync(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
    void rebuildNodeModels();
    void setLodColor(bool enabled);
    void setShowRanges(bool enabled);
    void setWireframe(bool wireframe);
    void setFreezeLod(bool frozen);
    void setUnlit(bool enabled);
    void setHeightBlendEnabled(bool enabled);
    void setDecal3DEnabled(bool enabled);
    void setBakeNormalEnabled(bool enabled);
    void setCliffEnabled(bool enabled);

    struct Patch {
        QuadNode node;
        uint32_t x{0}; // cell offset inside the original 16 x 16 node
        uint32_t z{0};
        uint32_t meshIndex{0}; // 1, 2, 4 or 8 cells per side
        VTPageAddress page;
        float vtPriority{0.0F}; // visible footprint; ancestors must not inflate it
        bool needsMaterial{true}; // false for quadrants used exclusively by shadow passes
    };

    struct ModelState {
        IntrusivePtr<scene::Model> model;
        Patch patch;
    };

    struct DecalDraw {
        IntrusivePtr<scene::Model> model;
        Patch patch;
        uint32_t decal{0};
        Vec4 grid; // fixed landscape-local XZ origin and width/depth
    };
    // Keep each request and its resolved inputs together across source uploads.
    struct VTPageUpdate {
        VTPageRequest request;
        VTPageInputs inputs;
    };
    // Frame stages: selection -> residency/source publication -> models -> decals.
    void buildFramePlan(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
    bool syncPageSources(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes, bool uploadsPolled);
    void prepareVTPageUpdates();
    void refreshVTPageInputs();
    void queueVTPageUpdates();
    bool initialDataReady() const;
    void syncTerrainModels();
    void initDecalCenters();
    void bindRuntimeTextures();
    void syncDecals(const ccstd::vector<Patch> &patches);
    void updateDecalInstance(const DecalDraw &draw);
    void appendDecalDraw(const Patch &patch, uint32_t decal, const Vec4 &grid);
    IntrusivePtr<scene::Model> createModel(uint32_t meshIndex);
    void updateMaterialProperties();
    void updateMorphCameraProperty();
    void setInstanceAttribute(scene::Model *model, const char *name, const Vec4 &value);
    void updateInstanceData(scene::Model *model, const Patch &patch);
    void updateModel(ModelState &state, const Patch &patch);
    void updateModelBounds(scene::Model *model, const Patch &patch);
    void selectPatches(const QuadNode &node, uint32_t x, uint32_t z, uint32_t meshIndex,
                       ccstd::vector<Patch> &patches);
    float patchDistance(const QuadNode &node, float x, float z, float size, bool farthest) const;

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    std::array<IntrusivePtr<RenderingSubMesh>, 4> _meshes;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;
    IntrusivePtr<Material> _decalSolid;
    IntrusivePtr<Material> _decalWire;
    IntrusivePtr<RenderingSubMesh> _decalMesh;
    ccstd::vector<DecalDraw> _decalDraws;
    size_t _decalActive{0};
    ccstd::vector<Vec3> _decalCenters;
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<TilePagePool> _tilePages;
    std::unique_ptr<MaterialLibrary> _materialLibrary;
    std::unique_ptr<VTRenderer> _vtRenderer;

    ccstd::unordered_map<uint64_t, ModelState> _active;
    struct NodeModel {
        uint8_t quadrantMask;
        scene::Model *model;
    };
    ccstd::unordered_map<uint64_t, ccstd::vector<NodeModel>> _nodeModels;
    std::array<ccstd::vector<IntrusivePtr<scene::Model>>, 4> _pool;
    uint32_t _vtRootLevel{0};
    LandscapeSyncCache _syncCache;
    // Reused across active frames; the stable-camera fast path touches none of these.
    VTRequestPlan _requestPlan;
    ccstd::vector<Patch> _patches;
    ccstd::vector<QuadNode> _shadowOnlyNodes;
    ccstd::vector<VTPageUpdate> _vtPageUpdates;
    ccstd::unordered_set<uint64_t> _visiblePatches;
    std::unique_ptr<TilePageResolver> _sourceResolver;

    // Stored as TypedArray to avoid both per-update ArrayBuffers and implicit
    // Float32Array-to-TypedArray copies when setting instance attributes.
    TypedArray _instanceAttributeScratch;

    LandscapeData _data;
    float _heightSampleSpacing{1.0F};
    Vec3 _viewPosition;
    Vec3 _vtViewPosition;
    ccstd::vector<float> _morphStart;
    ccstd::vector<float> _morphEnd;
    LandscapeDebugData _debugData;
    bool _castShadow{true};
    bool _receiveShadow{true};
    bool _ready{false};
    bool _initializationFailed{false};
};

} // namespace landscape
} // namespace cc

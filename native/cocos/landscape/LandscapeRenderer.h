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

#include <algorithm>
#include <array>
#include <memory>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "core/TypedArray.h"
#include "landscape/DecalRenderer.h"
#include "landscape/Landscape.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTPaging.h"
#include "math/Vec3.h"
#include "math/Vec4.h"
#include "scene/Model.h"

#include "landscape/Quadtree.h"

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
 * Coordinates the frame plan, source residency and terrain models.
 * The quadtree owns visibility/LOD; VirtualTexture owns material-page residency;
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
    // Latched after the initial view settles within budget with all exact bindings ready.
    bool isReady() const { return _ready; }

    void setLodRanges(const ccstd::vector<float> &morphStart,
                      const ccstd::vector<float> &morphEnd);
    bool setAsset(LandscapeAsset *asset);
    void setDebugData(const LandscapeDebugData &data);
    void preparePasses(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
    void collectPassModels(const geometry::Frustum &frustum,
                           bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void onGlobalPipelineStateChanged();
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setViewPos(const Vec3 &position);
    RenderTexture *vtAtlas() const;

private:
    friend struct LandscapePagingTestAccess;

    // A camera/pass change must not replenish any per-frame streaming budget.
    // The time limit is checked between indivisible steps; allow the first step
    // to make progress even if source uploads/selection have used the time slice.
    struct StreamingBudget {
        bool begin(uint64_t frame, double nowMs) {
            if (valid && stamp == frame) {
                return false;
            }
            valid = true;
            stamp = frame;
            deadlineMs = nowMs + config::STREAMING_TIME_BUDGET_MS;
            pages = config::VT_PAGE_UPDATE_BUDGET;
            attempts = config::STREAMING_ADMISSION_BUDGET;
            steps = config::STREAMING_STEP_BUDGET;
            return true;
        }
        bool nextStep(double nowMs) {
            if (steps == 0 || (steps != config::STREAMING_STEP_BUDGET && nowMs >= deadlineMs)) {
                return false;
            }
            --steps;
            return true;
        }
        uint32_t pages{0}, attempts{0}, steps{0};
        uint64_t stamp{0};
        double deadlineMs{0};
        bool valid{false};
    };

    // Skip CPU reconstruction only when both selection and streamed content are
    // unchanged. Publishing completed pages must refresh direct instance mappings,
    // even when the camera is stationary.
    class SyncCache {
    public:
        using Positions = std::array<float, 6>; // selection camera and terrain origin

        bool matches(const Positions &positions, uint64_t tileRevision, uint64_t contentRevision,
                     const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) const {
            return _valid && positions == _positions && tileRevision == _tileRevision &&
                   contentRevision == _contentRevision && sameNodes(geometryNodes, _geometryNodes) && sameNodes(surfaceNodes, _surfaceNodes);
        }

        void store(const Positions &positions, uint64_t tileRevision, uint64_t contentRevision,
                   const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
            _positions = positions;
            _tileRevision = tileRevision;
            _contentRevision = contentRevision;
            _geometryNodes = geometryNodes;
            _surfaceNodes = surfaceNodes;
            _valid = true;
        }

        void invalidate() { _valid = false; }

    private:
        static bool sameNodes(const ccstd::vector<QuadNode> &a, const ccstd::vector<QuadNode> &b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const QuadNode &left, const QuadNode &right) {
                       return left.level == right.level && left.ix == right.ix && left.iz == right.iz &&
                              left.minY == right.minY && left.maxY == right.maxY && left.quadrantMask == right.quadrantMask;
                   });
        }

        bool _valid{false};
        Positions _positions{};
        uint64_t _tileRevision{0};
        uint64_t _contentRevision{0};
        ccstd::vector<QuadNode> _geometryNodes;
        ccstd::vector<QuadNode> _surfaceNodes;
    };

    void sync(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
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
        float vtPriority{0.0F};   // visible footprint; ancestors must not inflate it
        bool needsMaterial{true}; // false for quadrants used exclusively by shadow passes
    };

    struct ModelState {
        IntrusivePtr<scene::Model> model;
        Patch patch;
    };

    // Frame stages: selection -> residency/source publication -> models -> decals.
    QuadNode drawNode(NodeAddress node) const;
    void buildPatches(NodeAddress node, uint32_t bias, ccstd::vector<Patch> &patches);
    bool stage(const RenderSelection::Change &change, bool geometry, bool roots = false);
    bool planResources(bool reserve);
    void resetPlanning();
    bool planNext(uint32_t &attempts);
    void commitPending();
    void updateReferences(uint64_t key, const ccstd::vector<Patch> &patches, bool add);
    bool colorRegion(NodeAddress node) const;
    void syncTerrainModels();
    void bindRuntimeTextures();
    void syncDecals(const ccstd::vector<Patch> &patches);
    IntrusivePtr<scene::Model> createModel(uint32_t meshIndex);
    void updateMaterialProperties();
    void updateMorphCameraProperty();
    LandscapeSurfaceInstance resolveSurface(const Patch &patch);
    std::array<Material *, 4> surfaceMaterials() const;
    void updateInstanceData(scene::Model *model, const Patch &patch);
    void updateModel(ModelState &state, const Patch &patch);
    void updateModelBounds(scene::Model *model, const Patch &patch);
    void selectPatches(const QuadNode &node, uint32_t x, uint32_t z, uint32_t meshIndex,
                       ccstd::vector<Patch> &patches);
    LandscapeLocalRegion patchRegion(const Patch &patch) const;
    float patchDistance(const QuadNode &node, float x, float z, float size, bool farthest) const;

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    std::array<IntrusivePtr<RenderingSubMesh>, 4> _meshes;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;
    std::unique_ptr<DecalRenderer> _decals;
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<TilePagePool> _tilePages;
    std::unique_ptr<MaterialLibrary> _materialLibrary;
    std::unique_ptr<VTRenderer> _vtRenderer;

    ccstd::unordered_map<uint64_t, ModelState> _active;
    struct NodeModel {
        uint8_t quadrantMask;
        scene::Model *model;
        uint64_t patchKey;
    };
    ccstd::unordered_map<uint64_t, ccstd::vector<NodeModel>> _nodeModels;
    std::array<ccstd::vector<IntrusivePtr<scene::Model>>, 4> _pool;
    VTPageLayout _vtLayout;
    SyncCache _syncCache, _targetCache, _selectionCache;
    StreamingBudget _streamingBudget;
    RenderSelection _selection;
    RenderSelection::Change _change;
    ccstd::unordered_map<uint64_t, ccstd::vector<Patch>> _draws, _pendingDraws;
    ccstd::unordered_map<uint64_t, uint32_t> _heightRefs, _pageRefs;
    ccstd::unordered_set<uint64_t> _dirtyNodes, _colorPaths, _colorSelected;
    ccstd::vector<RenderSelection::Request> _requests;
    ccstd::vector<NodeAddress> _materialNodes, _workingSources;
    ccstd::vector<VTPageAddress> _workingPages;
    size_t _requestIndex{0}, _materialIndex{0};
    uint32_t _materialBias{0};
    bool _pending{false}, _reserved{false}, _settled{false}, _initialized{false};
    bool _changingGeometry{false}, _planningInvalid{false};
    bool _preferMaterial{true};
    uint64_t _preparedPageRevision{0};
    // Reused across active frames; the stable-camera fast path touches none of these.
    ccstd::vector<Patch> _patches;
    std::unique_ptr<TilePageResolver> _sourceResolver;

    // Stored as TypedArray to avoid both per-update ArrayBuffers and implicit
    // Float32Array-to-TypedArray copies when setting instance attributes.
    TypedArray _instanceAttributeScratch;

    LandscapeData _data;
    float _heightSampleSpacing{1.0F};
    Vec3 _viewPosition;
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

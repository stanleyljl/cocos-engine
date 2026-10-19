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
#include <optional>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "core/TypedArray.h"
#include "landscape/DecalRenderer.h"
#include "landscape/Landscape.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/TilePagePool.h"
#include "landscape/VirtualTexture.h"
#include "math/Vec3.h"
#include "math/Vec4.h"
#include "scene/Model.h"

#include "landscape/Quadtree.h"

namespace cc {

class Node;
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
 * TilePageResolver owns source lookup. Each committed node draws one shared
 * 16 x 16 grid and selects material pages through a local mapping texture.
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
    void setHoleMode(uint32_t mode);
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);
    void setGlobalColorStrength(float strength);
    void setGlobalColorMap(Texture2D *texture);
    void setViewPos(const Vec3 &position);
    TextureBase *vtAtlas() const;
    LandscapeDrawCalls getDrawCalls(uint32_t frame) const;
    void estimateDrawCalls(const ccstd::vector<const scene::Model *> &models);

private:
    friend struct LandscapePagingTestAccess;
    uint32_t _holeMode{0};

    // Skip CPU reconstruction only when both selection and streamed content are
    // unchanged. Publishing completed pages must refresh local grid mappings,
    // even when the camera is stationary.
    class SyncCache {
    public:
        using Positions = std::array<float, 6>; // selection camera and terrain origin

        bool matches(const Positions &positions, uint64_t tileRevision, uint64_t contentRevision,
                     const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) const {
            return _frameValid && matchesPosition(positions) && tileRevision == _tileRevision && contentRevision == _contentRevision &&
                   matchesSelection(geometryNodes, surfaceNodes);
        }

        void store(const Positions &positions, uint64_t tileRevision, uint64_t contentRevision,
                   const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) {
            _positions = positions;
            _tileRevision = tileRevision;
            _contentRevision = contentRevision;
            _geometryNodes = geometryNodes;
            _surfaceNodes = surfaceNodes;
            _frameValid = true;
            _positionValid = true;
            _selectionValid = true;
        }

        bool matchesPosition(const Positions &positions) const {
            return _positionValid && positions == _positions;
        }

        bool matchesSelection(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes) const {
            return _selectionValid && sameNodes(geometryNodes, _geometryNodes) && sameNodes(surfaceNodes, _surfaceNodes);
        }

        void invalidate() { _frameValid = false; }

        void invalidatePosition() {
            _frameValid = false;
            _positionValid = false;
        }

        void clear() {
            _frameValid = false;
            _positionValid = false;
            _selectionValid = false;
        }

    private:
        static bool sameNodes(const ccstd::vector<QuadNode> &a, const ccstd::vector<QuadNode> &b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const QuadNode &left, const QuadNode &right) {
                       return left.level == right.level && left.ix == right.ix && left.iz == right.iz &&
                              left.minY == right.minY && left.maxY == right.maxY && left.quadrantMask == right.quadrantMask;
                   });
        }

        bool _frameValid{false};
        bool _positionValid{false};
        bool _selectionValid{false};
        Positions _positions{};
        uint64_t _tileRevision{0};
        uint64_t _contentRevision{0};
        ccstd::vector<QuadNode> _geometryNodes;
        ccstd::vector<QuadNode> _surfaceNodes;
    };

    void sync(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes);
    void updateSelection(const ccstd::vector<QuadNode> &geometryNodes, const ccstd::vector<QuadNode> &surfaceNodes,
                         const SyncCache::Positions &positions);
    void streamResources();
    void setLodColor(bool enabled);
    void setShowRanges(bool enabled);
    void setWireframe(bool wireframe);
    void setFreezeLod(bool frozen);
    void setUnlit(bool enabled);
    void setHeightBlendEnabled(bool enabled);
    void setDecal3DEnabled(bool enabled);
    void setBakeNormalEnabled(bool enabled);
    void setCliffEnabled(bool enabled);

    struct GridDraw {
        QuadNode node;
        VTGridMapping material;
        int mapping{-1};
        // Pending draws contain only the plan; committed draws own GPU instances.
        IntrusivePtr<scene::Model> model;
    };

    enum class ChangeKind {
        INITIAL,
        GEOMETRY,
        MATERIAL,
    };
    // Geometry candidates replace selected nodes; material candidates only
    // replace bindings on existing nodes and leave selection empty.
    // Discarding it never changes the displayed models or resource references.
    struct PendingChange {
        RenderSelection::Change selection;
        ccstd::unordered_map<uint64_t, GridDraw> draws;
        ChangeKind kind{ChangeKind::INITIAL};
        bool empty() const {
            return selection.empty() && draws.empty();
        }
    };
    struct ResourcePlan {
        // Deduplicated here, before either resource pool validates or reserves them.
        ccstd::vector<NodeAddress> sources;
        ccstd::vector<VTPageAddress> pages;
    };

    // Frame stages: selection -> residency/source publication -> models -> decals.
    QuadNode drawNode(NodeAddress node) const;
    bool stage(const RenderSelection::Change &change, ChangeKind kind);
    bool stageMaterial(NodeAddress node);
    bool planResources(ResourcePlan &plan) const;
    bool reserveResources();
    bool reserveResources(const ResourcePlan &plan);
    void resetPlanning();
    void planNext();
    void commitPending();
    void clearPending();
    bool updateResources(bool synchronous);
    bool colorRegion(NodeAddress node) const;
    void stopLoading();
    void syncTerrainModels();
    void bindRuntimeTextures();
    void recycleModel(GridDraw &draw);
    IntrusivePtr<scene::Model> createModel();
    void updateMaterialProperties();
    void refreshTerrainMaterials();
    void updateMorphCameraProperty();
    LandscapeSurfaceInstance resolveSurface(const GridDraw &draw);
    std::array<Material *, 4> surfaceMaterials() const;
    void updateInstanceData(scene::Model *model, const GridDraw &draw);
    void updateModel(GridDraw &draw);
    void updateModelBounds(scene::Model *model, const GridDraw &draw);
    float regionDistance(const QuadNode &node, float x, float z, float size) const;

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;
    std::unique_ptr<DecalRenderer> _decals;
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<TilePagePool> _tilePages;
    std::unique_ptr<MaterialLibrary> _materialLibrary;
    std::unique_ptr<VTRenderer> _vtRenderer;

    ccstd::vector<IntrusivePtr<scene::Model>> _pool;
    VTPageLayout _vtLayout;
    SyncCache _syncCache;
    // All cameras share one VT composition allowance per frame.
    std::optional<uint32_t> _streamingFrame;
    uint32_t _remainingVTPages{0};
    RenderSelection _selection;
    PendingChange _pending;
    ccstd::unordered_map<uint64_t, GridDraw> _draws;
    ccstd::unordered_set<uint64_t> _dirtyNodes;
    ccstd::unordered_set<uint64_t> _colorPaths;
    ccstd::unordered_set<uint64_t> _colorSelected;
    ccstd::vector<RenderSelection::Request> _requests;
    ccstd::vector<NodeAddress> _materialNodes;
    size_t _requestIndex{0};
    size_t _materialIndex{0};
    bool _settled{false};
    bool _initialized{false};
    bool _planningInvalid{false};
    bool _preferMaterial{true};
    std::optional<uint64_t> _reservedPageRevision;
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
    bool _loadingFailed{false};
    uint32_t _drawCallFrame{0};
    LandscapeDrawCalls _drawCalls;
};

} // namespace landscape
} // namespace cc

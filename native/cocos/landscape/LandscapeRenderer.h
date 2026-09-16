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
#include <array>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "core/TypedArray.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/LandscapeAsset.h"
#include "landscape/VTPaging.h"
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

class TilePagePool;
class MaterialLibrary;
class VTRenderer;

/**
 * The quadtree owns visibility and LOD selection. This class only turns the
 * selected node quadrants into material-page patches without adding triangles.
 * Four shared grids batch patches while VT residency follows its own hierarchy.
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
    void setUnlit(bool enabled);
    void setGlobalColorStrength(float strength);
    void setFreezeLod(bool frozen);
    void setViewPos(const Vec3 &position);
    RenderTexture *debugAtlas() const;

private:
    struct TilePage {
        uint32_t level{0};
        uint32_t x{0};
        uint32_t z{0};
        int layer{-1};
    };

    struct Patch {
        QuadNode node;
        uint32_t x{0}; // cell offset inside the original 16 x 16 node
        uint32_t z{0};
        uint32_t meshIndex{0}; // 1, 2, 4 or 8 cells per side
        VTPageAddress page;
    };

    struct ModelState {
        IntrusivePtr<scene::Model> model;
        Patch patch;
    };

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
    TilePage resolveTilePage(const QuadNode &node);
    TilePage resolveVTSource(const VTPageAddress &page);
    Vec4 tileParams(const TilePage &tile) const;
    int resolveVTPage(VTPageAddress page) const;

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    std::array<IntrusivePtr<RenderingSubMesh>, 4> _meshes;
    IntrusivePtr<Material> _materialSolid;
    IntrusivePtr<Material> _materialWire;
    IntrusivePtr<LandscapeAsset> _asset;
    std::unique_ptr<TilePagePool> _tilePages;
    std::unique_ptr<MaterialLibrary> _materialLibrary;
    std::unique_ptr<VTRenderer> _vtRenderer;

    ccstd::unordered_map<uint64_t, ModelState> _active;
    std::array<ccstd::vector<IntrusivePtr<scene::Model>>, 4> _pool;
    uint32_t _vtRootLevel{0};

    // Stored as TypedArray to avoid both per-update ArrayBuffers and implicit
    // Float32Array-to-TypedArray copies when setting instance attributes.
    TypedArray _instanceAttributeScratch;

    LandscapeData _data;
    float _heightSampleSpacing{1.0F};
    Vec3 _viewPosition;
    Vec3 _vtViewPosition;
    ccstd::vector<float> _morphStart;
    ccstd::vector<float> _morphEnd;
    bool _lodColor{false};
    bool _showRanges{false};
    bool _wireframe{false};
    bool _unlit{false};
    bool _freezeLod{false};
};

} // namespace landscape
} // namespace cc

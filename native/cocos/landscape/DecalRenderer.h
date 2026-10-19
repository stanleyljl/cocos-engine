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
#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "core/TypedArray.h"
#include "landscape/Quadtree.h"
#include "landscape/LandscapeAsset.h"
#include "math/Vec4.h"
#include "scene/Model.h"

namespace cc {
class Material;
class Node;
class RenderingSubMesh;
namespace gfx {
class Device;
class Texture;
} // namespace gfx
namespace scene {
class RenderScene;
}
namespace landscape {
class MaterialLibrary;

// Resolved terrain sources shared by terrain and displaced decal instances.
struct LandscapeSurfaceInstance {
    Vec4 grid;
    Vec4 tile;
    Vec4 mapping; // local table slot, committed edge mask, unused ZW
    void apply(scene::Model *model, TypedArray &scratch) const;
};

// Owns decal selection, VT composition inputs and displaced geometry.
// LandscapeRenderer supplies resolved terrain nodes; VTRenderer schedules
// page composition. Neither residency nor quadtree selection belongs here.
class DecalRenderer {
public:
    struct TerrainNode {
        uint64_t nodeKey{0};
        LandscapeRegion region;
        float minY{0};
        float maxY{0};
        LandscapeSurfaceInstance surface;
    };

    DecalRenderer();
    ~DecalRenderer();
    DecalRenderer(const DecalRenderer &) = delete;
    DecalRenderer &operator=(const DecalRenderer &) = delete;

    bool init(Node *node, scene::RenderScene *scene, bool heightUnorm);
    bool setAsset(LandscapeAsset *asset, gfx::Device *device);
    // Only shared terrain shader uniforms/textures/variants are set by the caller.
    std::array<Material *, 2> materials() const;

    bool bindComposeMaterial(Material *material, const MaterialLibrary &materials);
    uint32_t collectPageDecals(uint32_t slot, const Vec4 &region);
    void uploadPageDecals();

    // Committed terrain changes update only the affected node's bindings.
    void updateNode(const TerrainNode &node);
    void removeNode(uint64_t nodeKey);
    // Camera motion changes visibility/fade, never the fixed decal lattice.
    void updateView(const Vec3 &viewPosition);
    void refreshInstances(const Vec3 &viewPosition);
    void preparePasses(uint32_t stamp);
    void collectPassModels(const QuadNode &node, const geometry::Frustum &frustum,
                           bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void onGlobalPipelineStateChanged();
    void setEnabled(bool enabled);
    void setWireframe(bool wireframe);
    // Rebind the selected material and restore instance attributes after variant changes.
    void refreshMaterials();
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);

private:
    struct Draw {
        IntrusivePtr<scene::Model> model;
        uint32_t decal{0};
        Vec4 grid;
    };
    struct Instance {
        Vec3 center;
        float fade{0};
        bool active{false};
        int firstX{0};
        int firstZ{0};
        int columns{0};
        ccstd::vector<Vec4> grids;
    };
    // Draw ownership changes only when the committed terrain cut changes.
    struct NodeDraws {
        TerrainNode terrain;
        ccstd::vector<Draw> draws;
    };
    void updateInstance(const Draw &draw, const TerrainNode &node);
    void updateBinding(const Draw &draw, const TerrainNode &node, bool boundsChanged);
    void appendDecal(NodeDraws &node, uint32_t decal);
    void recycle(Draw &draw);

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    IntrusivePtr<Material> _solid;
    IntrusivePtr<Material> _wire;
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<gfx::Texture> _pageIndexTexture;
    ccstd::vector<uint8_t> _pageIndexData; // two bytes per instance index, low byte first
    ccstd::vector<Instance> _instances;
    ccstd::unordered_map<uint64_t, NodeDraws> _nodes;
    ccstd::vector<IntrusivePtr<scene::Model>> _pool;
    TypedArray _scratch;
    Vec3 _viewPosition;
    bool _enabled{true};
    bool _wireframe{false};
    bool _castShadow{true};
    bool _receiveShadow{true};
};
} // namespace landscape
} // namespace cc

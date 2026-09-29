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
#include "landscape/LandscapeAsset.h"
#include "landscape/Quadtree.h"
#include "math/Vec4.h"
#include "scene/Model.h"

namespace cc {
class Material;
class Node;
class RenderingSubMesh;
namespace gfx { class Device; class Texture; }
namespace scene { class RenderScene; }
namespace landscape {
class MaterialLibrary;

// The terrain and displaced decals use the same shader and resolved sources.
// These helpers keep that shader contract in one place; they never request pages.
Material *createLandscapeMaterial(bool wireframe, bool decal, bool heightUnorm);

struct LandscapeSurfaceInstance {
    Vec4 grid;
    Vec4 quadrant;
    Vec4 tile;
    Vec4 normalParent;
    Vec4 vt;
    bool needsMaterial{true};
    void apply(scene::Model *model, TypedArray &scratch) const;
};

// Owns decal selection, VT composition inputs and displaced geometry.
// LandscapeRenderer supplies resolved terrain patches; VTRenderer schedules
// page composition. Neither residency nor quadtree selection belongs here.
class DecalRenderer {
public:
    struct TerrainPatch {
        uint64_t nodeKey{0};
        uint8_t quadrantMask{0};
        LandscapeLocalRegion region;
        Vec3 boundsMin;
        Vec3 boundsMax;
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

    // Stream resolved patches without allocating a temporary per-frame vector.
    // False means no eligible displaced decals; previous draws are disabled.
    bool beginSync(const Vec3 &viewPosition);
    void addPatch(const TerrainPatch &patch);
    void endSync();
    void refreshInstances(const Vec3 &viewPosition);
    void preparePasses(uint32_t stamp);
    void collectPassModels(const ccstd::vector<QuadNode> &selected, const geometry::Frustum &frustum,
                           bool shadow, ccstd::vector<const scene::Model *> &models) const;
    void onGlobalPipelineStateChanged();
    void setEnabled(bool enabled);
    void setWireframe(bool wireframe);
    void setCastShadow(bool enabled);
    void setReceiveShadow(bool enabled);

private:
    struct Draw {
        IntrusivePtr<scene::Model> model;
        size_t patch{0};
        uint32_t decal{0};
        Vec4 grid;
    };
    void updateInstance(const Draw &draw);
    void appendDraw(size_t patch, uint32_t decal, const Vec4 &grid);

    IntrusivePtr<Node> _node;
    scene::RenderScene *_scene{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    IntrusivePtr<Material> _solid;
    IntrusivePtr<Material> _wire;
    IntrusivePtr<RenderingSubMesh> _mesh;
    IntrusivePtr<gfx::Texture> _pageIndices;
    ccstd::vector<uint8_t> _pageIndexData;
    ccstd::vector<Vec3> _centers;
    ccstd::vector<TerrainPatch> _patches;
    ccstd::vector<Draw> _draws;
    ccstd::unordered_map<uint64_t, ccstd::vector<size_t>> _nodeDraws;
    std::array<uint32_t, config::DECAL_INSTANCE_MAX> _candidates{};
    size_t _candidateCount{0};
    size_t _active{0};
    TypedArray _scratch;
    Vec3 _viewPosition;
    bool _enabled{true};
    bool _wireframe{false};
    bool _castShadow{true};
    bool _receiveShadow{true};
};
} // namespace landscape
} // namespace cc

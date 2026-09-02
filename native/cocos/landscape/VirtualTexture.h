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

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"

namespace cc {

class Node;
class Material;
class RenderingSubMesh;
class RenderTexture;

namespace scene {
class RenderScene;
class Camera;
class Model;
} // namespace scene

namespace landscape {

/**
 * The VT physical atlas + its compose pass (M9b). The composed terrain surface
 * (albedo, from splat + material library) lives in one physical-atlas texture,
 * carved into VT_PAGE_COUNT square page slots. Each visible Node is assigned a
 * page slot (LRU); the terrain later samples its page instead of blending inline.
 *
 * Compose is a pixel-shader render-to-texture with NO shared-pipeline changes: the
 * atlas is a RenderTexture, and a dedicated orthographic "compose camera" (driven
 * by the default forward pipeline) renders one instanced quad per page slot into
 * it. The quads live on config::VT_COMPOSE_LAYER (rendered only by this camera,
 * cleared from the main camera), so they never appear on screen. The compose
 * fragment shader reads the slot's Node splat tile + the material library and
 * writes the composed albedo. Unused slots draw a degenerate (clipped) quad.
 *
 * Per frame the renderer calls beginFrame() -> acquirePage() per visible Node ->
 * commit(); the atlas is fully cleared and every in-use page is recomposed.
 */
class VirtualTexture {
public:
    VirtualTexture();
    ~VirtualTexture();

    // quadMesh: the shared [0,1] grid mesh (reused as the page quad).
    bool init(scene::RenderScene *scene, RenderingSubMesh *quadMesh);
    void destroy();

    inline bool valid() const { return _atlas != nullptr; }
    inline RenderTexture *atlas() const { return _atlas; }
    // The compose material, so the renderer can bind splatPages + materialAlbedo.
    inline Material *composeMaterial() const { return _material; }

    // --- Per-frame paging (main thread) ---
    // Each visible Node keeps a STABLE atlas page slot while it stays visible, so
    // its page never moves frame-to-frame (moving the slot each frame — e.g. from
    // the quadtree's unordered selection order — makes tiles flicker). A Node's
    // slot is freed only when it leaves. Every in-use page is recomposed each frame.
    void beginFrame();
    // Assigns (or keeps) this Node's page slot and records its compose params
    // (splat layer + UV remap + node world size for material tiling). Returns the
    // slot, or -1 if the atlas is full.
    int acquirePage(uint64_t key, int splatLayer, float uvScale, float uvOffX, float uvOffZ, float nodeWorldSize);
    // Frees slots whose Node was not requested this frame, then pushes per-slot
    // instance data to the compose sub-models (in-use slots recompose, rest degenerate).
    void commit();

private:
    struct PageParams {
        int splatLayer{0};
        float uvScale{1.0F};
        float uvOffX{0.0F};
        float uvOffZ{0.0F};
        float nodeWorldSize{1.0F};
    };

    int acquireSlot(); // lowest free slot, or -1 if none

    scene::RenderScene *_scene{nullptr}; // weak; owned by the scene graph
    IntrusivePtr<RenderTexture> _atlas;
    IntrusivePtr<Node> _cameraNode;
    IntrusivePtr<scene::Camera> _camera;
    IntrusivePtr<Node> _quadNode;
    IntrusivePtr<scene::Model> _model;
    IntrusivePtr<Material> _material;

    // Stable slot assignment: slot -> node key (INVALID = free), key -> slot,
    // + which keys were requested this frame, + per-slot compose params.
    ccstd::vector<uint64_t> _slotKey;
    ccstd::unordered_map<uint64_t, int> _keyToSlot;
    ccstd::unordered_set<uint64_t> _inUse;
    ccstd::vector<PageParams> _slotParams;
    bool _warnedFull{false};
};

} // namespace landscape
} // namespace cc

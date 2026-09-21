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

#include <array>

#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "math/Vec4.h"
#include "landscape/LandscapeConfig.h"
#include "landscape/VTPaging.h"

namespace cc {
class RenderTexture;
namespace gfx {
class Device;
class Texture;
class Sampler;
class Framebuffer;
class RenderPass;
}
namespace landscape {

// Owns VT storage and residency only. No cameras, models, materials or draws.
// Pages use a material quadtree independent of the selected geometry LOD.
class VirtualTexture {
public:
    struct Page {
        uint64_t key{0};
        uint64_t lastUsed{0};
        float priority{0.0F};
        VTPageInputs inputs;
        bool occupied{false};
        bool dirty{true};
        bool permanent{false}; // Sector roots never enter the eviction candidates.
    };

    VirtualTexture();
    ~VirtualTexture();
    VirtualTexture(const VirtualTexture &) = delete;
    VirtualTexture &operator=(const VirtualTexture &) = delete;

    bool init(gfx::Device *device);
    void destroy();
    bool valid() const;
    RenderTexture *atlas() const;
    gfx::Texture *albedo() const;
    gfx::Texture *normalRoughnessAO() const;
    gfx::Sampler *sampler() const { return _sampler; }
    gfx::Framebuffer *framebuffer() const;
    gfx::RenderPass *renderPass() const;
    gfx::Framebuffer *mipFramebuffer(uint32_t level) const { return _mips[level - 1].framebuffer.get(); }
    gfx::Texture *mipSourceAlbedo(uint32_t level) const { return _mips[level - 1].sourceAlbedo.get(); }
    gfx::Texture *mipSourceNormal(uint32_t level) const { return _mips[level - 1].sourceNormal.get(); }

    // Protect ALL requested keys before allocating anything, so selection order
    // cannot evict pages that another visible node requests later this frame.
    void beginFrame(const ccstd::vector<uint64_t> &keys);
    // Allocation only schedules composition. A dirty page cannot be sampled
    // until markRendered publishes its completed base level AND mip levels.
    int acquirePage(const VTPageRequest &request, const VTPageInputs &inputs, bool permanent = false);
    bool requested(uint64_t key) const { return _requestedPages.count(key) != 0; }
    int findPage(uint64_t key) const;
    int findReadyPage(uint64_t key) const;
    // The finest ready page still protected by this frame's request plan, or
    // the permanent sector root (composed during pass preparation before draws).
    int findReadyPageOrRoot(VTPageAddress desired, uint32_t rootLevel) const;
    void collectDirtyPages(ccstd::vector<uint32_t> &slots) const;
    void markRendered(const ccstd::vector<uint32_t> &slots);
    void invalidate();
    uint64_t contentRevision() const { return _contentRevision; }
    const Page &page(uint32_t slot) const { return _pages[slot]; }

private:
    int allocatePageSlot(uint64_t key);
    IntrusivePtr<RenderTexture> _atlas;
    struct MipTarget {
        IntrusivePtr<gfx::Texture> albedo;
        IntrusivePtr<gfx::Texture> normal;
        IntrusivePtr<gfx::Texture> sourceAlbedo;
        IntrusivePtr<gfx::Texture> sourceNormal;
        IntrusivePtr<gfx::Framebuffer> framebuffer;
    };
    std::array<MipTarget, config::VT_MIP_LEVELS - 1> _mips;
    gfx::Sampler *_sampler{nullptr}; // device-owned
    ccstd::vector<Page> _pages;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;
    ccstd::unordered_set<uint64_t> _requestedPages;
    ccstd::unordered_set<uint64_t> _resolvedThisFrame; // Sources refreshed by acquirePage this frame.
    uint64_t _frame{0};
    uint64_t _contentRevision{0};
    bool _warnedFull{false};
};
} // namespace landscape
} // namespace cc

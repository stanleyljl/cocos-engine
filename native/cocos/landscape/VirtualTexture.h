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
#include "math/Vec4.h"

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
// A page caches a complete CDLOD node; quadrant instances share its mapping.
class VirtualTexture {
public:
    struct Page {
        uint64_t key{0};
        uint64_t lastUsed{0};
        Vec4 region; // node-local XZ origin, size, unused
        Vec4 source; // source tile XZ origin, size, splat array layer
        bool occupied{false};
        bool dirty{true};
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

    // Protect ALL requested keys before allocating anything, so selection order
    // cannot evict pages that another visible node requests later this frame.
    void beginFrame(const ccstd::vector<uint64_t> &keys);
    int acquirePage(uint64_t key, const Vec4 &region, const Vec4 &source);
    void collectDirtyPages(ccstd::vector<uint32_t> &slots) const;
    void markRendered(const ccstd::vector<uint32_t> &slots);
    void invalidate();
    const Page &page(uint32_t slot) const { return _pages[slot]; }

private:
    IntrusivePtr<RenderTexture> _atlas;
    gfx::Sampler *_sampler{nullptr}; // device-owned
    ccstd::vector<Page> _pages;
    ccstd::unordered_map<uint64_t, uint32_t> _lookup;
    ccstd::unordered_set<uint64_t> _requested;
    uint64_t _frame{0};
    bool _warnedFull{false};
};
} // namespace landscape
} // namespace cc

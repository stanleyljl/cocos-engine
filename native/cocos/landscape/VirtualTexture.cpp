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
#include "landscape/VirtualTexture.h"

#include <limits>
#include <algorithm>
#include "base/Log.h"
#include "base/Macros.h"
#include "core/assets/RenderTexture.h"
#include "landscape/LandscapeConfig.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "scene/RenderWindow.h"

namespace cc {
namespace landscape {
namespace {
bool same(const Vec4 &a, const Vec4 &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
}

VirtualTexture::VirtualTexture() = default;
VirtualTexture::~VirtualTexture() { destroy(); }

bool VirtualTexture::init(gfx::Device *device) {
    CC_ASSERT(_atlas == nullptr);
    if (device == nullptr || device->getCapabilities().maxColorRenderTargets < 2 ||
        device->getCapabilities().maxTextureSize < config::VT_ATLAS_SIZE) {
        return false;
    }
    gfx::RenderPassInfo passInfo;
    gfx::ColorAttachment color;
    color.format = gfx::Format::RGBA8;
    color.loadOp = gfx::LoadOp::LOAD;
    color.storeOp = gfx::StoreOp::STORE;
    gfx::GeneralBarrierInfo barrier;
    barrier.prevAccesses = gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    barrier.nextAccesses = gfx::AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    color.barrier = device->getGeneralBarrier(barrier);
    passInfo.colorAttachments = {color, color};
    // No depth attachment: page rectangles never overlap.
    IRenderTextureCreateInfo info;
    info.name = "landscape-vt-atlas";
    info.width = config::VT_ATLAS_SIZE;
    info.height = config::VT_ATLAS_SIZE;
    info.passInfo = passInfo;
    info.colorMipLevels = config::VT_MIP_LEVELS;
    _atlas = ccnew RenderTexture();
    _atlas->initialize(info);
    const auto createMipView = [device](gfx::Texture *texture, uint32_t baseLevel, uint32_t levelCount) {
        gfx::TextureViewInfo view;
        view.texture = texture;
        view.format = texture->getFormat();
        view.baseLevel = baseLevel;
        view.levelCount = levelCount;
        return device->createTexture(view);
    };
    for (uint32_t level = 1; level < config::VT_MIP_LEVELS; ++level) {
        auto &mip = _mips[level - 1];
        mip.albedo = createMipView(albedo(), level, 1);
        mip.normal = createMipView(normalRoughnessAO(), level, 1);
        // Prefix views exclude the destination mip and preserve absolute LOD
        // numbering for texelFetch on Vulkan and GLES alike.
        mip.sourceAlbedo = createMipView(albedo(), 0, level);
        mip.sourceNormal = createMipView(normalRoughnessAO(), 0, level);
        mip.framebuffer = device->createFramebuffer({renderPass(), {mip.albedo, mip.normal}, nullptr});
    }
    gfx::SamplerInfo samplerInfo;
    samplerInfo.minFilter = gfx::Filter::LINEAR;
    samplerInfo.magFilter = gfx::Filter::LINEAR;
    samplerInfo.mipFilter = gfx::Filter::LINEAR;
    samplerInfo.addressU = gfx::Address::CLAMP;
    samplerInfo.addressV = gfx::Address::CLAMP;
    _sampler = device->getSampler(samplerInfo);
    _pages.resize(config::VT_PAGE_COUNT);
    return valid();
}

bool VirtualTexture::valid() const {
    return albedo() != nullptr && normalRoughnessAO() != nullptr &&
        std::all_of(_mips.begin(), _mips.end(), [](const MipTarget &mip) { return mip.framebuffer != nullptr; });
}
RenderTexture *VirtualTexture::atlas() const { return _atlas.get(); }
gfx::Framebuffer *VirtualTexture::framebuffer() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getFramebuffer() : nullptr;
}
gfx::RenderPass *VirtualTexture::renderPass() const {
    return framebuffer() ? framebuffer()->getRenderPass() : nullptr;
}
gfx::Texture *VirtualTexture::albedo() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getColorTexture(0) : nullptr;
}
gfx::Texture *VirtualTexture::normalRoughnessAO() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getColorTexture(1) : nullptr;
}

void VirtualTexture::beginFrame(const ccstd::vector<uint64_t> &keys) {
    ++_frame;
    _requestedPages.clear();
    _requestedPages.insert(keys.begin(), keys.end());
    _resolvedThisFrame.clear();
}

int VirtualTexture::findPage(uint64_t key) const {
    const auto found = _lookup.find(key);
    return found != _lookup.end() ? static_cast<int>(found->second) : -1;
}

int VirtualTexture::findReadyPage(uint64_t key) const {
    const int slot = findPage(key);
    return slot >= 0 && !_pages[slot].dirty ? slot : -1;
}

int VirtualTexture::findReadyPageOrRoot(VTPageAddress desired, uint32_t rootLevel) const {
    for (; desired.level < rootLevel; desired = desired.parent()) {
        const int slot = findReadyPage(desired.key());
        if (slot >= 0 && requested(desired.key())) return slot;
    }
    // Roots bootstrap startup/invalidation on the same queue before Base Pass.
    // Fine pages do not have that guarantee: never bind a dirty fine page.
    return findPage(desired.key());
}

int VirtualTexture::allocatePageSlot(uint64_t key) {
    int emptySlot = -1;
    int evictionSlot = -1;
    uint64_t oldestUse = std::numeric_limits<uint64_t>::max();
    for (uint32_t slot = 0; slot < _pages.size(); ++slot) {
        const auto &page = _pages[slot];
        if (!page.occupied) {
            if (emptySlot < 0) emptySlot = static_cast<int>(slot);
        } else if (!page.permanent && !requested(page.key) && page.lastUsed < oldestUse) {
            oldestUse = page.lastUsed;
            evictionSlot = static_cast<int>(slot);
        }
    }
    // Preserve the existing reuse policy: recycle unused content before growing
    // into an empty slot. This frame's requested pages and roots are protected.
    const int slot = evictionSlot >= 0 ? evictionSlot : emptySlot;
    if (slot < 0) {
        if (!_warnedFull) {
            CC_LOG_WARNING("[Landscape] VT cache full; using resident ancestor VT pages");
            _warnedFull = true;
        }
        return -1;
    }
    auto &page = _pages[slot];
    if (page.occupied) _lookup.erase(page.key);
    page = Page{};
    page.key = key;
    page.occupied = true;
    _lookup.emplace(key, static_cast<uint32_t>(slot));
    return slot;
}

int VirtualTexture::acquirePage(const VTPageRequest &request, const VTPageInputs &inputs, bool permanent) {
    if (!valid()) return -1;
    _requestedPages.insert(request.key);
    int slot = findPage(request.key);
    if (slot < 0) slot = allocatePageSlot(request.key);
    if (slot < 0) return -1;

    auto &page = _pages[slot];
    const auto &previous = page.inputs;
    const bool sameNormals = std::equal(previous.normalSources.begin(), previous.normalSources.end(),
                                        inputs.normalSources.begin(), same);
    page.dirty |= !same(previous.region, inputs.region) ||
                  !same(previous.splatSource, inputs.splatSource) || !sameNormals;
    page.inputs = inputs;
    page.permanent |= permanent;
    page.priority = request.priority;
    page.lastUsed = _frame;
    _resolvedThisFrame.insert(request.key);
    return slot;
}

void VirtualTexture::collectDirtyPages(ccstd::vector<uint32_t> &slots) const {
    slots.clear();
    for (uint32_t i = 0; i < _pages.size(); ++i) {
        const auto &p = _pages[i];
        // A protected ancestor may refer to a source tile that has since been
        // recycled. Only roots or pages with a freshly resolved source can bake.
        if (p.occupied && p.dirty && (p.permanent || _resolvedThisFrame.count(p.key) != 0)) slots.push_back(i);
    }
    std::sort(slots.begin(), slots.end(), [this](uint32_t a, uint32_t b) {
        const auto &pa = _pages[a];
        const auto &pb = _pages[b];
        if (pa.permanent != pb.permanent) return pa.permanent;
        return pa.priority != pb.priority ? pa.priority > pb.priority : pa.key < pb.key;
    });
    const size_t roots = static_cast<size_t>(std::count_if(slots.begin(), slots.end(),
        [this](uint32_t slot) { return _pages[slot].permanent; }));
    // Roots bootstrap the first frame and invalidation. Fine pages only become
    // sampleable after composition, so deferring them cannot expose empty data.
    if (slots.size() > roots + config::VT_PAGE_UPDATE_BUDGET) slots.resize(roots + config::VT_PAGE_UPDATE_BUDGET);
}
void VirtualTexture::markRendered(const ccstd::vector<uint32_t> &slots) {
    for (uint32_t slot : slots) _pages[slot].dirty = false;
    if (!slots.empty()) ++_contentRevision;
}
void VirtualTexture::invalidate() {
    for (auto &p : _pages) p.dirty = true;
    ++_contentRevision;
}
void VirtualTexture::destroy() {
    for (auto &mip : _mips) {
        mip.framebuffer = nullptr;
        mip.sourceAlbedo = nullptr;
        mip.sourceNormal = nullptr;
        mip.albedo = nullptr;
        mip.normal = nullptr;
    }
    if (_atlas) _atlas->destroy();
    _atlas = nullptr;
    _sampler = nullptr;
    _pages.clear();
    _lookup.clear();
    _requestedPages.clear();
    _resolvedThisFrame.clear();
    _frame = 0;
    _warnedFull = false;
}
} // namespace landscape
} // namespace cc

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
#include "base/Log.h"
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
    destroy();
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
    _atlas = ccnew RenderTexture();
    _atlas->initialize(info);
    gfx::SamplerInfo samplerInfo;
    samplerInfo.minFilter = gfx::Filter::LINEAR;
    samplerInfo.magFilter = gfx::Filter::LINEAR;
    samplerInfo.mipFilter = gfx::Filter::NONE;
    samplerInfo.addressU = gfx::Address::CLAMP;
    samplerInfo.addressV = gfx::Address::CLAMP;
    _sampler = device->getSampler(samplerInfo);
    _pages.resize(config::VT_PAGE_COUNT);
    return valid();
}

bool VirtualTexture::valid() const { return albedo() != nullptr && normalRoughnessAO() != nullptr; }
RenderTexture *VirtualTexture::atlas() const { return _atlas.get(); }
gfx::Framebuffer *VirtualTexture::framebuffer() const {
    return _atlas && _atlas->getWindow() ? _atlas->getWindow()->getFramebuffer() : nullptr;
}
gfx::RenderPass *VirtualTexture::renderPass() const {
    return framebuffer() ? framebuffer()->getRenderPass() : nullptr;
}
gfx::Texture *VirtualTexture::albedo() const {
    return framebuffer() ? framebuffer()->getColorTextures()[0] : nullptr;
}
gfx::Texture *VirtualTexture::normalRoughnessAO() const {
    return framebuffer() ? framebuffer()->getColorTextures()[1] : nullptr;
}

void VirtualTexture::beginFrame(const ccstd::vector<uint64_t> &keys) {
    ++_frame;
    _requested.clear();
    _requested.insert(keys.begin(), keys.end());
}

int VirtualTexture::acquirePage(uint64_t key, const Vec4 &region, const Vec4 &source) {
    if (!valid()) return -1;
    _requested.insert(key);
    auto found = _lookup.find(key);
    int slot = found != _lookup.end() ? static_cast<int>(found->second) : -1;
    if (slot < 0) {
        uint64_t oldest = std::numeric_limits<uint64_t>::max();
        for (uint32_t i = 0; i < _pages.size(); ++i) {
            const auto &p = _pages[i];
            if (!p.occupied) { slot = static_cast<int>(i); break; }
            if (_requested.count(p.key) == 0 && p.lastUsed < oldest) {
                oldest = p.lastUsed;
                slot = static_cast<int>(i);
            }
        }
        if (slot < 0) {
            if (!_warnedFull) {
                CC_LOG_WARNING("[Landscape] VT cache full; excess nodes use direct material shading");
                _warnedFull = true;
            }
            return -1;
        }
        auto &p = _pages[slot];
        if (p.occupied) _lookup.erase(p.key);
        p = Page{};
        p.key = key;
        p.occupied = true;
        _lookup.emplace(key, static_cast<uint32_t>(slot));
    }
    auto &p = _pages[slot];
    p.dirty |= !same(p.region, region) || !same(p.source, source);
    p.region = region;
    p.source = source;
    p.lastUsed = _frame;
    return slot;
}

void VirtualTexture::collectDirtyPages(ccstd::vector<uint32_t> &slots) const {
    slots.clear();
    for (uint32_t i = 0; i < _pages.size(); ++i) {
        const auto &p = _pages[i];
        if (p.occupied && p.dirty && _requested.count(p.key) != 0) slots.push_back(i);
    }
}
void VirtualTexture::markRendered(const ccstd::vector<uint32_t> &slots) {
    for (uint32_t slot : slots) _pages[slot].dirty = false;
}
void VirtualTexture::invalidate() {
    for (auto &p : _pages) p.dirty = true;
}
void VirtualTexture::destroy() {
    if (_atlas) _atlas->destroy();
    _atlas = nullptr;
    _sampler = nullptr;
    _pages.clear();
    _lookup.clear();
    _requested.clear();
    _frame = 0;
    _warnedFull = false;
}
} // namespace landscape
} // namespace cc

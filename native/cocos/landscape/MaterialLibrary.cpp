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

#include "landscape/MaterialLibrary.h"

#include <algorithm>

#include "base/Log.h"
#include "base/Ptr.h"
#include "landscape/LandscapeConfig.h"
#include "platform/FileUtils.h"
#include "platform/Image.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"

namespace cc {
namespace landscape {

namespace {
// Decodes an albedo image into a tightly packed RGBA8 buffer of res*res texels.
// Returns false if it cannot be read / is the wrong size. Expands RGB -> RGBA.
bool decodeAlbedo(const ccstd::string &path, uint32_t res, ccstd::vector<uint8_t> &out) {
    const Data data = FileUtils::getInstance()->getDataFromFile(path);
    if (data.isNull()) {
        return false;
    }
    IntrusivePtr<Image> image = ccnew Image();
    if (!image->initWithImageData(data.getBytes(), data.getSize())) {
        return false;
    }
    if (static_cast<uint32_t>(image->getWidth()) != res || static_cast<uint32_t>(image->getHeight()) != res) {
        CC_LOG_WARNING("[Landscape] material '%s' is %dx%d, expected %ux%u", path.c_str(), image->getWidth(), image->getHeight(), res, res);
        return false;
    }
    const gfx::Format fmt = image->getRenderFormat();
    const uint8_t *src = image->getData();
    const uint32_t count = res * res;
    out.resize(count * 4U);
    if (fmt == gfx::Format::RGBA8) {
        std::copy(src, src + count * 4U, out.begin());
    } else if (fmt == gfx::Format::RGB8) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i * 4U + 0U] = src[i * 3U + 0U];
            out[i * 4U + 1U] = src[i * 3U + 1U];
            out[i * 4U + 2U] = src[i * 3U + 2U];
            out[i * 4U + 3U] = 255U;
        }
    } else {
        CC_LOG_WARNING("[Landscape] material '%s' has unsupported format %d (need RGB8/RGBA8)", path.c_str(), static_cast<int>(fmt));
        return false;
    }
    return true;
}

// Box-downsamples an RGBA8 image to half size (into dst). Assumes even dims > 1.
void downsampleRGBA8(const uint8_t *src, uint32_t sw, uint32_t sh, ccstd::vector<uint8_t> &dst) {
    const uint32_t dw = sw >> 1;
    const uint32_t dh = sh >> 1;
    dst.resize(static_cast<size_t>(dw) * dh * 4U);
    for (uint32_t y = 0; y < dh; ++y) {
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t sx = x * 2U;
            const uint32_t sy = y * 2U;
            for (uint32_t c = 0; c < 4U; ++c) {
                const uint32_t sum = src[(sy * sw + sx) * 4U + c] +
                                     src[(sy * sw + sx + 1U) * 4U + c] +
                                     src[((sy + 1U) * sw + sx) * 4U + c] +
                                     src[((sy + 1U) * sw + sx + 1U) * 4U + c];
                dst[(y * dw + x) * 4U + c] = static_cast<uint8_t>(sum >> 2U);
            }
        }
    }
}
} // namespace

MaterialLibrary::MaterialLibrary() = default;

MaterialLibrary::~MaterialLibrary() {
    destroy();
}

bool MaterialLibrary::init(gfx::Device *device, const ccstd::vector<ccstd::string> &paths) {
    if (device == nullptr || paths.empty()) {
        return false;
    }
    _device = device;
    const uint32_t layerCount = std::min(static_cast<uint32_t>(paths.size()), config::MATERIAL_LIBRARY_MAX);

    // Resolution comes from the first readable image; all layers must match it.
    uint32_t res = 0;
    {
        const Data probe = FileUtils::getInstance()->getDataFromFile(paths[0]);
        IntrusivePtr<Image> image = ccnew Image();
        if (probe.isNull() || !image->initWithImageData(probe.getBytes(), probe.getSize())) {
            CC_LOG_WARNING("[Landscape] MaterialLibrary: cannot read first albedo '%s'", paths[0].c_str());
            _device = nullptr;
            return false;
        }
        res = static_cast<uint32_t>(image->getWidth());
        if (res == 0 || static_cast<uint32_t>(image->getHeight()) != res) {
            CC_LOG_WARNING("[Landscape] MaterialLibrary: first albedo must be square, got %dx%d", image->getWidth(), image->getHeight());
            _device = nullptr;
            return false;
        }
    }

    // Full mip chain so the compose pass can minify densely-tiled coarse pages
    // (a coarse Node tiles the material many times into its 128^2 page; without
    // mips that undersamples into aliasing/moire and looks different from finer LODs).
    uint32_t mipCount = 1;
    for (uint32_t r = res; r > 1U; r >>= 1U) {
        ++mipCount;
    }

    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D_ARRAY;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = gfx::Format::RGBA8;
    info.width = res;
    info.height = res;
    info.layerCount = layerCount;
    info.levelCount = mipCount;
    _array = _device->createTexture(info);
    if (_array == nullptr) {
        CC_LOG_WARNING("[Landscape] MaterialLibrary: failed to create RGBA8 TEX2D_ARRAY (%u layers @ %u, %u mips)", layerCount, res, mipCount);
        _device = nullptr;
        return false;
    }

    gfx::SamplerInfo si;
    si.minFilter = gfx::Filter::LINEAR;
    si.magFilter = gfx::Filter::LINEAR;
    si.mipFilter = gfx::Filter::LINEAR; // trilinear across the generated mips
    si.addressU = gfx::Address::WRAP;
    si.addressV = gfx::Address::WRAP;
    si.addressW = gfx::Address::CLAMP;
    _sampler = _device->getSampler(si);

    ccstd::vector<uint8_t> level;    // current mip level data
    ccstd::vector<uint8_t> next;     // scratch for the next (downsampled) level
    const ccstd::vector<uint8_t> fallback(static_cast<size_t>(res) * res * 4U, 128U); // neutral gray for unreadable layers
    _layerCount = layerCount;
    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        if (decodeAlbedo(paths[layer], res, level)) {
            // ok
        } else {
            CC_LOG_WARNING("[Landscape] MaterialLibrary: layer %u '%s' unreadable, using gray", layer, paths[layer].c_str());
            level = fallback;
        }
        uint32_t w = res;
        uint32_t h = res;
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            gfx::BufferTextureCopy region;
            region.texExtent.width = w;
            region.texExtent.height = h;
            region.texExtent.depth = 1;
            region.texSubres.mipLevel = mip;
            region.texSubres.baseArrayLayer = layer;
            region.texSubres.layerCount = 1;
            const uint8_t *buffers[1]{level.data()};
            _device->copyBuffersToTexture(buffers, _array, &region, 1);
            if (mip + 1 < mipCount) {
                downsampleRGBA8(level.data(), w, h, next);
                level.swap(next);
                w >>= 1U;
                h >>= 1U;
            }
        }
    }
    CC_LOG_INFO("[Landscape] MaterialLibrary: %u layers @ %ux%u, %u mips", layerCount, res, res, mipCount);
    return true;
}

void MaterialLibrary::destroy() {
    _array = nullptr; // IntrusivePtr releases the gfx texture
    _sampler = nullptr;
    _device = nullptr;
    _layerCount = 0;
}

} // namespace landscape
} // namespace cc

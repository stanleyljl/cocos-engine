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
#include <cmath>

#include "base/Log.h"
#include "landscape/LandscapeConfig.h"
#include "platform/FileUtils.h"
#include "platform/Image.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXTexture.h"

namespace cc {
namespace landscape {
namespace {

bool decodeRGBA8(const ccstd::string &path, uint32_t resolution, ccstd::vector<uint8_t> &pixels) {
    const Data data = FileUtils::getInstance()->getDataFromFile(path);
    IntrusivePtr<Image> image = ccnew Image();
    if (data.isNull() || !image->initWithImageData(data.getBytes(), data.getSize()) ||
        image->getRenderFormat() != gfx::Format::RGBA8 ||
        static_cast<uint32_t>(image->getWidth()) != resolution ||
        static_cast<uint32_t>(image->getHeight()) != resolution ||
        image->getDataLen() != static_cast<size_t>(resolution) * resolution * 4U) {
        CC_LOG_WARNING("[Landscape] expected %ux%u RGBA8 material PNG: %s", resolution, resolution, path.c_str());
        return false;
    }
    // Image's PNG decoder does not apply gamma or premultiply alpha. Alpha is
    // height/AO data, never opacity. Keep the complete base level unchanged.
    pixels.assign(image->getData(), image->getData() + image->getDataLen());
    return true;
}

float srgbToLinear(float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float value) {
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

uint8_t encode(float value) {
    return static_cast<uint8_t>(std::round(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

void downsample(const ccstd::vector<uint8_t> &src, uint32_t size, bool albedo, ccstd::vector<uint8_t> &dst, bool linearColor) {
    const uint32_t nextSize = std::max(1U, size / 2U);
    dst.resize(static_cast<size_t>(nextSize) * nextSize * 4U);
    for (uint32_t y = 0; y < nextSize; ++y) {
        for (uint32_t x = 0; x < nextSize; ++x) {
            float sum[4]{};
            float normal[3]{};
            uint32_t count = 0U;
            for (uint32_t sy = y * size / nextSize; sy < (y + 1U) * size / nextSize; ++sy) {
                for (uint32_t sx = x * size / nextSize; sx < (x + 1U) * size / nextSize; ++sx) {
                    const auto *pixel = src.data() + (static_cast<size_t>(sy) * size + sx) * 4U;
                    for (uint32_t c = 0; c < 4U; ++c) {
                        const float value = static_cast<float>(pixel[c]) / 255.0F;
                        sum[c] += albedo && !linearColor && c < 3U ? srgbToLinear(value) : value;
                    }
                    if (!albedo) {
                        const float nx = static_cast<float>(pixel[0]) / 127.5F - 1.0F;
                        const float ny = static_cast<float>(pixel[1]) / 127.5F - 1.0F;
                        normal[0] += nx;
                        normal[1] += ny;
                        normal[2] += std::sqrt(std::max(0.0F, 1.0F - nx * nx - ny * ny));
                    }
                    ++count;
                }
            }
            auto *pixel = dst.data() + (static_cast<size_t>(y) * nextSize + x) * 4U;
            for (uint32_t c = 0; c < 4U; ++c) {
                const float value = sum[c] / static_cast<float>(count);
                pixel[c] = encode(albedo && !linearColor && c < 3U ? linearToSrgb(value) : value);
            }
            if (!albedo) {
                const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                pixel[0] = encode(length > 1e-6F ? normal[0] / length * 0.5F + 0.5F : 0.5F);
                pixel[1] = encode(length > 1e-6F ? normal[1] / length * 0.5F + 0.5F : 0.5F);
            }
        }
    }
}

IntrusivePtr<gfx::Texture> createTexture(gfx::Device *device, uint32_t resolution, uint32_t layerCount,
                                       gfx::TextureType type = gfx::TextureType::TEX2D_ARRAY) {
    gfx::TextureInfo info;
    info.type = type;
    info.layerCount = layerCount;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    // Match builtin-standard: RGBA8 sampling, followed by shader RGB decoding.
    // Alpha remains linear height/AO data throughout filtering and shading.
    info.format = gfx::Format::RGBA8;
    info.width = resolution;
    info.height = resolution;
    info.levelCount = 1U;
    for (uint32_t size = resolution; size > 1U; size >>= 1U) {
        ++info.levelCount;
    }
    return device->createTexture(info);
}

void uploadTexture(gfx::Device *device, gfx::Texture *texture, uint32_t resolution,
                   uint32_t layer, ccstd::vector<uint8_t> pixels, bool albedo, bool linearColor = false) {
    ccstd::vector<uint8_t> next;
    uint32_t size = resolution;
    for (uint32_t mip = 0U; mip < texture->getInfo().levelCount; ++mip) {
        gfx::BufferTextureCopy region;
        region.texExtent = {size, size, 1U};
        region.texSubres.mipLevel = mip;
        region.texSubres.baseArrayLayer = layer;
        region.texSubres.layerCount = 1U;
        const uint8_t *buffers[]{pixels.data()};
        device->copyBuffersToTexture(buffers, texture, &region, 1U);
        if (mip + 1U < texture->getInfo().levelCount) {
            downsample(pixels, size, albedo, next, linearColor);
            pixels.swap(next);
            size = std::max(1U, size / 2U);
        }
    }
}
} // namespace

MaterialLibrary::MaterialLibrary() = default;
MaterialLibrary::~MaterialLibrary() { destroy(); }

bool MaterialLibrary::init(gfx::Device *device, const LandscapeAsset &asset) {
    destroy();
    const auto &layers = asset.materialLayers();
    if (device == nullptr || layers.empty()) {
        return false;
    }
    const uint32_t resolution = asset.materialResolution();
    const uint32_t count = static_cast<uint32_t>(layers.size());
    _albedoHeight = createTexture(device, resolution, count);
    _normalRoughnessAO = createTexture(device, resolution, count);
    if (!valid()) {
        destroy();
        return false;
    }
    _tilingParams.resize(config::MATERIAL_LIBRARY_MAX);
    for (const auto &layer : layers) {
        ccstd::vector<uint8_t> albedo, normal;
        if (!decodeRGBA8(layer.albedoHeight, resolution, albedo) ||
            !decodeRGBA8(layer.normalRoughnessAO, resolution, normal)) {
            destroy();
            return false;
        }
        // A layer's mean lets the global color replace its broad color while
        // retaining the tiled texture's fine contrast.
        double mean[3]{};
        const size_t pixelCount = albedo.size() / 4U;
        for (size_t pixel = 0U; pixel < pixelCount; ++pixel) {
            for (size_t c = 0U; c < 3U; ++c) {
                mean[c] += srgbToLinear(static_cast<float>(albedo[pixel * 4U + c]) / 255.0F);
            }
        }
        uploadTexture(device, _albedoHeight, resolution, layer.id, std::move(albedo), true);
        uploadTexture(device, _normalRoughnessAO, resolution, layer.id, std::move(normal), false);
        // The shader samples normalized UVs, so convert source pixels/m to repeats/m.
        const float uvScale = std::min(layer.pixelsPerMeter, static_cast<float>(config::VT_PAGE_INTERIOR)) /
                              static_cast<float>(resolution);
        float shaderMean[3];
        for (size_t c = 0U; c < 3U; ++c) {
            // Mips encode the linear average as sRGB; common/color/gamma uses
            // gamma * gamma when decoding. Match that shader convention here.
            const float encodedMean = linearToSrgb(static_cast<float>(mean[c] / pixelCount));
            shaderMean[c] = encodedMean * encodedMean;
        }
        _tilingParams[layer.id] = Vec4{uvScale, shaderMean[0], shaderMean[1], shaderMean[2]};
    }
    // Decal color is premultiplied linear RGB: mip filtering cannot introduce
    // dark borders through transparent texels. Height RG is a uint16 scalar.
    const auto &decalLayers = asset.decalLayers();
    const uint32_t decalSize = decalLayers.empty() ? 1U : asset.decalResolution();
    const uint32_t decalCount = std::max(1U, static_cast<uint32_t>(decalLayers.size()));
    _decalAlbedo = createTexture(device, decalSize, decalCount);
    _decalNormal = createTexture(device, decalSize, decalCount);
    if (!_decalAlbedo || !_decalNormal) return false;
    auto heightInfo = _decalAlbedo->getInfo();
    heightInfo.levelCount = 1;
    _decalHeight = device->createTexture(heightInfo);
    if (!_decalAlbedo || !_decalNormal || !_decalHeight) return false;
    for (uint32_t i = 0; i < decalCount; ++i) {
        ccstd::vector<uint8_t> color{0, 0, 0, 0}, normal{128, 128, 255, 255}, height{0, 0, 0, 255};
        if (!decalLayers.empty()) {
            const auto &d = decalLayers[i];
            if (!decodeRGBA8(d.albedoMask, decalSize, color) || !decodeRGBA8(d.normalRoughnessAO, decalSize, normal) ||
                !decodeRGBA8(d.height, decalSize, height)) return false;
        }
        for (size_t p = 0; p < color.size(); p += 4) {
            const float alpha = color[p + 3] / 255.0F;
            for (size_t c = 0; c < 3; ++c) color[p + c] = encode(srgbToLinear(color[p + c] / 255.0F) * alpha);
        }
        uploadTexture(device, _decalAlbedo, decalSize, i, std::move(color), true, true);
        uploadTexture(device, _decalNormal, decalSize, i, std::move(normal), false);
        uploadTexture(device, _decalHeight, decalSize, i, std::move(height), true, true);
    }
    const auto &global = asset.globalColorMap();
    uint32_t globalResolution = 1U;
    ccstd::vector<uint8_t> globalPixels{255, 255, 255, 255};
    if (!global.file.empty()) {
        if (decodeRGBA8(global.file, global.resolution, globalPixels)) {
            globalResolution = global.resolution;
            _globalColorStrength = global.strength;
            _hasGlobalColorMap = true;
        } else {
            CC_LOG_WARNING("[Landscape] global color map '%s' could not be loaded; using layer colors", global.file.c_str());
        }
    }
    _globalColorMap = createTexture(device, globalResolution, 1U, gfx::TextureType::TEX2D);
    if (!_globalColorMap) {
        destroy();
        return false;
    }
    uploadTexture(device, _globalColorMap, globalResolution, 0U, std::move(globalPixels), true);
    gfx::SamplerInfo info;
    info.minFilter = gfx::Filter::LINEAR;
    info.magFilter = gfx::Filter::LINEAR;
    info.mipFilter = gfx::Filter::LINEAR;
    info.addressU = gfx::Address::WRAP;
    info.addressV = gfx::Address::WRAP;
    _sampler = device->getSampler(info);
    info.addressU = gfx::Address::CLAMP;
    info.addressV = gfx::Address::CLAMP;
    _globalColorSampler = device->getSampler(info);
#if CC_LANDSCAPE_DEBUG
    CC_LOG_INFO("[Landscape] %u material layers: paired %ux%u RGBA8 arrays with mips",
                count, resolution, resolution);
#endif
    return true;
}

void MaterialLibrary::destroy() {
    _albedoHeight = nullptr;
    _normalRoughnessAO = nullptr;
    _globalColorMap = nullptr;
    _decalAlbedo = nullptr;
    _decalNormal = nullptr;
    _decalHeight = nullptr;
    _sampler = nullptr;
    _globalColorSampler = nullptr;
    _globalColorStrength = 0.0F;
    _hasGlobalColorMap = false;
    _tilingParams.clear();
}

} // namespace landscape
} // namespace cc

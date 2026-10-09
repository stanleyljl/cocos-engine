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
#include "base/Macros.h"
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
    // Keep the decoded bytes unchanged here. Each caller handles its own
    // RGB color space and Alpha meaning (height, AO or decal coverage).
    pixels.assign(image->getData(), image->getData() + image->getDataLen());
    return true;
}

float srgbToLinear(float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float value) {
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

uint8_t toByte(float value) {
    return static_cast<uint8_t>(std::round(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

void buildAlbedoHeightMip(const ccstd::vector<uint8_t> &source, uint32_t size, ccstd::vector<uint8_t> &destination) {
    // RGB is sRGB color; Alpha is linear material height. Average colors in
    // linear space, then encode RGB back to sRGB. Do not gamma-correct height.
    const uint32_t nextSize = std::max(1U, size / 2U);
    destination.resize(static_cast<size_t>(nextSize) * nextSize * 4U);
    for (uint32_t y = 0; y < nextSize; ++y) {
        for (uint32_t x = 0; x < nextSize; ++x) {
            // Integer ranges also include the last row/column of odd-sized images.
            const uint32_t beginY = y * size / nextSize;
            const uint32_t endY = (y + 1U) * size / nextSize;
            const uint32_t beginX = x * size / nextSize;
            const uint32_t endX = (x + 1U) * size / nextSize;
            float red = 0.0F, green = 0.0F, blue = 0.0F, height = 0.0F;
            for (uint32_t sourceY = beginY; sourceY < endY; ++sourceY) {
                for (uint32_t sourceX = beginX; sourceX < endX; ++sourceX) {
                    const uint8_t *pixel = source.data() + (static_cast<size_t>(sourceY) * size + sourceX) * 4U;
                    red += srgbToLinear(pixel[0] / 255.0F);
                    green += srgbToLinear(pixel[1] / 255.0F);
                    blue += srgbToLinear(pixel[2] / 255.0F);
                    height += pixel[3] / 255.0F;
                }
            }
            const float sampleCount = static_cast<float>((endY - beginY) * (endX - beginX));
            uint8_t *pixel = destination.data() + (static_cast<size_t>(y) * nextSize + x) * 4U;
            pixel[0] = toByte(linearToSrgb(red / sampleCount));
            pixel[1] = toByte(linearToSrgb(green / sampleCount));
            pixel[2] = toByte(linearToSrgb(blue / sampleCount));
            pixel[3] = toByte(height / sampleCount);
        }
    }
}

void buildNormalRoughnessAOMip(const ccstd::vector<uint8_t> &source, uint32_t size, ccstd::vector<uint8_t> &destination) {
    // RG encodes tangent-space normal XY. Reconstruct positive Z before
    // averaging and normalize the result. B (roughness) and A (AO) stay linear.
    const uint32_t nextSize = std::max(1U, size / 2U);
    destination.resize(static_cast<size_t>(nextSize) * nextSize * 4U);
    for (uint32_t y = 0; y < nextSize; ++y) {
        for (uint32_t x = 0; x < nextSize; ++x) {
            // Integer ranges also include the last row/column of odd-sized images.
            const uint32_t beginY = y * size / nextSize;
            const uint32_t endY = (y + 1U) * size / nextSize;
            const uint32_t beginX = x * size / nextSize;
            const uint32_t endX = (x + 1U) * size / nextSize;
            float normalX = 0.0F, normalY = 0.0F, normalZ = 0.0F;
            float roughness = 0.0F, ao = 0.0F;
            for (uint32_t sourceY = beginY; sourceY < endY; ++sourceY) {
                for (uint32_t sourceX = beginX; sourceX < endX; ++sourceX) {
                    const uint8_t *pixel = source.data() + (static_cast<size_t>(sourceY) * size + sourceX) * 4U;
                    const float x = pixel[0] / 127.5F - 1.0F;
                    const float y = pixel[1] / 127.5F - 1.0F;
                    normalX += x;
                    normalY += y;
                    normalZ += std::sqrt(std::max(0.0F, 1.0F - x * x - y * y));
                    roughness += pixel[2] / 255.0F;
                    ao += pixel[3] / 255.0F;
                }
            }
            const float sampleCount = static_cast<float>((endY - beginY) * (endX - beginX));
            uint8_t *pixel = destination.data() + (static_cast<size_t>(y) * nextSize + x) * 4U;
            const float length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
            pixel[0] = length > 1e-6F ? toByte(normalX / length * 0.5F + 0.5F) : 128;
            pixel[1] = length > 1e-6F ? toByte(normalY / length * 0.5F + 0.5F) : 128;
            pixel[2] = toByte(roughness / sampleCount);
            pixel[3] = toByte(ao / sampleCount);
        }
    }
}

void buildDecalColorMip(const ccstd::vector<uint8_t> &source, uint32_t size, ccstd::vector<uint8_t> &destination) {
    // RGB has already been converted to linear space and multiplied by
    // coverage Alpha. Average all four channels without another conversion.
    const uint32_t nextSize = std::max(1U, size / 2U);
    destination.resize(static_cast<size_t>(nextSize) * nextSize * 4U);
    for (uint32_t y = 0; y < nextSize; ++y) {
        for (uint32_t x = 0; x < nextSize; ++x) {
            // Integer ranges also include the last row/column of odd-sized images.
            const uint32_t beginY = y * size / nextSize;
            const uint32_t endY = (y + 1U) * size / nextSize;
            const uint32_t beginX = x * size / nextSize;
            const uint32_t endX = (x + 1U) * size / nextSize;
            float red = 0.0F, green = 0.0F, blue = 0.0F, alpha = 0.0F;
            for (uint32_t sourceY = beginY; sourceY < endY; ++sourceY) {
                for (uint32_t sourceX = beginX; sourceX < endX; ++sourceX) {
                    const uint8_t *pixel = source.data() + (static_cast<size_t>(sourceY) * size + sourceX) * 4U;
                    red += pixel[0] / 255.0F;
                    green += pixel[1] / 255.0F;
                    blue += pixel[2] / 255.0F;
                    alpha += pixel[3] / 255.0F;
                }
            }
            const float sampleCount = static_cast<float>((endY - beginY) * (endX - beginX));
            uint8_t *pixel = destination.data() + (static_cast<size_t>(y) * nextSize + x) * 4U;
            pixel[0] = toByte(red / sampleCount);
            pixel[1] = toByte(green / sampleCount);
            pixel[2] = toByte(blue / sampleCount);
            pixel[3] = toByte(alpha / sampleCount);
        }
    }
}

IntrusivePtr<gfx::Texture> createTextureArray(gfx::Device *device, uint32_t resolution, uint32_t layerCount) {
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D_ARRAY;
    info.layerCount = layerCount;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    // Use UNORM storage. Material RGB is decoded by the shader; decal RGB
    // is already linear. The GPU must not apply an automatic sRGB conversion.
    info.format = gfx::Format::RGBA8;
    info.width = resolution;
    info.height = resolution;
    info.levelCount = 1U;
    for (uint32_t size = resolution; size > 1U; size >>= 1U) {
        ++info.levelCount;
    }
    return device->createTexture(info);
}

void uploadMipLevel(gfx::Device *device, gfx::Texture *texture, uint32_t layer,
                    uint32_t mip, uint32_t size, const ccstd::vector<uint8_t> &pixels) {
    gfx::BufferTextureCopy region;
    region.texExtent = {size, size, 1U};
    region.texSubres.mipLevel = mip;
    region.texSubres.baseArrayLayer = layer;
    region.texSubres.layerCount = 1U;
    const uint8_t *buffers[]{pixels.data()};
    device->copyBuffersToTexture(buffers, texture, &region, 1U);
}

void uploadAlbedoHeight(gfx::Device *device, gfx::Texture *texture, uint32_t resolution,
                        uint32_t layer, ccstd::vector<uint8_t> pixels) {
    ccstd::vector<uint8_t> nextMip;
    uint32_t size = resolution;
    for (uint32_t mip = 0U; mip < texture->getInfo().levelCount; ++mip) {
        uploadMipLevel(device, texture, layer, mip, size, pixels);
        if (mip + 1U < texture->getInfo().levelCount) {
            buildAlbedoHeightMip(pixels, size, nextMip);
            pixels.swap(nextMip);
            size = std::max(1U, size / 2U);
        }
    }
}

void uploadNormalRoughnessAO(gfx::Device *device, gfx::Texture *texture, uint32_t resolution,
                             uint32_t layer, ccstd::vector<uint8_t> pixels) {
    ccstd::vector<uint8_t> nextMip;
    uint32_t size = resolution;
    for (uint32_t mip = 0U; mip < texture->getInfo().levelCount; ++mip) {
        uploadMipLevel(device, texture, layer, mip, size, pixels);
        if (mip + 1U < texture->getInfo().levelCount) {
            buildNormalRoughnessAOMip(pixels, size, nextMip);
            pixels.swap(nextMip);
            size = std::max(1U, size / 2U);
        }
    }
}

void uploadDecalColor(gfx::Device *device, gfx::Texture *texture, uint32_t resolution,
                      uint32_t layer, ccstd::vector<uint8_t> pixels) {
    ccstd::vector<uint8_t> nextMip;
    uint32_t size = resolution;
    for (uint32_t mip = 0U; mip < texture->getInfo().levelCount; ++mip) {
        uploadMipLevel(device, texture, layer, mip, size, pixels);
        if (mip + 1U < texture->getInfo().levelCount) {
            buildDecalColorMip(pixels, size, nextMip);
            pixels.swap(nextMip);
            size = std::max(1U, size / 2U);
        }
    }
}

} // namespace

MaterialLibrary::MaterialLibrary() = default;
MaterialLibrary::~MaterialLibrary() {
    destroy();
}

bool MaterialLibrary::init(gfx::Device *device, const LandscapeAsset &asset) {
    CC_ASSERT(_albedoHeight == nullptr && _normalRoughnessAO == nullptr && _whiteTexture == nullptr);
    if (device == nullptr || asset.materialLayers().empty()) {
        return false;
    }
    // Keep failure cleanup in one place, including partially loaded decals.
    if (!initMaterialLayers(device, asset) ||
        !initDecalLayers(device, asset) ||
        !initWhiteTexture(device)) {
        destroy();
        return false;
    }
    initSamplers(device);
    return true;
}

bool MaterialLibrary::initMaterialLayers(gfx::Device *device, const LandscapeAsset &asset) {
    const auto &layers = asset.materialLayers();
    const uint32_t resolution = asset.materialResolution();
    const uint32_t count = static_cast<uint32_t>(layers.size());
    _albedoHeight = createTextureArray(device, resolution, count);
    _normalRoughnessAO = createTextureArray(device, resolution, count);
    if (!valid()) {
        return false;
    }
    _tilingParams.resize(config::MATERIAL_LIBRARY_MAX);
    for (const auto &layer : layers) {
        ccstd::vector<uint8_t> albedoHeightPixels, normalRoughnessAOPixels;
        if (!decodeRGBA8(layer.albedoHeight, resolution, albedoHeightPixels) ||
            !decodeRGBA8(layer.normalRoughnessAO, resolution, normalRoughnessAOPixels)) {
            return false;
        }
        // Repeats per meter depend on the source texture, not the VT page resolution.
        const float uvScale = layer.pixelsPerMeter / static_cast<float>(resolution);
        _tilingParams[layer.id] = Vec4{uvScale, 0.0F, 0.0F, 0.0F};
        uploadAlbedoHeight(device, _albedoHeight, resolution, layer.id, std::move(albedoHeightPixels));
        uploadNormalRoughnessAO(device, _normalRoughnessAO, resolution, layer.id, std::move(normalRoughnessAOPixels));
    }
    return true;
}

bool MaterialLibrary::initDecalLayers(gfx::Device *device, const LandscapeAsset &asset) {
    // Decal color is premultiplied linear RGB: mip filtering cannot introduce
    // dark borders through transparent texels. Height RG is a uint16 scalar.
    const auto &decalLayers = asset.decalLayers();
    const uint32_t decalSize = decalLayers.empty() ? 1U : asset.decalResolution();
    const uint32_t decalCount = std::max(1U, static_cast<uint32_t>(decalLayers.size()));
    _decalAlbedo = createTextureArray(device, decalSize, decalCount);
    _decalNormal = createTextureArray(device, decalSize, decalCount);
    if (!_decalAlbedo || !_decalNormal) {
        return false;
    }
    auto heightInfo = _decalAlbedo->getInfo();
    // Packed RG stores one 16-bit height; displacement samples only mip 0.
    heightInfo.levelCount = 1;
    _decalHeight = device->createTexture(heightInfo);
    if (!_decalHeight) {
        return false;
    }
    for (uint32_t i = 0; i < decalCount; ++i) {
        ccstd::vector<uint8_t> color{0, 0, 0, 0}, normal{128, 128, 255, 255}, height{0, 0, 0, 255};
        if (!decalLayers.empty()) {
            const auto &decal = decalLayers[i];
            if (!decodeRGBA8(decal.albedoAlpha, decalSize, color) ||
                !decodeRGBA8(decal.normalRoughnessAO, decalSize, normal) ||
                !decodeRGBA8(decal.height, decalSize, height)) {
                return false;
            }
        }
        // Convert sRGB to linear RGB, then premultiply by coverage. Alpha itself
        // stays unchanged. The compose shader adds this RGB without multiplying
        // by Alpha again, and mip filtering preserves transparent color edges.
        for (size_t pixel = 0; pixel < color.size(); pixel += 4) {
            const float alpha = color[pixel + 3] / 255.0F;
            const float linearRed = srgbToLinear(color[pixel] / 255.0F);
            const float linearGreen = srgbToLinear(color[pixel + 1] / 255.0F);
            const float linearBlue = srgbToLinear(color[pixel + 2] / 255.0F);
            color[pixel] = toByte(linearRed * alpha);
            color[pixel + 1] = toByte(linearGreen * alpha);
            color[pixel + 2] = toByte(linearBlue * alpha);
        }
        uploadDecalColor(device, _decalAlbedo, decalSize, i, std::move(color));
        // With detailMaterials, B is a blend weight rather than roughness.
        // Both use the same linear average; RG still uses normal filtering.
        uploadNormalRoughnessAO(device, _decalNormal, decalSize, i, std::move(normal));
        uploadMipLevel(device, _decalHeight, i, 0U, decalSize, height);
    }
    return true;
}

bool MaterialLibrary::initWhiteTexture(gfx::Device *device) {
    gfx::TextureInfo info;
    info.type = gfx::TextureType::TEX2D;
    info.usage = gfx::TextureUsageBit::SAMPLED | gfx::TextureUsageBit::TRANSFER_DST;
    info.format = gfx::Format::RGBA8;
    info.width = 1U;
    info.height = 1U;
    info.layerCount = 1U;
    info.levelCount = 1U;
    _whiteTexture = device->createTexture(info);
    if (!_whiteTexture) {
        return false;
    }
    uploadMipLevel(device, _whiteTexture, 0U, 0U, 1U, {255, 255, 255, 255});
    return true;
}

void MaterialLibrary::initSamplers(gfx::Device *device) {
    gfx::SamplerInfo info;
    info.minFilter = gfx::Filter::LINEAR;
    info.magFilter = gfx::Filter::LINEAR;
    info.mipFilter = gfx::Filter::LINEAR;
    info.addressU = gfx::Address::WRAP;
    info.addressV = gfx::Address::WRAP;
    info.addressW = gfx::Address::WRAP;
    _repeatSampler = device->getSampler(info);
    info.addressU = gfx::Address::CLAMP;
    info.addressV = gfx::Address::CLAMP;
    info.addressW = gfx::Address::CLAMP;
    _clampSampler = device->getSampler(info);
}

void MaterialLibrary::destroy() {
    _albedoHeight = nullptr;
    _normalRoughnessAO = nullptr;
    _whiteTexture = nullptr;
    _decalAlbedo = nullptr;
    _decalNormal = nullptr;
    _decalHeight = nullptr;
    _repeatSampler = nullptr;
    _clampSampler = nullptr;
    _tilingParams.clear();
}

} // namespace landscape
} // namespace cc

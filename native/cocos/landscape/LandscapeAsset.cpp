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

#include "landscape/LandscapeAsset.h"

#include <cmath>
#include <cstring>
#include <utility>

#include "base/Log.h"
#include "base/Ptr.h"
#include "base/ThreadPool.h"
#include "platform/FileUtils.h"
#include "platform/Image.h"
#include "rapidjson/document.h"
#include "renderer/gfx-base/GFXDef-common.h"

namespace cc {
namespace landscape {

namespace {
bool readUnsigned(const rapidjson::Value &object, const char *name, uint32_t &value) {
    if (!object.IsObject() || !object.HasMember(name) || !object[name].IsUint()) {
        return false;
    }
    value = object[name].GetUint();
    return true;
}

bool readFloat(const rapidjson::Value &object, const char *name, float &value) {
    if (!object.IsObject() || !object.HasMember(name) || !object[name].IsNumber()) {
        return false;
    }
    value = object[name].GetFloat();
    return std::isfinite(value);
}

ccstd::string manifestPathFor(const ccstd::string &dataDir) {
    if (dataDir.size() >= 5U && dataDir.compare(dataDir.size() - 5U, 5U, ".json") == 0) {
        return dataDir;
    }
    const size_t end = dataDir.find_last_not_of("/\\");
    const ccstd::string trimmed = end == ccstd::string::npos ? dataDir : dataDir.substr(0, end + 1U);
    const size_t slash = trimmed.find_last_of("/\\");
    const ccstd::string name = slash == ccstd::string::npos ? trimmed : trimmed.substr(slash + 1U);
    return trimmed + "/" + name + ".json";
}
} // namespace

LandscapeAsset::LandscapeAsset() = default;

LandscapeAsset::~LandscapeAsset() {
    resetAsyncState();
}

void LandscapeAsset::resetAsyncState() {
    if (_async != nullptr) {
        _async->cancelled.store(true);
    }
    _async.reset();
    _pendingTiles.clear();
}

bool LandscapeAsset::load(const ccstd::string &dataDir) {
    resetAsyncState();
    const ccstd::string manifestPath = manifestPathFor(dataDir);
    const Data file = FileUtils::getInstance()->getDataFromFile(manifestPath);
    if (file.isNull()) {
        CC_LOG_WARNING("[Landscape] failed to load heightmap manifest from '%s'", manifestPath.c_str());
        return false;
    }

    rapidjson::Document document;
    document.Parse(reinterpret_cast<const char *>(file.getBytes()), file.getSize());
    if (document.HasParseError() || !document.IsObject()) {
        CC_LOG_WARNING("[Landscape] invalid heightmap manifest '%s'", manifestPath.c_str());
        return false;
    }

    LandscapeData parsed;
    if (!document.HasMember("sectorCount") || !document.HasMember("levels")) {
        CC_LOG_WARNING("[Landscape] heightmap manifest '%s' is missing required fields", manifestPath.c_str());
        return false;
    }
    const auto &sectorCount = document["sectorCount"];
    if (!sectorCount.IsArray() || sectorCount.Size() != 2U || !sectorCount[0].IsUint() ||
        !sectorCount[1].IsUint() || !readFloat(document, "sectorSizeMeters", parsed.sectorSize) ||
        !readUnsigned(document, "maxLevel", parsed.maxLevel) ||
        !readUnsigned(document, "minTileLevel", parsed.minTileLevel) ||
        !readUnsigned(document, "nodeTileResolution", parsed.tileResolution) ||
        !readFloat(document, "heightScale", parsed.heightScale) ||
        !readFloat(document, "heightBias", parsed.heightBias)) {
        CC_LOG_WARNING("[Landscape] heightmap manifest '%s' is missing required fields", manifestPath.c_str());
        return false;
    }
    parsed.sectorsX = sectorCount[0].GetUint();
    parsed.sectorsZ = sectorCount[1].GetUint();
    if (!parsed.valid()) {
        CC_LOG_WARNING("[Landscape] invalid heightmap dimensions in '%s'", manifestPath.c_str());
        return false;
    }

    const auto &levels = document["levels"];
    if (!levels.IsArray() || levels.Size() != parsed.maxLevel + 1U) {
        CC_LOG_WARNING("[Landscape] heightmap manifest '%s' has invalid levels", manifestPath.c_str());
        return false;
    }

    ccstd::vector<size_t> levelOffsets(static_cast<size_t>(parsed.maxLevel) + 1U, 0U);
    size_t nodesPerSector = 0U;
    for (uint32_t level = 0; level <= parsed.maxLevel; ++level) {
        levelOffsets[level] = nodesPerSector;
        const uint32_t side = 1U << (parsed.maxLevel - level);
        nodesPerSector += static_cast<size_t>(side) * side;
    }
    ccstd::vector<HeightRange> ranges(static_cast<size_t>(parsed.sectorsX) * parsed.sectorsZ * nodesPerSector,
                                      HeightRange{parsed.minHeight(), parsed.maxHeight()});

    // Both height tiles and range metadata use the complete uint16 domain.
    constexpr float HEIGHT_DENOMINATOR = 65535.0F;
    for (uint32_t level = 0; level <= parsed.maxLevel; ++level) {
        const auto &levelValue = levels[level];
        if (!levelValue.IsObject() || !levelValue.HasMember("heightRange") ||
            !levelValue["heightRange"].IsObject()) {
            CC_LOG_WARNING("[Landscape] heightmap manifest level L%u has no height range", level);
            return false;
        }
        const auto &range = levelValue["heightRange"];
        if (!range.HasMember("min") || !range.HasMember("max")) {
            CC_LOG_WARNING("[Landscape] heightmap manifest level L%u has incomplete height range", level);
            return false;
        }
        const auto &mins = range["min"];
        const auto &maxs = range["max"];
        const uint32_t side = 1U << (parsed.maxLevel - level);
        const size_t expected = static_cast<size_t>(parsed.sectorsX) * parsed.sectorsZ * side * side;
        if (!mins.IsArray() || !maxs.IsArray() || mins.Size() != expected || maxs.Size() != expected) {
            CC_LOG_WARNING("[Landscape] heightmap manifest level L%u has invalid height range size", level);
            return false;
        }
        const float scale = parsed.heightScale / HEIGHT_DENOMINATOR;
        const uint32_t globalSide = parsed.sectorsX * side;
        for (uint32_t globalZ = 0; globalZ < parsed.sectorsZ * side; ++globalZ) {
            for (uint32_t globalX = 0; globalX < globalSide; ++globalX) {
                const size_t index = static_cast<size_t>(globalZ) * globalSide + globalX;
                if (!mins[index].IsNumber() || !maxs[index].IsNumber()) {
                    CC_LOG_WARNING("[Landscape] heightmap manifest level L%u contains invalid ranges", level);
                    return false;
                }
                const uint32_t sectorX = globalX / side;
                const uint32_t sectorZ = globalZ / side;
                const uint32_t localX = globalX % side;
                const uint32_t localZ = globalZ % side;
                const size_t dst = static_cast<size_t>(sectorZ * parsed.sectorsX + sectorX) * nodesPerSector +
                                   levelOffsets[level] + static_cast<size_t>(localZ) * side + localX;
                ranges[dst] = HeightRange{
                    parsed.heightBias + mins[index].GetFloat() * scale,
                    parsed.heightBias + maxs[index].GetFloat() * scale,
                };
            }
        }
    }

    const size_t slash = manifestPath.find_last_of("/\\");
    const ccstd::string assetDir = slash == ccstd::string::npos ? "." : manifestPath.substr(0, slash);
    ccstd::vector<MaterialLayer> materialLayers;
    uint32_t materialResolution = 0U;
    if (document.HasMember("materialLibrary")) {
        const auto &library = document["materialLibrary"];
        uint32_t count = 0U;
        if (!readUnsigned(library, "count", count) || count == 0U || count > config::MATERIAL_LIBRARY_MAX ||
            !readUnsigned(library, "resolution", materialResolution) || materialResolution == 0U ||
            !library.HasMember("format") || !library["format"].IsString() ||
            ccstd::string(library["format"].GetString()) != "RGBA8" ||
            !library.HasMember("dir") || !library["dir"].IsString() ||
            !library.HasMember("layers") || !library["layers"].IsArray() || library["layers"].Size() != count) {
            CC_LOG_WARNING("[Landscape] invalid RGBA8 materialLibrary in '%s'", manifestPath.c_str());
            return false;
        }
        const ccstd::string materialDir = assetDir + "/" + library["dir"].GetString() + "/";
        for (uint32_t i = 0U; i < count; ++i) {
            const auto &value = library["layers"][i];
            MaterialLayer layer;
            if (!readUnsigned(value, "id", layer.id) || layer.id != i ||
                !value.HasMember("name") || !value["name"].IsString() ||
                !value.HasMember("albedoHeight") || !value["albedoHeight"].IsString() ||
                !value.HasMember("normalRoughnessAO") || !value["normalRoughnessAO"].IsString() ||
                !readFloat(value, "detailHeightScale", layer.detailHeightScale) ||
                !readFloat(value, "detailHeightBias", layer.detailHeightBias) ||
                (value.HasMember("uvScale") && (!readFloat(value, "uvScale", layer.uvScale) || layer.uvScale <= 0.0F))) {
                CC_LOG_WARNING("[Landscape] invalid material layer %u in '%s'", i, manifestPath.c_str());
                return false;
            }
            layer.name = value["name"].GetString();
            layer.albedoHeight = materialDir + value["albedoHeight"].GetString();
            layer.normalRoughnessAO = materialDir + value["normalRoughnessAO"].GetString();
            materialLayers.emplace_back(std::move(layer));
        }
    }

    // The shader's integer decode and the shared page pool require this layout.
    if (!document.HasMember("splatMap") || !document["splatMap"].IsObject()) {
        CC_LOG_WARNING("[Landscape] manifest requires paired splatMap tiles");
        return false;
    }
    const auto &splat = document["splatMap"];
    const auto matchesString = [](const rapidjson::Value &v, const char *name, const char *expected) {
        return v.HasMember(name) && v[name].IsString() && std::strcmp(v[name].GetString(), expected) == 0;
    };
    const auto matchesUint = [](const rapidjson::Value &v, const char *name, uint32_t expected) {
        uint32_t value = 0;
        return readUnsigned(v, name, value) && value == expected;
    };
    if (!matchesString(splat, "format", "R16UI") || !matchesString(splat, "fileFormat", "PNG") ||
        !matchesString(splat, "path", "nodes/L{level}/s_{x}_{z}.png") ||
        !matchesUint(splat, "resolution", parsed.tileResolution) ||
        !matchesUint(splat, "minTileLevel", parsed.minTileLevel) ||
        !matchesUint(splat, "maxTileLevel", parsed.maxLevel) ||
        !splat.HasMember("encoding") || !splat["encoding"].IsObject() || materialLayers.empty()) {
        CC_LOG_WARNING("[Landscape] splatMap must match the height tile pyramid and material library");
        return false;
    }
    const auto &encoding = splat["encoding"];
    const auto matchesField = [&](const char *name, uint32_t offset, uint32_t bits) {
        return encoding.HasMember(name) && encoding[name].IsObject() &&
               matchesUint(encoding[name], "offset", offset) && matchesUint(encoding[name], "bits", bits);
    };
    if (!matchesField("layer0", 0U, 5U) || !matchesField("layer1", 5U, 5U) ||
        !matchesField("layer1Weight", 10U, 6U) || !matchesUint(encoding["layer1Weight"], "divisor", 63U)) {
        CC_LOG_WARNING("[Landscape] unsupported splat bit encoding; expected 5/5/6");
        return false;
    }

    _data = parsed;
    _dataDir = assetDir;
    _materialLayers = std::move(materialLayers);
    _materialResolution = materialResolution;
    _levelOffsets = std::move(levelOffsets);
    _heightRanges = std::move(ranges);
    _nodesPerSector = nodesPerSector;
    return true;
}

bool LandscapeAsset::decodeTilePair(const ccstd::string &heightPath, const ccstd::string &splatPath,
                                    uint32_t resolution, uint32_t layerCount, TileData &tile) {
    bool valid = loadTile(heightPath, gfx::Format::RG8, resolution, tile.height) &&
                 loadTile(splatPath, gfx::Format::R16UI, resolution, tile.splat);
    for (size_t i = 0; valid && i < tile.splat.size(); i += sizeof(uint16_t)) {
        uint16_t packed = 0;
        std::memcpy(&packed, tile.splat.data() + i, sizeof(packed));
        valid = (packed & 31U) < layerCount && ((packed >> 5U) & 31U) < layerCount;
    }
    if (!valid) {
        CC_LOG_WARNING("[Landscape] failed to load height/splat pair: %s, %s",
                       heightPath.c_str(), splatPath.c_str());
    }
    return valid;
}

bool LandscapeAsset::loadRootTile(uint32_t x, uint32_t z, TileData &tile) const {
    if (!valid() || x >= _data.sectorsX || z >= _data.sectorsZ) {
        return false;
    }
    tile.key = makeNodeKey(_data.maxLevel, x, z);
    const ccstd::string directory = _dataDir + "/nodes/L" + std::to_string(_data.maxLevel) + "/";
    const ccstd::string suffix = std::to_string(x) + "_" + std::to_string(z) + ".png";
    return decodeTilePair(directory + "h_" + suffix, directory + "s_" + suffix,
                          _data.tileResolution, static_cast<uint32_t>(_materialLayers.size()), tile);
}

bool LandscapeAsset::requestTile(uint32_t level, uint32_t x, uint32_t z) {
    if (level < _data.minTileLevel || level > _data.maxLevel) {
        return false;
    }
    if (_async == nullptr) {
        _async = std::make_shared<AsyncState>();
    }
    const uint64_t key = makeNodeKey(level, x, z);
    if (!_pendingTiles.insert(key).second) {
        return false;
    }
    const ccstd::string directory = _dataDir + "/nodes/L" + std::to_string(level) + "/";
    const ccstd::string suffix = std::to_string(x) + "_" + std::to_string(z) + ".png";
    const ccstd::string heightPath = directory + "h_" + suffix;
    const ccstd::string splatPath = directory + "s_" + suffix;
    const uint32_t resolution = _data.tileResolution;
    const uint32_t layerCount = static_cast<uint32_t>(_materialLayers.size());
    const auto async = _async;
    LegacyThreadPool::getDefaultThreadPool()->pushTask(
        [async, key, heightPath, splatPath, resolution, layerCount](int /*threadId*/) {
            if (async->cancelled.load()) {
                return;
            }
            TileData tile;
            tile.key = key;
            const bool valid = decodeTilePair(heightPath, splatPath, resolution, layerCount, tile);
            std::lock_guard<std::mutex> lock(async->mutex);
            if (async->cancelled.load()) {
                return;
            }
            if (valid) {
                async->ready.push_back(std::move(tile));
            } else {
                async->failed.push_back(key);
            }
        },
        LegacyThreadPool::TaskType::IO);
    return true;
}

bool LandscapeAsset::takeReadyTile(TileData &tile) {
    if (_async == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_async->mutex);
    if (_async->ready.empty()) {
        return false;
    }
    tile = std::move(_async->ready.front());
    _async->ready.pop_front();
    _pendingTiles.erase(tile.key);
    return true;
}

bool LandscapeAsset::takeFailedTile(uint64_t &key) {
    if (_async == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_async->mutex);
    if (_async->failed.empty()) {
        return false;
    }
    key = _async->failed.front();
    _async->failed.pop_front();
    _pendingTiles.erase(key);
    return true;
}

size_t LandscapeAsset::rangeOffset(uint32_t level, uint32_t globalX, uint32_t globalZ) const {
    if (level > _data.maxLevel) {
        return _heightRanges.size();
    }
    const uint32_t side = 1U << (_data.maxLevel - level);
    const uint32_t globalSide = _data.sectorsX * side;
    if (globalX >= globalSide || globalZ >= _data.sectorsZ * side) {
        return _heightRanges.size();
    }
    const uint32_t sectorX = globalX / side;
    const uint32_t sectorZ = globalZ / side;
    const uint32_t localX = globalX % side;
    const uint32_t localZ = globalZ % side;
    return static_cast<size_t>(sectorZ * _data.sectorsX + sectorX) * _nodesPerSector +
           _levelOffsets[level] + static_cast<size_t>(localZ) * side + localX;
}

bool LandscapeAsset::getHeightRange(uint32_t level, uint32_t globalX, uint32_t globalZ,
                                    float &minY, float &maxY) const {
    if (!_data.valid() || level > _data.maxLevel) {
        return false;
    }
    const size_t offset = rangeOffset(level, globalX, globalZ);
    if (offset >= _heightRanges.size()) {
        return false;
    }
    minY = _heightRanges[offset].minY;
    maxY = _heightRanges[offset].maxY;
    return true;
}

bool LandscapeAsset::loadTile(const ccstd::string &path, gfx::Format format,
                              uint32_t tileResolution, ccstd::vector<uint8_t> &data) {
    if (format != gfx::Format::RG8 && format != gfx::Format::R16UI) {
        return false;
    }
    const Data file = FileUtils::getInstance()->getDataFromFile(path);
    if (file.isNull()) {
        return false;
    }
    IntrusivePtr<Image> image = ccnew Image();
    const bool packHeight = format == gfx::Format::RG8;
    if (!image->initWithImageData(file.getBytes(), file.getSize()) ||
        image->getRenderFormat() != gfx::Format::R16UI ||
        static_cast<uint32_t>(image->getWidth()) != tileResolution ||
        static_cast<uint32_t>(image->getHeight()) != tileResolution) {
        return false;
    }
    const size_t sampleCount = static_cast<size_t>(tileResolution) * tileResolution;
    const size_t sourceByteSize = sampleCount * sizeof(uint16_t);
    if (image->getDataLen() != sourceByteSize) {
        return false;
    }
    if (packHeight) {
        // Pack each source uint16 as high byte, low byte into an RG8 page.
        // The shader reconstructs the original 16-bit value after sampling.
        const auto *source = reinterpret_cast<const uint16_t *>(image->getData());
        data.resize(sampleCount * 2U);
        for (size_t i = 0; i < sampleCount; ++i) {
            data[i * 2U] = static_cast<uint8_t>(source[i] >> 8U);
            data[i * 2U + 1U] = static_cast<uint8_t>(source[i] & 0xffU);
        }
        return true;
    }
    data.assign(image->getData(), image->getData() + sourceByteSize);
    return true;
}

} // namespace landscape
} // namespace cc

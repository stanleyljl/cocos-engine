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
#include "landscape/VTPaging.h"
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

bool readPixelsPerMeter(const rapidjson::Value &object, uint32_t resolution, float &value) {
    if (object.HasMember("pixelsPerMeter")) {
        return readFloat(object, "pixelsPerMeter", value) && value > 0.0F;
    }
    // Preserve the appearance of manifests authored before density was in pixels/m.
    float legacyScale = 0.1F;
    if (object.HasMember("uvScale") && !readFloat(object, "uvScale", legacyScale)) {
        return false;
    }
    value = legacyScale * static_cast<float>(resolution);
    return std::isfinite(value) && value > 0.0F;
}

ccstd::string manifestPathFor(const ccstd::string &dataDir) {
    constexpr size_t SUFFIX_SIZE = sizeof(".lsmanifest") - 1U;
    if (dataDir.size() >= SUFFIX_SIZE && dataDir.compare(dataDir.size() - SUFFIX_SIZE, SUFFIX_SIZE, ".lsmanifest") == 0) {
        return dataDir;
    }
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

// Temporary parse state: publish to LandscapeAsset only after every section validates.
// JSON types remain confined to this implementation file.
struct LandscapeAsset::Manifest {
    const rapidjson::Document &document;
    const ccstd::string &manifestPath;
    LandscapeData parsed;
    ccstd::vector<size_t> levelOffsets;
    size_t nodesPerSector{0};
    ccstd::vector<HeightRange> ranges;
    ccstd::string assetDir;
    bool imported{false};
    ccstd::unordered_map<ccstd::string, ccstd::string> files;
    GlobalColorMap globalColorMap;
    ccstd::vector<MaterialLayer> materialLayers;
    uint32_t materialResolution{0};
    CliffMaterial cliffMaterial;
    ccstd::vector<DecalLayer> decalLayers;
    ccstd::vector<Decal> decals;
    uint32_t decalResolution{1};

    bool read() {
        return readDimensions() && readHeightRanges() && readFileIndex() &&
               readGlobalColor() && readMaterials() && readCliffMaterial() &&
               readDecalLayers() && readDecals() && validateTileLayout();
    }

    ccstd::string resolve(const ccstd::string &logicalPath) const {
        if (!imported) return assetDir + "/" + logicalPath;
        const auto it = files.find(logicalPath);
        return it == files.end() ? ccstd::string{} : it->second;
    }
    bool readDimensions();
    bool readHeightRanges();
    bool readFileIndex();
    bool readGlobalColor();
    bool readMaterials();
    bool readCliffMaterial();
    bool readDecalLayers();
    bool readDecals();
    bool validateTileLayout();
};

bool LandscapeAsset::Manifest::readDimensions() {
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
    return true;
}

bool LandscapeAsset::Manifest::readHeightRanges() {
    const auto &levels = document["levels"];
    if (!levels.IsArray() || levels.Size() != parsed.maxLevel + 1U) {
        CC_LOG_WARNING("[Landscape] heightmap manifest '%s' has invalid levels", manifestPath.c_str());
        return false;
    }

    nodesPerSector = computeSectorNodeLayout(parsed.maxLevel, levelOffsets);
    ranges.assign(static_cast<size_t>(parsed.sectorsX) * parsed.sectorsZ * nodesPerSector,
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
        const uint32_t side = computeNodesPerSide(parsed.maxLevel, level);
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
                const size_t dst = globalNodeRangeIndex(parsed, levelOffsets, nodesPerSector,
                                                        level, globalX, globalZ);
                ranges[dst] = HeightRange{
                    parsed.heightBias + mins[index].GetFloat() * scale,
                    parsed.heightBias + maxs[index].GetFloat() * scale,
                };
                float childStretch = 1.0F;
                if (level > 0U) {
                    for (uint32_t q = 0; q < 4U; ++q) {
                        const size_t child = globalNodeRangeIndex(parsed, levelOffsets, nodesPerSector,
                            level - 1U, globalX * 2U + (q & 1U), globalZ * 2U + (q >> 1U));
                        childStretch = std::max(childStretch, ranges[child].surfaceStretch);
                    }
                }
                ranges[dst].surfaceStretch = cliffDensityScale(ranges[dst].maxY - ranges[dst].minY,
                    computeNodeSize(parsed.sectorSize, parsed.maxLevel, level), childStretch);
            }
        }
    }
    return true;
}

bool LandscapeAsset::Manifest::readFileIndex() {
    const size_t slash = manifestPath.find_last_of("/\\");
    assetDir = slash == ccstd::string::npos ? "." : manifestPath.substr(0, slash);
    imported = manifestPath.size() >= 11U && manifestPath.compare(manifestPath.size() - 11U, 11U, ".lsmanifest") == 0;
    if (imported) {
        if (!document.HasMember("files") || !document["files"].IsObject() || document["files"].ObjectEmpty()) {
            CC_LOG_WARNING("[Landscape] imported manifest has no raw file index");
            return false;
        }
        // All native artifacts share the asset UUID and optional build hash.
        // Only replace the extension; do not assume an editor/source directory.
        const ccstd::string prefix = manifestPath.substr(0, manifestPath.size() - 11U);
        for (auto it = document["files"].MemberBegin(); it != document["files"].MemberEnd(); ++it) {
            if (!it->value.IsString()) return false;
            const ccstd::string suffix = it->value.GetString();
            if (suffix.size() <= 6U || suffix.compare(0, 6U, ".lsraw") != 0 ||
                suffix.find_first_not_of("0123456789", 6U) != ccstd::string::npos) {
                CC_LOG_WARNING("[Landscape] invalid raw file extension");
                return false;
            }
            if (!files.emplace(it->name.GetString(), prefix + suffix).second) return false;
        }
    }
    return true;
}

bool LandscapeAsset::Manifest::readGlobalColor() {
    if (document.HasMember("globalColorMap")) {
        const auto &map = document["globalColorMap"];
        if (!map.IsObject() || !map.HasMember("file") || !map["file"].IsString() ||
            !readUnsigned(map, "resolution", globalColorMap.resolution) ||
            globalColorMap.resolution == 0U || globalColorMap.resolution > 4096U ||
            !readFloat(map, "strength", globalColorMap.strength) ||
            globalColorMap.strength < 0.0F || globalColorMap.strength > 1.0F) {
            CC_LOG_WARNING("[Landscape] invalid globalColorMap in '%s'", manifestPath.c_str());
            return false;
        }
        globalColorMap.file = resolve(map["file"].GetString());
        if (globalColorMap.file.empty()) {
            CC_LOG_WARNING("[Landscape] global color map '%s' is missing from the imported file index in '%s'; "
                           "using layer colors. Reload the landscape-assets extension, reimport the terrain and rebuild assets.",
                           map["file"].GetString(), manifestPath.c_str());
            globalColorMap = GlobalColorMap{};
        }
    }
    return true;
}

bool LandscapeAsset::Manifest::readMaterials() {
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
        const ccstd::string materialDir = ccstd::string(library["dir"].GetString()) + "/";
        for (uint32_t i = 0U; i < count; ++i) {
            const auto &value = library["layers"][i];
            MaterialLayer layer;
            if (!readUnsigned(value, "id", layer.id) || layer.id != i ||
                !value.HasMember("name") || !value["name"].IsString() ||
                !value.HasMember("albedoHeight") || !value["albedoHeight"].IsString() ||
                !value.HasMember("normalRoughnessAO") || !value["normalRoughnessAO"].IsString() ||
                !readFloat(value, "detailHeightScale", layer.detailHeightScale) ||
                !readFloat(value, "detailHeightBias", layer.detailHeightBias) ||
                !readPixelsPerMeter(value, materialResolution, layer.pixelsPerMeter)) {
                CC_LOG_WARNING("[Landscape] invalid material layer %u in '%s'", i, manifestPath.c_str());
                return false;
            }
            layer.name = value["name"].GetString();
            layer.albedoHeight = resolve(materialDir + value["albedoHeight"].GetString());
            layer.normalRoughnessAO = resolve(materialDir + value["normalRoughnessAO"].GetString());
            if (layer.albedoHeight.empty() || layer.normalRoughnessAO.empty()) return false;
            materialLayers.emplace_back(std::move(layer));
        }
    }
    return true;
}

bool LandscapeAsset::Manifest::readCliffMaterial() {
    if (document.HasMember("cliffMaterial")) {
        const auto &value = document["cliffMaterial"];
        uint32_t layer = 0U;
        if (!readUnsigned(value, "layer", layer) || layer >= materialLayers.size() ||
            !readFloat(value, "maxNormalY", cliffMaterial.maxNormalY) ||
            cliffMaterial.maxNormalY < 0.0F || cliffMaterial.maxNormalY > 1.0F ||
            !readFloat(value, "globalColorInfluence", cliffMaterial.globalColorInfluence) ||
            cliffMaterial.globalColorInfluence < 0.0F || cliffMaterial.globalColorInfluence > 1.0F) {
            CC_LOG_WARNING("[Landscape] invalid cliffMaterial in '%s'", manifestPath.c_str());
            return false;
        }
        cliffMaterial.layer = static_cast<int32_t>(layer);
    }
    return true;
}

bool LandscapeAsset::Manifest::readDecalLayers() {
    if (document.HasMember("decalLibrary")) {
        const auto &library = document["decalLibrary"];
        if (!readUnsigned(library, "resolution", decalResolution) || decalResolution < 2 || decalResolution > 2048 ||
            (decalResolution & (decalResolution - 1U)) != 0 || !library.HasMember("layers") ||
            !library["layers"].IsArray() || library["layers"].Empty() || library["layers"].Size() > config::DECAL_LIBRARY_MAX) return false;
        for (const auto &value : library["layers"].GetArray()) {
            DecalLayer layer;
            if (!value.IsObject() || !value.HasMember("albedoMask") || !value["albedoMask"].IsString() ||
                !value.HasMember("normalRoughnessAO") || !value["normalRoughnessAO"].IsString() ||
                !value.HasMember("height") || !value["height"].IsString() ||
                !readFloat(value, "heightScale", layer.heightScale) || layer.heightScale < 0 || layer.heightScale > 10) return false;
            layer.albedoMask = resolve(value["albedoMask"].GetString());
            layer.normalRoughnessAO = resolve(value["normalRoughnessAO"].GetString());
            layer.height = resolve(value["height"].GetString());
            if (value.HasMember("modulateColor")) {
                if (!value["modulateColor"].IsBool()) return false;
                layer.modulateColor = value["modulateColor"].GetBool();
            }
            if (value.HasMember("detailMaterials")) {
                const auto &ids = value["detailMaterials"];
                if (!ids.IsArray() || ids.Size() != 2 || layer.heightScale != 0 || layer.modulateColor ||
                    !ids[0].IsUint() || !ids[1].IsUint() ||
                    ids[0].GetUint() >= materialLayers.size() || ids[1].GetUint() >= materialLayers.size()) return false;
                layer.detailMaterial0 = static_cast<int32_t>(ids[0].GetUint());
                layer.detailMaterial1 = static_cast<int32_t>(ids[1].GetUint());
            }
            if (layer.albedoMask.empty() || layer.normalRoughnessAO.empty() || layer.height.empty()) return false;
            decalLayers.emplace_back(std::move(layer));
        }
    }
    return true;
}

bool LandscapeAsset::Manifest::readDecals() {
    if (document.HasMember("decals")) {
        const auto &values = document["decals"];
        if (!values.IsArray() || values.Size() > config::DECAL_INSTANCE_MAX) return false;
        for (const auto &value : values.GetArray()) {
            Decal d;
            if (!readFloat(value, "x", d.x) || !readFloat(value, "z", d.z) || !readFloat(value, "size", d.size) ||
                !readUnsigned(value, "layer", d.layer) || d.layer >= decalLayers.size() || d.size <= 0 || d.size > 64 ||
                !readFloat(value, "nearDistance", d.nearDistance) || !readFloat(value, "farDistance", d.farDistance) ||
                d.nearDistance < 0 || d.farDistance <= d.nearDistance || d.farDistance > 200 ||
                d.x < -parsed.worldWidth() * .5F || d.z < -parsed.worldDepth() * .5F ||
                d.x + d.size > parsed.worldWidth() * .5F || d.z + d.size > parsed.worldDepth() * .5F) return false;
            decals.push_back(d);
        }
    }
    return true;
}

bool LandscapeAsset::Manifest::validateTileLayout() {
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
    if (!matchesUint(splat, "triangleMaterials", 3U) || !matchesString(splat, "triangleDiagonal", "00-11")) {
        CC_LOG_WARNING("[Landscape] splat maps require triangle ID fixing; restart editor, reimport Landscape assets and rebuild");
        return false;
    }
    const auto matchesField = [&](const char *name, uint32_t offset, uint32_t bits) {
        return encoding.HasMember(name) && encoding[name].IsObject() &&
               matchesUint(encoding[name], "offset", offset) && matchesUint(encoding[name], "bits", bits);
    };
    if (!matchesField("layer0", 0U, 5U) || !matchesField("layer1", 5U, 5U) ||
        !matchesField("layer1Weight", 10U, 6U) || !matchesUint(encoding["layer1Weight"], "divisor", 63U)) {
        CC_LOG_WARNING("[Landscape] unsupported splat bit encoding; expected 5/5/6");
        return false;
    }

    if (!document.HasMember("normalMap") || !document["normalMap"].IsObject()) {
        CC_LOG_WARNING("[Landscape] normalMap is required; regenerate normal PNG tiles, reimport and rebuild assets");
        return false;
    }
    const auto &normalMap = document["normalMap"];
    if (!matchesString(normalMap, "format", "RGB8") || !matchesString(normalMap, "fileFormat", "PNG") ||
        !matchesString(normalMap, "space", "terrain-local") || !matchesString(normalMap, "upAxis", "Y") ||
        !matchesString(normalMap, "path", "nodes/L{level}/n_{x}_{z}.png") ||
        !matchesUint(normalMap, "resolution", parsed.tileResolution) ||
        !matchesUint(normalMap, "minTileLevel", parsed.minTileLevel) || !matchesUint(normalMap, "maxTileLevel", parsed.maxLevel)) {
        CC_LOG_WARNING("[Landscape] RGB8 normalMap must match the height tile layout");
        return false;
    }
    return true;
}

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

    Manifest manifest{document, manifestPath};
    if (!manifest.read()) return false;

    _data = manifest.parsed;
    _dataDir = manifest.assetDir;
    _files = std::move(manifest.files);
    _materialLayers = std::move(manifest.materialLayers);
    _materialResolution = manifest.materialResolution;
    _globalColorMap = std::move(manifest.globalColorMap);
    _cliffMaterial = manifest.cliffMaterial;
    _decalLayers = std::move(manifest.decalLayers);
    _decals = std::move(manifest.decals);
    _decalResolution = manifest.decalResolution;
    _levelOffsets = std::move(manifest.levelOffsets);
    _heightRanges = std::move(manifest.ranges);
    _nodesPerSector = manifest.nodesPerSector;
    return true;
}

bool LandscapeAsset::decodeTileSet(const ccstd::string &heightPath, const ccstd::string &splatPath, const ccstd::string &normalPath,
                                    uint32_t resolution, TileData &tile) {
    const bool valid = loadTile(heightPath, gfx::Format::RG8, resolution, tile.height) &&
                       loadTile(splatPath, gfx::Format::R16UI, resolution, tile.splat) &&
                       loadTile(normalPath, gfx::Format::RGB8, resolution, tile.normal);
    if (!valid) {
        CC_LOG_WARNING("[Landscape] failed to load height/splat/normal tiles: %s, %s, %s",
                       heightPath.c_str(), splatPath.c_str(), normalPath.c_str());
    } else {
        // Disk PNGs retain RGB XYZ. Compact to RG XZ on the decoding worker,
        // before upload; height-field normals always lie in the +Y hemisphere.
        // Forward compaction is safe because each destination precedes the next
        // unread RGB pixel. No extra allocation or per-frame conversion needed.
        const size_t count = static_cast<size_t>(resolution) * resolution;
        for (size_t i = 0; i < count; ++i) {
            tile.normal[i * 2U] = tile.normal[i * 3U];
            tile.normal[i * 2U + 1U] = tile.normal[i * 3U + 2U];
        }
        tile.normal.resize(count * 2U);
    }
    return valid;
}

ccstd::string LandscapeAsset::resolveFile(const ccstd::string &logicalPath) const {
    if (_files.empty()) return _dataDir + "/" + logicalPath;
    const auto it = _files.find(logicalPath);
    return it == _files.end() ? ccstd::string{} : it->second;
}

bool LandscapeAsset::loadRootTile(uint32_t x, uint32_t z, TileData &tile) const {
    if (!valid() || x >= _data.sectorsX || z >= _data.sectorsZ) {
        return false;
    }
    tile.key = makeNodeKey(_data.maxLevel, x, z);
    const ccstd::string directory = "nodes/L" + std::to_string(_data.maxLevel) + "/";
    const ccstd::string suffix = std::to_string(x) + "_" + std::to_string(z) + ".png";
    return decodeTileSet(resolveFile(directory + "h_" + suffix), resolveFile(directory + "s_" + suffix),
                          resolveFile(directory + "n_" + suffix), _data.tileResolution, tile);
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
    const ccstd::string directory = "nodes/L" + std::to_string(level) + "/";
    const ccstd::string suffix = std::to_string(x) + "_" + std::to_string(z) + ".png";
    // Resolve search paths on the main thread: FileUtils' relative-path cache
    // is not synchronized. Worker reads must take its absolute-path fast path,
    // even for cache hits (another thread could insert/rehash the cache).
    auto *fileUtils = FileUtils::getInstance();
    const ccstd::string heightPath = fileUtils->fullPathForFilename(resolveFile(directory + "h_" + suffix));
    const ccstd::string splatPath = fileUtils->fullPathForFilename(resolveFile(directory + "s_" + suffix));
    const ccstd::string normalPath = fileUtils->fullPathForFilename(resolveFile(directory + "n_" + suffix));
    if (heightPath.empty() || splatPath.empty() || normalPath.empty() ||
        !fileUtils->isAbsolutePath(heightPath) || !fileUtils->isAbsolutePath(splatPath) || !fileUtils->isAbsolutePath(normalPath)) {
        CC_LOG_WARNING("[Landscape] cannot resolve absolute paths for tile L%u/%u/%u", level, x, z);
        // Preserve the asynchronous failure contract so the page cache can
        // release the pending request without scheduling an unsafe file read.
        std::lock_guard<std::mutex> lock(_async->mutex);
        _async->failed.push_back(key);
        return true;
    }
    const uint32_t resolution = _data.tileResolution;
    const auto async = _async;
    LegacyThreadPool::getDefaultThreadPool()->pushTask(
        [async, key, heightPath, splatPath, normalPath, resolution](int /*threadId*/) {
            if (async->cancelled.load()) {
                return;
            }
            TileData tile;
            tile.key = key;
            const bool valid = decodeTileSet(heightPath, splatPath, normalPath, resolution, tile);
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

bool LandscapeAsset::getHeightRange(uint32_t level, uint32_t globalX, uint32_t globalZ,
                                    float &minY, float &maxY) const {
    if (!_data.valid() || level > _data.maxLevel) {
        return false;
    }
    const size_t offset = globalNodeRangeIndex(_data, _levelOffsets, _nodesPerSector, level, globalX, globalZ);
    if (offset >= _heightRanges.size()) {
        return false;
    }
    minY = _heightRanges[offset].minY;
    maxY = _heightRanges[offset].maxY;
    return true;
}

float LandscapeAsset::getSurfaceStretch(uint32_t level, uint32_t globalX, uint32_t globalZ) const {
    if (!_data.valid() || level > _data.maxLevel) return 1.0F;
    const size_t offset = globalNodeRangeIndex(_data, _levelOffsets, _nodesPerSector, level, globalX, globalZ);
    return offset < _heightRanges.size() ? _heightRanges[offset].surfaceStretch : 1.0F;
}

bool LandscapeAsset::loadTile(const ccstd::string &path, gfx::Format format,
                              uint32_t tileResolution, ccstd::vector<uint8_t> &data) {
    if (format != gfx::Format::RG8 && format != gfx::Format::R16UI && format != gfx::Format::RGB8) {
        return false;
    }
    const Data file = FileUtils::getInstance()->getDataFromFile(path);
    if (file.isNull()) {
        return false;
    }
    IntrusivePtr<Image> image = ccnew Image();
    const bool packHeight = format == gfx::Format::RG8;
    if (!image->initWithImageData(file.getBytes(), file.getSize()) ||
        image->getRenderFormat() != (format == gfx::Format::RGB8 ? gfx::Format::RGB8 : gfx::Format::R16UI) ||
        static_cast<uint32_t>(image->getWidth()) != tileResolution ||
        static_cast<uint32_t>(image->getHeight()) != tileResolution) {
        return false;
    }
    const size_t sampleCount = static_cast<size_t>(tileResolution) * tileResolution;
    const size_t sourceByteSize = sampleCount * (format == gfx::Format::RGB8 ? 3U : sizeof(uint16_t));
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

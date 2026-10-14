// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeQuery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include "landscape/LandscapeConfig.h"

namespace cc {
namespace landscape {

LandscapeQuery::LandscapeQuery(const LandscapeData &data, Loader loader, uint32_t capacity)
: _data(data)
, _loader(std::move(loader))
, _capacity(std::max(1U, capacity))
, _tileSize(data.nodeSize(data.minTileLevel)) {
}

LandscapeQuery::~LandscapeQuery() = default;

uint64_t LandscapeQuery::TileRegion::tileCount() const {
    return uint64_t(maxX - minX + 1U) * (maxZ - minZ + 1U);
}

bool LandscapeQuery::TileRegion::contains(NodeAddress tile) const {
    return tile.x >= minX && tile.x <= maxX && tile.z >= minZ && tile.z <= maxZ;
}

bool LandscapeQuery::TileRegion::operator==(const TileRegion &other) const {
    return minX == other.minX && minZ == other.minZ && maxX == other.maxX && maxZ == other.maxZ;
}

LandscapeQueryStatus LandscapeQuery::addSource(uint32_t id, LandscapePoint center, float radius) {
    TileRegion region;
    const auto requested = resolveRegion(center, radius, region);
    if (requested != LandscapeQueryStatus::HIT) {
        return requested;
    }
    const auto previous = _sources.find(id);
    // Motion within the same tile rectangle does not allocate or change references.
    if (previous != _sources.end() && previous->second == region) {
        return getSourceStatus(id);
    }
    if (!canAdmit(region, previous == _sources.end() ? nullptr : &previous->second)) {
        return LandscapeQueryStatus::OUT_OF_CAPACITY;
    }
    // Acquire the new references first so moving a source keeps overlapping tiles.
    region.forEachTile(_data.minTileLevel, [&](uint64_t key) {
        auto &entry = _tiles[key];
        if (!entry) {
            entry = std::make_shared<Tile>();
            const auto address = NodeAddress::fromKey(key);
            _loader(address.x, address.z,
                    [weak = std::weak_ptr<Tile>(entry)](LandscapeQueryTile data, bool success) {
                        // Removal destroys the tile. Late results cannot restore it
                        // or affect a new request for the same coordinates.
                        if (auto tile = weak.lock()) {
                            if (success) {
                                tile->data = std::move(data);
                            }
                            tile->loaded.store(true, std::memory_order_release);
                        }
                    });
        }
        ++entry->references;
    });
    removeSource(id);
    _sources[id] = region;
    return getSourceStatus(id);
}

LandscapeQueryStatus LandscapeQuery::resolveRegion(LandscapePoint center, float radius, TileRegion &region) const {
    if (!_data.valid() || radius < 0) {
        return LandscapeQueryStatus::ERROR;
    }
    if (center.x + radius < 0 || center.z + radius < 0 || center.x - radius > _data.width() || center.z - radius > _data.depth()) {
        return LandscapeQueryStatus::MISS;
    }
    const auto lo = _data.nodeAtClamped(_data.minTileLevel, {center.x - radius, center.z - radius});
    const auto hi = _data.nodeAtClamped(_data.minTileLevel, {center.x + radius, center.z + radius});
    region = {lo.x, lo.z, hi.x, hi.z};
    return region.tileCount() <= _capacity ? LandscapeQueryStatus::HIT : LandscapeQueryStatus::OUT_OF_CAPACITY;
}

bool LandscapeQuery::canAdmit(const TileRegion &region, const TileRegion *previous) const {
    // Count the requested region plus tiles referenced by other sources outside it.
    // The source being moved will release its previous reference on each old tile.
    uint64_t required = region.tileCount();
    for (const auto &pair : _tiles) {
        const auto address = NodeAddress::fromKey(pair.first);
        const uint32_t releasedReferences = previous && previous->contains(address) ? 1U : 0U;
        if (pair.second->references > releasedReferences && !region.contains(address)) {
            if (++required > _capacity)
                return false;
        }
    }
    return true;
}

void LandscapeQuery::removeSource(uint32_t id) {
    const auto it = _sources.find(id);
    if (it == _sources.end()) {
        return;
    }
    it->second.forEachTile(_data.minTileLevel, [&](uint64_t key) {
        const auto tile = _tiles.find(key);
        if (--tile->second->references == 0) {
            _tiles.erase(tile);
        }
    });
    _sources.erase(it);
}

LandscapeQueryStatus LandscapeQuery::getSourceStatus(uint32_t id) const {
    const auto it = _sources.find(id);
    if (it == _sources.end()) {
        return LandscapeQueryStatus::NOT_READY;
    }
    bool ready = true;
    const auto &region = it->second;
    for (uint32_t z = region.minZ; z <= region.maxZ; ++z) {
        for (uint32_t x = region.minX; x <= region.maxX; ++x) {
            const auto status = _tiles.at(makeNodeKey(_data.minTileLevel, x, z))->status;
            if (status == LandscapeQueryStatus::ERROR)
                return LandscapeQueryStatus::ERROR;
            ready &= status == LandscapeQueryStatus::HIT;
        }
    }
    return ready ? LandscapeQueryStatus::HIT : LandscapeQueryStatus::NOT_READY;
}

void LandscapeQuery::update() {
    const size_t samples = size_t(_data.tileResolution) * _data.tileResolution;
    for (const auto &pair : _tiles) {
        auto &tile = *pair.second;
        if (tile.status != LandscapeQueryStatus::NOT_READY || !tile.loaded.load(std::memory_order_acquire)) {
            continue;
        }
        const bool valid = tile.data.height.size() == samples * 2U &&
                           tile.data.splat.size() == samples * 2U && tile.data.normal.size() == samples * 3U;
        tile.status = valid ? LandscapeQueryStatus::HIT : LandscapeQueryStatus::ERROR;
    }
}

LandscapeSurfaceResult LandscapeQuery::sample(LandscapePoint local) const {
    LandscapeSurfaceResult result;
    if (!_data.valid()) {
        result.status = LandscapeQueryStatus::ERROR;
        return result;
    }
    if (!_data.contains(local)) {
        result.status = LandscapeQueryStatus::MISS;
        return result;
    }
    const auto address = _data.nodeAtClamped(_data.minTileLevel, local);
    const auto it = _tiles.find(address.key());
    if (it == _tiles.end()) {
        return result;
    }
    result.status = it->second->status;
    if (result.status != LandscapeQueryStatus::HIT) {
        return result;
    }
    const auto &tile = it->second->data;
    // Locate one grid cell. All channels below share its four texels and
    // fractional coordinates. Height and painted materials share the B-C diagonal.
    const uint32_t edge = _data.tileResolution - 1U;
    const float gx = std::clamp((local.x / _tileSize - address.x) * edge, 0.0F, float(edge));
    const float gz = std::clamp((local.z / _tileSize - address.z) * edge, 0.0F, float(edge));
    const auto ix = std::min(static_cast<uint32_t>(gx), edge - 1U);
    const auto iz = std::min(static_cast<uint32_t>(gz), edge - 1U);
    const size_t a = size_t(iz) * _data.tileResolution + ix;
    // texels = {A, B, C, D}; X increases to the right, Z increases downward.
    // A ---- B
    // |      |
    // C ---- D
    // Height and splat use triangles (A,C,B) and (B,C,D), matching Terrain and physics.
    const std::array<size_t, 4> texels{a, a + 1U, a + _data.tileResolution, a + _data.tileResolution + 1U};
    const float fx = gx - ix;
    const float fz = gz - iz;

    uint16_t encoded;
    std::memcpy(&encoded, tile.splat.data() + a * 2U, sizeof(encoded));
    if ((encoded & 0x8000U) != 0U) {
        result.status = LandscapeQueryStatus::MISS;
        return result;
    }
    // Shading normals retain smooth bilinear interpolation. Physical height
    // follows GridMesh's B-C diagonal: triangles (A,C,B) and (B,C,D).
    const std::array<float, 4> weights{(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
    const std::array<float, 4> triangleWeights = fx + fz <= 1.0F
                                                   ? std::array<float, 4>{1 - fx - fz, fx, fz, 0}
                                                   : std::array<float, 4>{0, 1 - fz, 1 - fx, fx + fz - 1};
    float height = 0;
    float nx = 0;
    float ny = 0;
    float nz = 0;
    for (size_t i = 0; i < texels.size(); ++i) {
        const auto h = texels[i] * 2U;
        const auto n = texels[i] * 3U;
        height += triangleWeights[i] * (uint32_t(tile.height[h]) * 256U + tile.height[h + 1U]);
        nx += weights[i] * (tile.normal[n] * (2.0F / 255.0F) - 1.0F);
        ny += weights[i] * (tile.normal[n + 1U] * (2.0F / 255.0F) - 1.0F);
        nz += weights[i] * (tile.normal[n + 2U] * (2.0F / 255.0F) - 1.0F);
    }
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(length > 0)) {
        result.status = LandscapeQueryStatus::ERROR;
        return result;
    }
    result.position.set(local.x, _data.heightBias + height * (_data.heightScale / 65535.0F), local.z);
    result.normal.set(nx / length, ny / length, nz / length);

    // Painted splat uses the same triangle weights as height and VT composition.
    // No visual height-blend, cliff or decal overrides.
    std::array<float, config::MATERIAL_LIBRARY_MAX> material{};
    for (size_t i = 0; i < texels.size(); ++i) {
        if (triangleWeights[i] == 0.0F) {
            continue;
        }
        std::memcpy(&encoded, tile.splat.data() + texels[i] * 2U, sizeof(encoded));
        const float blend = float((encoded >> 10U) & 31U) / 31.0F;
        material[encoded & 31U] += triangleWeights[i] * (1 - blend);
        material[(encoded >> 5U) & 31U] += triangleWeights[i] * blend;
    }
    result.surfaceType = static_cast<int32_t>(std::max_element(material.begin(), material.end()) - material.begin());
    result.surfaceWeight = material[result.surfaceType];
    result.status = LandscapeQueryStatus::HIT;
    return result;
}

} // namespace landscape
} // namespace cc

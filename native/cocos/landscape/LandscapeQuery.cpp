// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeQuery.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <utility>

namespace cc {
namespace landscape {

LandscapeQuery::LandscapeQuery(const LandscapeData &data, Loader loader, uint32_t capacity)
: _data(data)
, _loader(std::move(loader))
, _capacity(std::max(1U, capacity))
, _tileSize(data.nodeSize(data.minTileLevel))
, _completions(std::make_shared<CompletionQueue>()) {
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

LandscapeQueryStatus LandscapeQuery::setSource(uint32_t id, LandscapePoint center, float radius) {
    TileRegion region;
    const auto requested = resolveRegion(center, radius, region);
    if (requested != LandscapeQueryStatus::HIT) {
        removeSource(id);
        return requested;
    }
    const auto previous = _sources.find(id);
    // Motion within the same tile rectangle does not allocate or repin tiles.
    if (previous != _sources.end() && previous->second == region) {
        return sourceStatus(id);
    }
    if (!canAdmit(region, previous == _sources.end() ? nullptr : &previous->second)) {
        removeSource(id);
        return LandscapeQueryStatus::OUT_OF_CAPACITY;
    }
    removeSource(id);
    pinRegion(region);
    _sources[id] = region;
    return sourceStatus(id);
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
    // Count the requested region plus tiles pinned by OTHER sources outside it.
    // The source being moved will release its previous pin on each old tile.
    uint64_t required = region.tileCount();
    for (const auto &pair : _cache) {
        const auto address = NodeAddress::fromKey(pair.first);
        const uint32_t releasedPins = previous && previous->contains(address) ? 1U : 0U;
        if (pair.second.pinCount > releasedPins && !region.contains(address)) {
            if (++required > _capacity)
                return false;
        }
    }
    return true;
}

void LandscapeQuery::pinRegion(const TileRegion &region) {
    region.forEachTile(_data.minTileLevel, [&](uint64_t key) {
        auto it = _cache.find(key);
        if (it == _cache.end()) {
            if (_cache.size() == _capacity)
                evictUnusedTile(region);
            it = _cache.emplace(key, CachedTile{}).first;
        }
        auto &entry = it->second;
        // A fresh registration can retry a previously failed, unpinned tile.
        if (!entry.pinCount && entry.state == TileState::FAILED) {
            entry.state = TileState::WAITING;
        }
        ++entry.pinCount;
        entry.lastUse = ++_clock;
    });
}

void LandscapeQuery::evictUnusedTile(const TileRegion &protectedRegion) {
    auto victim = _cache.end();
    for (auto it = _cache.begin(); it != _cache.end(); ++it) {
        // Protect the entire incoming region, including its not-yet-pinned
        // entries, so inserting an earlier tile cannot evict a later one.
        if (it->second.pinCount || protectedRegion.contains(NodeAddress::fromKey(it->first)))
            continue;
        if (victim == _cache.end() || it->second.lastUse < victim->second.lastUse)
            victim = it;
    }
    assert(victim != _cache.end()); // canAdmit() guarantees an available slot.
    _cache.erase(victim);
}

void LandscapeQuery::removeSource(uint32_t id) {
    const auto it = _sources.find(id);
    if (it == _sources.end()) {
        return;
    }
    it->second.forEachTile(_data.minTileLevel, [&](uint64_t key) {
        --_cache.at(key).pinCount;
    });
    _sources.erase(it);
}

LandscapeQueryStatus LandscapeQuery::sourceStatus(uint32_t id) const {
    const auto it = _sources.find(id);
    if (it == _sources.end()) {
        return LandscapeQueryStatus::NOT_READY;
    }
    bool ready = true;
    const auto &region = it->second;
    for (uint32_t z = region.minZ; z <= region.maxZ; ++z) {
        for (uint32_t x = region.minX; x <= region.maxX; ++x) {
            const auto state = _cache.at(makeNodeKey(_data.minTileLevel, x, z)).state;
            if (state == TileState::FAILED)
                return LandscapeQueryStatus::ERROR;
            ready &= state == TileState::READY;
        }
    }
    return ready ? LandscapeQueryStatus::HIT : LandscapeQueryStatus::NOT_READY;
}

void LandscapeQuery::update() {
    acceptCompletedLoads();
    startPendingLoads();
}

void LandscapeQuery::acceptCompletedLoads() {
    ccstd::vector<CompletedLoad> completed;
    {
        std::lock_guard<std::mutex> lock(_completions->mutex);
        completed.swap(_completions->results);
    }
    const size_t samples = size_t(_data.tileResolution) * _data.tileResolution;
    for (auto &item : completed) {
        --_inFlight;
        const auto it = _cache.find(item.key);
        // A moved source may have evicted/re-requested this key while decoding.
        if (it == _cache.end() || it->second.loadId != item.loadId) {
            continue;
        }
        auto &entry = it->second;
        const bool valid = item.success && item.data.height.size() == samples * 2U &&
                           item.data.splat.size() == samples * 2U && item.data.normal.size() == samples * 3U;
        entry.state = valid ? TileState::READY : TileState::FAILED;
        if (valid) {
            entry.data = std::move(item.data);
        }
    }
}

void LandscapeQuery::startPendingLoads() {
    // Includes queued completions: unpolled results cannot grow without bound.
    constexpr uint32_t MAX_IN_FLIGHT = 2;
    for (auto &pair : _cache) {
        auto &entry = pair.second;
        if (_inFlight >= MAX_IN_FLIGHT) {
            break;
        }
        if (!entry.pinCount || entry.state != TileState::WAITING) {
            continue;
        }
        entry.state = TileState::LOADING;
        entry.loadId = ++_nextLoadId;
        ++_inFlight;
        const auto key = pair.first;
        const auto loadId = entry.loadId;
        std::weak_ptr<CompletionQueue> weak = _completions;
        const auto address = NodeAddress::fromKey(key);
        _loader(address.x, address.z,
                [weak, key, loadId](LandscapeQueryTile tile, bool success) {
                    if (auto queue = weak.lock()) {
                        std::lock_guard<std::mutex> lock(queue->mutex);
                        queue->results.push_back({key, loadId, std::move(tile), success});
                    }
                });
    }
}

LandscapeSurfaceResult LandscapeQuery::sample(LandscapePoint local) {
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
    const auto key = address.key();
    const auto it = _cache.find(key);
    if (it == _cache.end() || it->second.state == TileState::WAITING || it->second.state == TileState::LOADING) {
        return result;
    }
    if (it->second.state == TileState::FAILED) {
        result.status = LandscapeQueryStatus::ERROR;
        return result;
    }
    auto &entry = it->second;
    entry.lastUse = ++_clock;
    const auto location = locateSample(local, address);
    uint16_t encoded;
    std::memcpy(&encoded, entry.data.splat.data() + location.texels[0] * 2U, sizeof(encoded));
    if ((encoded & 0x8000U) != 0U) {
        result.status = LandscapeQueryStatus::MISS;
        return result;
    }
    if (!sampleGeometry(entry.data, location, local, result)) {
        result.status = LandscapeQueryStatus::ERROR;
        return result;
    }
    sampleMaterial(entry.data, location, result);
    result.status = LandscapeQueryStatus::HIT;
    return result;
}

LandscapeQuery::SampleLocation LandscapeQuery::locateSample(LandscapePoint local, NodeAddress tile) const {
    const uint32_t edge = _data.tileResolution - 1U;
    const float gx = std::clamp((local.x / _tileSize - tile.x) * edge, 0.0F, float(edge));
    const float gz = std::clamp((local.z / _tileSize - tile.z) * edge, 0.0F, float(edge));
    const auto ix = std::min(static_cast<uint32_t>(gx), edge - 1U);
    const auto iz = std::min(static_cast<uint32_t>(gz), edge - 1U);
    const size_t a = size_t(iz) * _data.tileResolution + ix;
    return {{a, a + 1U, a + _data.tileResolution, a + _data.tileResolution + 1U},
            gx - ix,
            gz - iz};
}

bool LandscapeQuery::sampleGeometry(const LandscapeQueryTile &tile, const SampleLocation &location,
                                    LandscapePoint local, LandscapeSurfaceResult &result) const {
    const float fx = location.fx;
    const float fz = location.fz;
    // Shading normals retain smooth bilinear interpolation. Physical height
    // follows GridMesh's B-C diagonal: triangles (A,C,B) and (B,C,D).
    const std::array<float, 4> weights{(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
    const std::array<float, 4> heightWeights = fx + fz <= 1.0F
                                                   ? std::array<float, 4>{1 - fx - fz, fx, fz, 0}
                                                   : std::array<float, 4>{0, 1 - fz, 1 - fx, fx + fz - 1};
    float height = 0, nx = 0, ny = 0, nz = 0;
    for (size_t i = 0; i < location.texels.size(); ++i) {
        const auto h = location.texels[i] * 2U, n = location.texels[i] * 3U;
        height += heightWeights[i] * (uint32_t(tile.height[h]) * 256U + tile.height[h + 1U]);
        nx += weights[i] * (tile.normal[n] * (2.0F / 255.0F) - 1.0F);
        ny += weights[i] * (tile.normal[n + 1U] * (2.0F / 255.0F) - 1.0F);
        nz += weights[i] * (tile.normal[n + 2U] * (2.0F / 255.0F) - 1.0F);
    }
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(length > 0)) {
        return false;
    }
    result.position.set(local.x, _data.heightBias + height * (_data.heightScale / 65535.0F), local.z);
    result.normal.set(nx / length, ny / length, nz / length);
    return true;
}

void LandscapeQuery::sampleMaterial(const LandscapeQueryTile &tile, const SampleLocation &location,
                                    LandscapeSurfaceResult &result) {
    const float fx = location.fx;
    const float fz = location.fz;
    // Painted splat interpolation uses the importer's 00->11 diagonal, as the
    // VT compose shader does. No visual height-blend, cliff or decal overrides.
    const auto &at = location.texels;
    const std::array<size_t, 3> splatAt{at[0], fx >= fz ? at[1] : at[2], at[3]};
    const std::array<float, 3> spatial{1 - std::max(fx, fz), std::abs(fx - fz), std::min(fx, fz)};
    std::array<float, 32> material{};
    for (size_t i = 0; i < splatAt.size(); ++i) {
        uint16_t encoded;
        std::memcpy(&encoded, tile.splat.data() + splatAt[i] * 2U, sizeof(encoded));
        const float blend = float((encoded >> 10U) & 31U) / 31.0F;
        material[encoded & 31U] += spatial[i] * (1 - blend);
        material[(encoded >> 5U) & 31U] += spatial[i] * blend;
    }
    result.surfaceType = static_cast<int32_t>(std::max_element(material.begin(), material.end()) - material.begin());
    result.surfaceWeight = material[result.surfaceType];
}

} // namespace landscape
} // namespace cc

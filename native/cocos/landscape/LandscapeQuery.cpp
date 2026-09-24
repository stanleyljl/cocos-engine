// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeQuery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include "base/std/container/unordered_set.h"

namespace cc {
namespace landscape {

LandscapeQuery::LandscapeQuery(const LandscapeData &data, Loader loader, uint32_t capacity)
: _data(data), _loader(std::move(loader)), _capacity(std::max(1U, capacity)),
  _tilesX(data.sectorsX * computeNodesPerSide(data.maxLevel, data.minTileLevel)),
  _tilesZ(data.sectorsZ * computeNodesPerSide(data.maxLevel, data.minTileLevel)),
  _tileSize(computeNodeSize(data.sectorSize, data.maxLevel, data.minTileLevel)),
  _deliveries(std::make_shared<Deliveries>()) {}

LandscapeQuery::~LandscapeQuery() = default;

uint64_t LandscapeQuery::keyAt(double x, double z) const {
    const auto ix = static_cast<uint32_t>(std::clamp(std::floor(x / _tileSize), 0.0, double(_tilesX - 1U)));
    const auto iz = static_cast<uint32_t>(std::clamp(std::floor(z / _tileSize), 0.0, double(_tilesZ - 1U)));
    return makeNodeKey(_data.minTileLevel, ix, iz);
}

LandscapeQueryStatus LandscapeQuery::setSource(uint32_t id, float x, float z, float radius) {
    if (!_data.valid() || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) || radius < 0) {
        removeSource(id);
        return LandscapeQueryStatus::ERROR;
    }
    const double px = double(x) + _data.worldWidth() * 0.5;
    const double pz = double(z) + _data.worldDepth() * 0.5;
    if (px + radius < 0 || pz + radius < 0 || px - radius > _data.worldWidth() || pz - radius > _data.worldDepth()) {
        removeSource(id);
        return LandscapeQueryStatus::MISS;
    }
    const auto lo = keyAt(px - radius, pz - radius);
    const auto hi = keyAt(px + radius, pz + radius);
    const auto x0 = static_cast<uint32_t>((lo >> NODE_KEY_COORD_BITS) & NODE_KEY_COORD_MASK);
    const auto z0 = static_cast<uint32_t>(lo & NODE_KEY_COORD_MASK);
    const auto x1 = static_cast<uint32_t>((hi >> NODE_KEY_COORD_BITS) & NODE_KEY_COORD_MASK);
    const auto z1 = static_cast<uint32_t>(hi & NODE_KEY_COORD_MASK);
    // Reject an oversized request before creating an unbounded key list.
    if (uint64_t(x1 - x0 + 1U) * (z1 - z0 + 1U) > _capacity) {
        removeSource(id);
        return LandscapeQueryStatus::NOT_READY;
    }
    const auto previous = _sources.find(id);
    // Motion within the same tile rectangle does not allocate or repin tiles.
    if (previous != _sources.end() && previous->second.front() == lo && previous->second.back() == hi)
        return sourceStatus(id);
    ccstd::vector<uint64_t> keys;
    keys.reserve(size_t(x1 - x0 + 1U) * (z1 - z0 + 1U));
    for (uint32_t iz = z0; iz <= z1; ++iz) {
        for (uint32_t ix = x0; ix <= x1; ++ix) keys.push_back(makeNodeKey(_data.minTileLevel, ix, iz));
    }
    const ccstd::unordered_set<uint64_t> desired(keys.begin(), keys.end());
    ccstd::unordered_set<uint64_t> old;
    if (previous != _sources.end()) old.insert(previous->second.begin(), previous->second.end());
    size_t protectedCount = keys.size();
    for (const auto &pair : _entries) {
        if (pair.second.references > old.count(pair.first) && !desired.count(pair.first)) ++protectedCount;
    }
    // An over-budget source cannot evict another source's data. Release its old
    // footprint so an unreachable previous area cannot block future admission.
    removeSource(id);
    if (protectedCount > _capacity) return LandscapeQueryStatus::NOT_READY;
    for (const auto key : keys) {
        if (!_entries.count(key)) {
            if (_entries.size() == _capacity) {
                auto victim = _entries.end();
                for (auto it = _entries.begin(); it != _entries.end(); ++it) {
                    if (it->second.references || desired.count(it->first)) continue;
                    if (victim == _entries.end() || it->second.lastUse < victim->second.lastUse) victim = it;
                }
                if (victim != _entries.end()) _entries.erase(victim);
            }
            _entries.emplace(key, Entry{});
        }
        auto &entry = _entries.at(key);
        // A fresh registration can retry a previously failed, unpinned tile.
        if (!entry.references && entry.state == State::FAILED) entry.state = State::WAITING;
        ++entry.references;
        entry.lastUse = ++_clock;
    }
    _sources[id] = std::move(keys);
    return sourceStatus(id);
}

void LandscapeQuery::removeSource(uint32_t id) {
    const auto it = _sources.find(id);
    if (it == _sources.end()) return;
    for (const auto key : it->second) --_entries.at(key).references;
    _sources.erase(it);
}

LandscapeQueryStatus LandscapeQuery::sourceStatus(uint32_t id) const {
    const auto it = _sources.find(id);
    if (it == _sources.end()) return LandscapeQueryStatus::NOT_READY;
    bool ready = true;
    for (const auto key : it->second) {
        const auto state = _entries.at(key).state;
        if (state == State::FAILED) return LandscapeQueryStatus::ERROR;
        ready &= state == State::READY;
    }
    return ready ? LandscapeQueryStatus::HIT : LandscapeQueryStatus::NOT_READY;
}

void LandscapeQuery::update() {
    ccstd::vector<Finished> finished;
    {
        std::lock_guard<std::mutex> lock(_deliveries->mutex);
        finished.swap(_deliveries->finished);
    }
    const size_t samples = size_t(_data.tileResolution) * _data.tileResolution;
    for (auto &item : finished) {
        --_inFlight;
        const auto it = _entries.find(item.key);
        // A moved source may have evicted/re-requested this key while decoding.
        if (it == _entries.end() || it->second.ticket != item.ticket) continue;
        auto &entry = it->second;
        const bool valid = item.success && item.tile.height.size() == samples * 2U &&
            item.tile.splat.size() == samples * 2U && item.tile.normal.size() == samples * 3U;
        entry.state = valid ? State::READY : State::FAILED;
        if (valid) entry.tile = std::move(item.tile);
    }
    // Includes queued completions: unpolled results cannot grow without bound.
    constexpr uint32_t MAX_IN_FLIGHT = 2;
    for (auto &pair : _entries) {
        auto &entry = pair.second;
        if (_inFlight >= MAX_IN_FLIGHT) break;
        if (!entry.references || entry.state != State::WAITING) continue;
        entry.state = State::LOADING;
        entry.ticket = ++_nextTicket;
        ++_inFlight;
        const auto key = pair.first;
        const auto ticket = entry.ticket;
        std::weak_ptr<Deliveries> weak = _deliveries;
        _loader(static_cast<uint32_t>((key >> NODE_KEY_COORD_BITS) & NODE_KEY_COORD_MASK),
                static_cast<uint32_t>(key & NODE_KEY_COORD_MASK),
                [weak, key, ticket](LandscapeQueryTile tile, bool success) {
                    if (auto delivery = weak.lock()) {
                        std::lock_guard<std::mutex> lock(delivery->mutex);
                        delivery->finished.push_back({key, ticket, std::move(tile), success});
                    }
                });
    }
}

LandscapeSurfaceResult LandscapeQuery::sample(float x, float z) {
    LandscapeSurfaceResult result;
    if (!_data.valid() || !std::isfinite(x) || !std::isfinite(z)) {
        result.status = LandscapeQueryStatus::ERROR;
        return result;
    }
    const double px = double(x) + _data.worldWidth() * 0.5;
    const double pz = double(z) + _data.worldDepth() * 0.5;
    if (px < 0 || pz < 0 || px > _data.worldWidth() || pz > _data.worldDepth()) {
        result.status = LandscapeQueryStatus::MISS;
        return result;
    }
    const auto key = keyAt(px, pz);
    const auto it = _entries.find(key);
    if (it == _entries.end() || it->second.state == State::WAITING || it->second.state == State::LOADING) return result;
    if (it->second.state == State::FAILED) {
        result.status = LandscapeQueryStatus::ERROR;
        return result;
    }
    auto &entry = it->second;
    entry.lastUse = ++_clock;
    const uint32_t edge = _data.tileResolution - 1U;
    const double gx = std::clamp((px / _tileSize - double((key >> NODE_KEY_COORD_BITS) & NODE_KEY_COORD_MASK)) * edge, 0.0, double(edge));
    const double gz = std::clamp((pz / _tileSize - double(key & NODE_KEY_COORD_MASK)) * edge, 0.0, double(edge));
    const auto ix = std::min(static_cast<uint32_t>(gx), edge - 1U);
    const auto iz = std::min(static_cast<uint32_t>(gz), edge - 1U);
    const float fx = static_cast<float>(gx - ix), fz = static_cast<float>(gz - iz);
    const size_t a = size_t(iz) * _data.tileResolution + ix;
    const std::array<size_t, 4> at{a, a + 1U, a + _data.tileResolution, a + _data.tileResolution + 1U};
    // Shading normals retain smooth bilinear interpolation. Physical height
    // follows GridMesh's B-C diagonal: triangles (A,C,B) and (B,C,D).
    const std::array<float, 4> weights{(1-fx)*(1-fz), fx*(1-fz), (1-fx)*fz, fx*fz};
    const std::array<float, 4> heightWeights = fx + fz <= 1.0F
        ? std::array<float, 4>{1-fx-fz, fx, fz, 0}
        : std::array<float, 4>{0, 1-fz, 1-fx, fx+fz-1};
    double height = 0, nx = 0, ny = 0, nz = 0;
    for (size_t i = 0; i < at.size(); ++i) {
        const auto h = at[i] * 2U, n = at[i] * 3U;
        height += heightWeights[i] * (uint32_t(entry.tile.height[h]) * 256U + entry.tile.height[h + 1U]);
        nx += weights[i] * (entry.tile.normal[n] * (2.0 / 255.0) - 1.0);
        ny += weights[i] * (entry.tile.normal[n + 1U] * (2.0 / 255.0) - 1.0);
        nz += weights[i] * (entry.tile.normal[n + 2U] * (2.0 / 255.0) - 1.0);
    }
    const double length = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (!(length > 0)) { result.status = LandscapeQueryStatus::ERROR; return result; }
    result.position.set(x, static_cast<float>(_data.heightBias + height * (_data.heightScale / 65535.0)), z);
    result.normal.set(static_cast<float>(nx / length), static_cast<float>(ny / length), static_cast<float>(nz / length));
    // Painted splat interpolation uses the importer's 00->11 diagonal, as the
    // VT compose shader does. No visual height-blend, cliff or decal overrides.
    const std::array<size_t, 3> splatAt{at[0], fx >= fz ? at[1] : at[2], at[3]};
    const std::array<float, 3> spatial{1-std::max(fx,fz), std::abs(fx-fz), std::min(fx,fz)};
    std::array<float, 32> material{};
    for (size_t i = 0; i < splatAt.size(); ++i) {
        uint16_t encoded;
        std::memcpy(&encoded, entry.tile.splat.data() + splatAt[i] * 2U, sizeof(encoded));
        const float blend = float(encoded >> 10U) / 63.0F;
        material[encoded & 31U] += spatial[i] * (1-blend);
        material[(encoded >> 5U) & 31U] += spatial[i] * blend;
    }
    result.surfaceType = static_cast<int32_t>(std::max_element(material.begin(), material.end()) - material.begin());
    result.surfaceWeight = material[result.surfaceType];
    result.status = LandscapeQueryStatus::HIT;
    return result;
}

} // namespace landscape
} // namespace cc

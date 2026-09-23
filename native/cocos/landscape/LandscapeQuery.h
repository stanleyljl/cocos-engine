// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeConfig.h"
#include "math/Vec3.h"

namespace cc {
namespace landscape {

enum class LandscapeQueryStatus : uint32_t {
    HIT,
    MISS,
    NOT_READY,
    ERROR
};

struct LandscapeSurfaceResult {
    LandscapeQueryStatus status{LandscapeQueryStatus::NOT_READY};
    Vec3 position;
    Vec3 normal;
    int32_t surfaceType{-1};
    float surfaceWeight{0.0F};
};

// CPU data only. Normals retain the source RGB8 XYZ, unlike the GPU's RG8 XZ.
struct LandscapeQueryTile {
    ccstd::vector<uint8_t> height;
    ccstd::vector<uint8_t> normal;
    ccstd::vector<uint8_t> splat;
};

// Main-thread cache; only completion delivery may run on a worker. It never
// reads render residency, camera LOD, or GPU resources. Source rectangles pin
// their union; unreferenced tiles are LRU eviction candidates.
class LandscapeQuery final {
public:
    using Completion = std::function<void(LandscapeQueryTile, bool)>;
    using Loader = std::function<void(uint32_t, uint32_t, Completion)>;
    LandscapeQuery(const LandscapeData &data, Loader loader, uint32_t capacity = 64);
    ~LandscapeQuery();
    LandscapeQueryStatus setSource(uint32_t id, float x, float z, float radius);
    void removeSource(uint32_t id);
    LandscapeQueryStatus sourceStatus(uint32_t id) const;
    void update();
    LandscapeSurfaceResult sample(float x, float z);

private:
    enum class State { WAITING, LOADING, READY, FAILED };
    struct Entry {
        LandscapeQueryTile tile;
        State state{State::WAITING};
        uint32_t references{0};
        uint64_t lastUse{0};
        uint64_t ticket{0};
    };
    struct Finished {
        uint64_t key;
        uint64_t ticket;
        LandscapeQueryTile tile;
        bool success;
    };
    struct Deliveries {
        std::mutex mutex;
        ccstd::vector<Finished> finished;
    };
    uint64_t keyAt(double x, double z) const;
    LandscapeData _data;
    Loader _loader;
    uint32_t _capacity;
    uint32_t _tilesX;
    uint32_t _tilesZ;
    double _tileSize;
    uint64_t _clock{0};
    uint64_t _nextTicket{0};
    uint32_t _inFlight{0};
    ccstd::unordered_map<uint64_t, Entry> _entries;
    ccstd::unordered_map<uint32_t, ccstd::vector<uint64_t>> _sources;
    std::shared_ptr<Deliveries> _deliveries;
};

} // namespace landscape
} // namespace cc

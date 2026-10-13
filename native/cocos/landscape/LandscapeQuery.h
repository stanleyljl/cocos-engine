// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/LandscapeData.h"
#include "math/Vec3.h"

namespace cc {
namespace landscape {

enum class LandscapeQueryStatus : uint32_t {
    HIT,
    MISS,
    NOT_READY,
    ERROR,
    OUT_OF_CAPACITY // Requested region and other sources' pinned tiles exceed the cache budget.
};

struct LandscapeSurfaceResult {
    LandscapeQueryStatus status{LandscapeQueryStatus::NOT_READY};
    Vec3 position; // landscape-local; Landscape adds the node's world translation
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

// Sources pin rectangular regions of CPU tiles, independently of rendering.
// Only unpinned tiles can be evicted. All cache/source changes happen on the
// main thread; workers only deliver results through the lifetime-safe queue.
class LandscapeQuery final {
public:
    using Completion = std::function<void(LandscapeQueryTile, bool)>;
    using Loader = std::function<void(uint32_t, uint32_t, Completion)>;
    LandscapeQuery(const LandscapeData &data, Loader loader, uint32_t capacity = 64);
    ~LandscapeQuery();
    // Inputs and returned positions are landscape-local; Landscape converts world space.
    LandscapeQueryStatus setSource(uint32_t id, LandscapePoint center, float radius);
    void removeSource(uint32_t id);
    LandscapeQueryStatus sourceStatus(uint32_t id) const;
    void update();
    LandscapeSurfaceResult sample(LandscapePoint local);

private:
    // Inclusive tile indices at minTileLevel. A source needs no allocated key
    // list: region equality, membership and tile count use these four bounds.
    struct TileRegion {
        uint32_t minX{0};
        uint32_t minZ{0};
        uint32_t maxX{0};
        uint32_t maxZ{0};
        uint64_t tileCount() const;
        bool contains(NodeAddress tile) const;
        bool operator==(const TileRegion &other) const;
        template <class Visitor>
        void forEachTile(uint32_t level, Visitor visit) const {
            for (uint32_t z = minZ; z <= maxZ; ++z) {
                for (uint32_t x = minX; x <= maxX; ++x) {
                    visit(makeNodeKey(level, x, z));
                }
            }
        }
    };
    enum class TileState {
        WAITING,
        LOADING,
        READY,
        FAILED
    };
    struct CachedTile {
        LandscapeQueryTile data;
        TileState state{TileState::WAITING};
        uint32_t pinCount{0};
        uint64_t lastUse{0};
        uint64_t loadId{0}; // Distinguishes an evicted/re-requested tile's loads.
    };
    struct CompletedLoad {
        uint64_t key;
        uint64_t loadId;
        LandscapeQueryTile data;
        bool success;
    };
    struct CompletionQueue {
        std::mutex mutex;
        ccstd::vector<CompletedLoad> results;
    };

    struct SampleLocation {
        std::array<size_t, 4> texels;
        float fx{0};
        float fz{0};
    };

    // Region admission and cache ownership. Admission is checked before pins
    // change, so a request can never evict another source's resident tiles.
    LandscapeQueryStatus resolveRegion(LandscapePoint center, float radius, TileRegion &region) const;
    bool canAdmit(const TileRegion &region, const TileRegion *previous) const;
    void pinRegion(const TileRegion &region);
    void evictUnusedTile(const TileRegion &protectedRegion);

    // One update publishes completed loads, then fills the available IO slots.
    void acceptCompletedLoads();
    void startPendingLoads();

    // Sampling only reads resident bytes; it never submits loading work.
    SampleLocation locateSample(LandscapePoint local, NodeAddress tile) const;
    bool sampleGeometry(const LandscapeQueryTile &tile, const SampleLocation &location,
                        LandscapePoint local, LandscapeSurfaceResult &result) const;
    static void sampleMaterial(const LandscapeQueryTile &tile, const SampleLocation &location, LandscapeSurfaceResult &result);

    LandscapeData _data;
    Loader _loader;
    uint32_t _capacity;
    float _tileSize;
    uint64_t _clock{0};
    uint64_t _nextLoadId{0};
    uint32_t _inFlight{0};
    ccstd::unordered_map<uint32_t, TileRegion> _sources;
    ccstd::unordered_map<uint64_t, CachedTile> _cache;
    std::shared_ptr<CompletionQueue> _completions;
};

} // namespace landscape
} // namespace cc

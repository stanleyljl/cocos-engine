// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "landscape/Data.h"
#include "math/Vec3.h"

namespace cc {
namespace landscape {

enum class LandscapeQueryStatus : uint32_t {
    HIT,
    MISS,
    NOT_READY,
    ERROR,
    OUT_OF_CAPACITY // Requested region and other sources' tiles exceed the capacity.
};

struct LandscapeSurfaceResult {
    LandscapeQueryStatus status{LandscapeQueryStatus::NOT_READY};
    Vec3 position; // landscape-local; Landscape adds the node's world translation
    Vec3 normal;
    int32_t surfaceType{-1};
    float surfaceWeight{0.0F};
};

// Sources reference rectangular regions of CPU tiles, independently of rendering.
// Public methods run on the main thread. Workers only fill their own tile;
// update() publishes completed data, and removeSource() deletes unreferenced tiles.
class LandscapeQuery final {
public:
    // Invoke the completion exactly once, on any thread, including inline.
    using Loader = std::function<void(uint32_t, uint32_t, LandscapeQueryCompletion)>;
    // Landscape validates capacity in [1, 4096] before constructing the query.
    LandscapeQuery(const LandscapeData &data, Loader loader, uint32_t capacity = 64);
    ~LandscapeQuery();
    // Inputs and returned positions are landscape-local; Landscape converts world space.
    // Adding an existing id moves its region, preserving overlapping tiles.
    // Invalid, out-of-bounds or over-capacity requests leave the old source unchanged.
    LandscapeQueryStatus addSource(uint32_t id, LandscapePoint center, float radius);
    void removeSource(uint32_t id);
    LandscapeQueryStatus getSourceStatus(uint32_t id) const;
    void update();
    LandscapeSurfaceResult sample(LandscapePoint local) const;

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
    struct Tile {
        LandscapeQueryTile data;
        uint32_t references{0};
        LandscapeQueryStatus status{LandscapeQueryStatus::NOT_READY};
        // The worker writes data, then publishes it with release ordering.
        // Only after an acquire read may the main thread consume the bytes.
        std::atomic<bool> loaded{false};
    };

    // Check capacity before changing references; other sources retain their tiles.
    LandscapeQueryStatus resolveRegion(LandscapePoint center, float radius, TileRegion &region) const;
    bool canAdmit(const TileRegion &region, const TileRegion *previous) const;

    LandscapeData _data;
    Loader _loader;
    uint32_t _capacity;
    float _tileSize;
    ccstd::unordered_map<uint32_t, TileRegion> _sources;
    ccstd::unordered_map<uint64_t, std::shared_ptr<Tile>> _tiles;
};

} // namespace landscape
} // namespace cc

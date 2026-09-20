// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#include "landscape/VTPaging.h"

#include <cstdlib>
#include <iostream>

using namespace cc::landscape;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void testSyncCache() {
    LandscapeSyncCache cache;
    LandscapeSyncCache::Positions positions{1, 2, 3, 1, 2, 3, 0, 0, 0};
    ccstd::vector<QuadNode> selected{{3, 4, 5, -10, 50, 15}, {4, 7, 8, 0, 90, 3}};
    uint64_t tiles = 4, pages = 0;
    require(!cache.matches(positions, tiles, pages, selected), "First frame must initialize bindings");
    cache.store(positions, tiles, pages, selected);
    // An unmoving camera must still advance streamed tiles and every batch of
    // newly rendered pages. Otherwise it would stay on the root VT forever.
    for (int batch = 0; batch < 32; ++batch) {
        ++pages;
        require(!cache.matches(positions, tiles, pages, selected), "VT publication must refresh bindings");
        cache.store(positions, tiles, pages, selected);
        ++tiles;
        require(!cache.matches(positions, tiles, pages, selected), "Completed source uploads must refresh normals");
        cache.store(positions, tiles, pages, selected);
    }
    for (int frame = 0; frame < 10000; ++frame) {
        require(cache.matches(positions, tiles, pages, selected), "Stable frames must not rebuild sources");
    }
    // F10/F11 invalidation changes VT content before new pages are published.
    require(!cache.matches(positions, tiles, pages + 1, selected), "Material toggle must not reuse stale bindings");
    require(!cache.matches(positions, tiles + 1, pages, selected), "Failed/dropped async completion must allow retries");
    for (size_t i = 0; i < positions.size(); ++i) {
        auto moved = positions;
        moved[i] += 0.001F;
        require(!cache.matches(moved, tiles, pages, selected), "Camera/terrain motion must invalidate selection");
    }
    for (int field = 0; field < 6; ++field) {
        auto changed = selected;
        auto &node = changed[1];
        switch (field) {
            case 0: ++node.level; break;
            case 1: ++node.ix; break;
            case 2: ++node.iz; break;
            case 3: ++node.minY; break;
            case 4: ++node.maxY; break;
            case 5: node.quadrantMask ^= 1; break;
        }
        require(!cache.matches(positions, tiles, pages, changed), "Frustum/LOD/bounds changes must not be missed");
    }
    require(!cache.matches(positions, tiles, pages, {}), "An empty view must retire visible models");
    cache.invalidate();
    require(!cache.matches(positions, tiles, pages, selected), "LOD configuration/reset must invalidate the cache");
    std::cout << "PASS: stable frames, asynchronous residency, VT publication, toggles, movement and selection changes\n";
}

int main() {
    testSyncCache();
    constexpr float sectorSize = 4096.0F;
    const uint32_t root = vtRootLevel(sectorSize);
    require(root == 12, "VT must have meter pages even when geometry has only nine LODs");
    require(vtPageWorldSize(sectorSize, root, 0) == 1.0F, "256 interior texels must cover one meter");
    require(vtPageWorldSize(sectorSize, root, root) == sectorSize, "Root fallback must cover the sector");
    require(vtDesiredLevel(sectorSize, root, 1.5F) == 0, "Near view must request maximum material precision");
    require(vtDesiredLevel(sectorSize, root, 8.0F) == 2, "Material precision must fall independently with distance");

    const auto fine = vtCoveringPage(sectorSize, root, 0, 17, 29, 18, 30);
    require(fine.level == 0 && fine.x == 17 && fine.z == 29, "An unmorphed meter cell must use one fine page");
    const auto morph = vtCoveringPage(sectorSize, root, 0, 16, 28, 18, 30);
    require(morph.level == 1 && morph.x == 8 && morph.z == 14, "A morphing odd cell must use its containing parent");
    const auto coarse = vtCoveringPage(sectorSize, root, 4, 17, 29, 18, 30);
    require(coarse.level == 4 && coarse.x == 1 && coarse.z == 1, "One geometry cell must also support a coarser VT page");

    uint64_t checked = 0;
    // Verify actual shader vertex trajectories against the chosen page. A page
    // is convex, so containment of all three vertices proves containment of
    // every fragment, including independently morphed vertices and diagonals.
    for (uint32_t lod = 0; lod < 9; ++lod) {
        const float cell = static_cast<float>(1U << lod);
        for (uint32_t cells : {1U, 2U, 4U, 8U}) {
            for (uint32_t z = 0; z < 16; z += cells) {
                for (uint32_t x = 0; x < 16; x += cells) {
                    for (uint32_t desired = 0; desired <= root; ++desired) {
                        for (float sectorOrigin : {0.0F, sectorSize * 3.0F}) {
                            const float minX = sectorOrigin + (x - (x & 1U)) * cell;
                            const float minZ = sectorOrigin + (z - (z & 1U)) * cell;
                            const auto page = vtCoveringPage(sectorSize, root, desired,
                                minX, minZ, sectorOrigin + (x + cells) * cell, sectorOrigin + (z + cells) * cell);
                            const float size = vtPageWorldSize(sectorSize, root, page.level);
                            for (uint32_t vz = z; vz <= z + cells; ++vz) {
                                for (uint32_t vx = x; vx <= x + cells; ++vx) {
                                    for (float amount : {0.0F, 0.125F, 0.5F, 0.875F, 1.0F}) {
                                        const float px = sectorOrigin + (vx - (vx & 1U) * amount) * cell;
                                        const float pz = sectorOrigin + (vz - (vz & 1U) * amount) * cell;
                                        require(px >= page.x * size && px <= (page.x + 1) * size &&
                                                pz >= page.z * size && pz <= (page.z + 1) * size,
                                                "A morphed triangle escaped its physical VT page");
                                        ++checked;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // Non-power-of-two sectors retain a dyadic hierarchy without exceeding the
    // requested density. Pages must still line up at distant sector boundaries.
    const auto oddRoot = vtRootLevel(1000.0F);
    require(vtPageWorldSize(1000.0F, oddRoot, 0) >= 1.0F, "Finest page must not exceed 256 texels/m");
    const auto boundary = vtCoveringPage(sectorSize, root, root, 8191, 4095, 8192, 4096);
    require(boundary.x == 1 && boundary.z == 0, "Boundary must remain in its own permanent sector root");
    std::cout << "PASS: VT density, independent levels, fallback boundaries and " << checked << " morph samples\n";
}

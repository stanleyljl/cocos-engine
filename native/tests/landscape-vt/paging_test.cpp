// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#include "landscape/VTPaging.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <random>

using namespace cc::landscape;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// Reference the pre-refactor request loop, including deduplication, priority
// promotion and root order. Exercise one reused planner across changing frames.
void testRequestPlanEquivalence() {
    std::mt19937 generator(0x51A7U);
    const auto random = [&generator]() -> uint32_t { return generator(); };
    VTRequestPlan plan;
    for (uint32_t frame = 0; frame < 400; ++frame) {
        const uint32_t root = 1U + random() % 11U;
        const uint32_t sectorsX = frame % 9U == 0 ? 16U : 1U + random() % 4U;
        const uint32_t sectorsZ = frame % 9U == 0 ? 16U : 1U + random() % 4U;
        const size_t roots = static_cast<size_t>(sectorsX) * sectorsZ;
        ccstd::unordered_map<uint64_t, VTPageRequest> unique;
        plan.begin(root);
        const uint32_t count = frame % 7U == 0 ? 0U : random() % 1200U;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t level = random() % (root + 1U);
            const VTPageAddress visible{level, random() % (sectorsX << (root - level)),
                                       random() % (sectorsZ << (root - level))};
            // Include equal/zero priorities and duplicates with different weights.
            for (float weight : {static_cast<float>(random() % 32U) / 8.0F, 0.5F}) {
                plan.addVisible(visible, weight);
                auto page = visible;
                for (; page.level < root; ++page.level, page.x >>= 1U, page.z >>= 1U) {
                    const float priority = vtAncestorPriority(weight, page.level - visible.level);
                    const uint64_t key = makeNodeKey(page.level, page.x, page.z);
                    auto inserted = unique.emplace(key, VTPageRequest{page, priority, key});
                    inserted.first->second.priority = std::max(inserted.first->second.priority, priority);
                    inserted.first->second.required |= page.level == visible.level;
                }
            }
        }
        ccstd::vector<VTPageRequest> expected;
        for (const auto &entry : unique) expected.push_back(entry.second);
        budgetVTRequests(expected, root, config::VT_PAGE_COUNT - roots);
        const auto dynamicCount = expected.size();
        for (uint32_t z = 0; z < sectorsZ; ++z) {
            for (uint32_t x = 0; x < sectorsX; ++x) {
                expected.push_back({VTPageAddress{root, x, z}, 0.0F, makeNodeKey(root, x, z)});
            }
        }
        plan.finish(sectorsX, sectorsZ);
        require(plan.requests().size() == expected.size(), "Request count changed after refactor");
        require(plan.dynamicKeys().size() == dynamicCount, "Pinned roots entered the dynamic protection set");
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto &a = plan.requests()[i];
            const auto &b = expected[i];
            require(a.key == b.key && a.page.level == b.page.level && a.page.x == b.page.x &&
                    a.page.z == b.page.z && a.priority == b.priority && a.required == b.required,
                    "Request selection/order/priority changed after refactor");
            if (i < dynamicCount) require(plan.dynamicKeys()[i] == a.key, "Protection set does not match selected pages");
        }
    }
    std::cout << "PASS: request planner matches the original loop across 400 changing frames\n";
}

void testNearGroundCoverage() {
    constexpr float sectorSize = 2048.0F;
    const auto root = vtRootLevel(sectorSize);
    constexpr size_t capacity = config::VT_PAGE_COUNT - 4U;
    // Near-ground 64 x 64 m view with meter cells. Triplanar density increases
    // demand beyond the physical cache; every visible cell still needs coverage.
    for (float density : {1.0F, 4.0F}) {
        std::map<uint64_t, VTPageRequest> unique;
        for (uint32_t z = 0; z < 64; ++z) {
            for (uint32_t x = 0; x < 64; ++x) {
                const float dx = x - 31.5F, dz = z - 31.5F;
                const float distance = std::sqrt(dx * dx + dz * dz + 1.7F * 1.7F);
                const auto desired = vtDesiredLevel(sectorSize, root, distance / density);
                const float priority = density / std::max(distance, 1.0F);
                for (auto level = desired; level < root; ++level) {
                    const VTPageAddress page{level, x >> level, z >> level};
                    const auto key = makeNodeKey(level, page.x, page.z);
                    auto &request = unique[key];
                    request.page = page;
                    request.key = key;
                    request.priority = std::max(request.priority, vtAncestorPriority(priority, level - desired));
                    request.required |= level == desired;
                }
            }
        }
        ccstd::vector<VTPageRequest> requests, required;
        for (const auto &entry : unique) {
            requests.push_back(entry.second);
            if (entry.second.required) required.push_back(entry.second);
        }
        const auto maximumGap = [&required, root](const ccstd::vector<VTPageRequest> &resident) {
            ccstd::unordered_set<uint64_t> keys;
            for (const auto &r : resident) keys.insert(r.key);
            uint32_t worst = 0;
            for (const auto &r : required) {
                auto page = r.page;
                while (page.level < root && keys.count(makeNodeKey(page.level, page.x, page.z)) == 0U) {
                    ++page.level;
                    page.x >>= 1U;
                    page.z >>= 1U;
                }
                worst = std::max(worst, page.level - r.page.level);
            }
            return worst;
        };
        if (density > 1.0F) {
            auto truncated = requests;
            std::sort(truncated.begin(), truncated.end(), [](const auto &a, const auto &b) {
                return a.priority != b.priority ? a.priority > b.priority : a.key < b.key;
            });
            truncated.resize(capacity);
            require(maximumGap(truncated) >= 8U, "Regression fixture must expose root-only fallback in old truncation");
        }
        auto empty = requests;
        budgetVTRequests(empty, root, 0);
        require(empty.empty(), "A zero dynamic budget must retain only permanent roots");
        budgetVTRequests(requests, root, capacity);
        require(requests.size() <= capacity, "Coverage must not enlarge the physical cache");
        require(maximumGap(requests) <= 2U, "Near-ground view lost intermediate coverage under cache pressure");
        if (density == 1.0F) require(maximumGap(requests) == 0U, "A fitting working set must retain full detail");
        else {
            auto initialBatch = requests;
            initialBatch.resize(capacity / 2U);
            require(maximumGap(initialBatch) <= 2U, "Coverage must be composed before optional detail pages");
        }
    }
    std::cout << "PASS: near-ground cache overflow retains view-wide coverage and prioritizes its publication\n";
}

void testSyncCache() {
    LandscapeSyncCache cache;
    LandscapeSyncCache::Positions positions{1, 2, 3, 1, 2, 3, 0, 0, 0};
    ccstd::vector<QuadNode> selected{{3, 4, 5, -10, 50, 15}, {4, 7, 8, 0, 90, 3}};
    uint64_t tiles = 4, pages = 0;
    require(!cache.matches(positions, tiles, pages, selected, selected), "First frame must initialize bindings");
    cache.store(positions, tiles, pages, selected, selected);
    // An unmoving camera must still advance streamed tiles and every batch of
    // newly rendered pages. Otherwise it would stay on the root VT forever.
    for (int batch = 0; batch < 32; ++batch) {
        ++pages;
        require(!cache.matches(positions, tiles, pages, selected, selected), "VT publication must refresh bindings");
        cache.store(positions, tiles, pages, selected, selected);
        ++tiles;
        require(!cache.matches(positions, tiles, pages, selected, selected), "Completed source uploads must refresh normals");
        cache.store(positions, tiles, pages, selected, selected);
    }
    for (int frame = 0; frame < 10000; ++frame) {
        require(cache.matches(positions, tiles, pages, selected, selected), "Stable frames must not rebuild sources");
    }
    // F10/F11 invalidation changes VT content before new pages are published.
    require(!cache.matches(positions, tiles, pages + 1, selected, selected), "Material toggle must not reuse stale bindings");
    require(!cache.matches(positions, tiles + 1, pages, selected, selected), "Failed/dropped async completion must allow retries");
    for (size_t i = 0; i < positions.size(); ++i) {
        auto moved = positions;
        moved[i] += 0.001F;
        require(!cache.matches(moved, tiles, pages, selected, selected), "Camera/terrain motion must invalidate selection");
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
        require(!cache.matches(positions, tiles, pages, changed, selected), "Frustum/LOD/bounds changes must not be missed");
    }
    require(!cache.matches(positions, tiles, pages, {}, {}), "An empty view must retire visible models");
    auto surface = selected;
    surface[0].quadrantMask = 1;
    require(!cache.matches(positions, tiles, pages, selected, surface),
            "A camera rotation can change material coverage while shadow geometry stays unchanged");
    cache.store(positions, tiles, pages, selected, surface);
    require(cache.matches(positions, tiles, pages, selected, surface), "Stable split plans must reuse bindings");
    require(!cache.matches(positions, tiles, pages, selected, {}), "Losing the last color pass must release material detail");
    require(!cache.matches(positions, tiles, pages, selected, selected), "Newly visible shadow geometry must acquire material bindings");
    cache.invalidate();
    require(!cache.matches(positions, tiles, pages, selected, selected), "LOD configuration/reset must invalidate the cache");
    std::cout << "PASS: stable frames, asynchronous residency, VT publication, toggles, movement and selection changes\n";
}

void testPassResourceSelection() {
    const ccstd::vector<QuadNode> camera{{2, 4, 3, -10, 20, 0x1}, {1, 0, 1, 0, 5, 0xF}};
    const ccstd::vector<QuadNode> shadow{{3, 7, 2, -3, 40, 0xF}, {2, 4, 3, -10, 20, 0x6}};
    auto resources = camera;
    resources.insert(resources.end(), shadow.begin(), shadow.end());
    mergeNodeSelections(resources);
    require(resources.size() == 3, "Overlapping pass nodes must share one resource entry");
    const auto find = [&](const QuadNode &node) -> const QuadNode & {
        const auto it = std::find_if(resources.begin(), resources.end(), [&](const QuadNode &candidate) {
            return makeNodeKey(candidate) == makeNodeKey(node);
        });
        require(it != resources.end(), "Camera-only and shadow-only nodes must both retain resources");
        return *it;
    };
    require(find(camera[0]).quadrantMask == 0x7, "Shared resources must cover both passes' quadrants");
    require(find(camera[1]).quadrantMask == 0xF, "Camera-only node lost its coverage");
    require(find(shadow[0]).quadrantMask == 0xF, "Offscreen shadow caster lost its coverage");
    require(camera[0].quadrantMask == 0x1 && shadow[1].quadrantMask == 0x6,
            "Resource merging must not widen either pass's draw selection");

    auto reversed = shadow;
    reversed.insert(reversed.end(), camera.begin(), camera.end());
    reversed.insert(reversed.end(), shadow.begin(), shadow.end());
    mergeNodeSelections(reversed);
    require(reversed.size() == resources.size(), "Repeated pass requests must be idempotent");
    for (size_t i = 0; i < resources.size(); ++i) {
        require(makeNodeKey(reversed[i]) == makeNodeKey(resources[i]) &&
                    reversed[i].quadrantMask == resources[i].quadrantMask,
                "Pass collection order must not invalidate the resource cache");
    }
    ccstd::vector<QuadNode> empty;
    mergeNodeSelections(empty);
    require(empty.empty(), "Empty pass batches must remain empty");
    auto surface = camera;
    mergeNodeSelections(surface);
    ccstd::vector<QuadNode> shadowOnly;
    collectShadowOnlyNodes(resources, surface, shadowOnly);
    require(shadowOnly.size() == 2, "Camera-only nodes must not allocate shadow-only models");
    require(makeNodeKey(shadowOnly[0]) == makeNodeKey(camera[0]) && shadowOnly[0].quadrantMask == 0x6,
            "Subtract material coverage per quadrant, not per whole node");
    require(makeNodeKey(shadowOnly[1]) == makeNodeKey(shadow[0]) && shadowOnly[1].quadrantMask == 0xF,
            "Offscreen casters must retain complete geometry without material requests");
    collectShadowOnlyNodes(resources, resources, shadowOnly);
    require(shadowOnly.empty(), "Fully shared geometry must reuse color models without duplicate casters");
    collectShadowOnlyNodes(resources, {}, shadowOnly);
    require(shadowOnly.size() == resources.size(), "A shadow-only batch must retain all geometry");
    collectShadowOnlyNodes({}, surface, shadowOnly);
    require(shadowOnly.empty(), "Empty geometry must clear previous shadow-only requests");
    std::cout << "PASS: independent pass selections, shared quadrants and offscreen shadow resources\n";
}

int main() {
    testPassResourceSelection();
    testRequestPlanEquivalence();
    testNearGroundCoverage();
    testSyncCache();
    constexpr float sectorSize = 4096.0F;
    const uint32_t root = vtRootLevel(sectorSize);
    require(root == 12, "VT must have meter pages even when geometry has only nine LODs");
    require(vtPageWorldSize(sectorSize, root, 0) == 1.0F, "256 interior texels must cover one meter");
    require(vtPageWorldSize(sectorSize, root, root) == sectorSize, "Root fallback must cover the sector");
    require(vtDesiredLevel(sectorSize, root, 1.5F) == 0, "Near view must request maximum material precision");
    require(vtDesiredLevel(sectorSize, root, 8.0F) == 2, "Material precision must fall independently with distance");

    LandscapeData reference;
    reference.sectorsX = reference.sectorsZ = 2;
    reference.maxLevel = 7;
    reference.minTileLevel = 3;
    require(cliffReferenceLevel(reference) == 4, "4 km reference must fit 256 source tiles at 2 m spacing");
    for (uint32_t sectors : {1U, 2U, 4U, 8U, 16U}) {
        reference.sectorsX = reference.sectorsZ = sectors;
        const auto level = cliffReferenceLevel(reference);
        const auto count = static_cast<uint64_t>(sectors) * sectors * (1ULL << (2U * (reference.maxLevel - level)));
        require(count <= config::PAGE_POOL_LAYERS / 4U, "Cliff references exceeded cache budget");
        require(level >= reference.minTileLevel && level <= reference.maxLevel, "Invalid reference level");
    }
    require(cliffDensityScale(0, 16) == 1, "Flat patches must retain baseline VT density");
    require(cliffDensityScale(1000, 1) == 4, "Cliff refinement must be capped at two levels");
    require(cliffDensityScale(16, 16) > 1 && cliffDensityScale(16, 16) < 2, "Slope density estimate must be continuous");

    // A narrow 64 m wall in a 16 m cell must not disappear from the density
    // estimate when CDLOD encloses it in a mostly flat 512 m distant node.
    float inherited = cliffDensityScale(64, 16);
    for (float width : {32.0F, 64.0F, 128.0F, 256.0F, 512.0F}) {
        inherited = cliffDensityScale(64, width, inherited);
        require(inherited == 4, "Coarse regions must preserve steep child density");
    }
    const auto oldCliffLevel = vtDesiredLevel(sectorSize, root, 1000 / cliffDensityScale(64, 512));
    const auto newCliffLevel = vtDesiredLevel(sectorSize, root, 1000 / inherited);
    require(oldCliffLevel == newCliffLevel + 2, "Distant narrow wall must retain two extra levels");
    require(cliffDensityScale(0, 512, cliffDensityScale(0, 16)) == 1, "Flat hierarchy must not request extra pages");

    // Within the optional detail budget, direct requests outrank redundant
    // ancestors. The separate coverage test above validates cache truncation.
    struct PriorityRequest { float priority; bool direct; };
    ccstd::vector<PriorityRequest> candidates;
    constexpr size_t capacity = config::VT_PAGE_COUNT - 4;
    for (size_t i = 0; i < capacity; ++i) candidates.push_back({vtAncestorPriority(1.0F, 0), true});
    for (uint32_t ancestor = 1; ancestor <= 8; ++ancestor) {
        candidates.push_back({vtAncestorPriority(1.0F, ancestor), false});
        require(vtAncestorPriority(1.0F, ancestor) < vtAncestorPriority(1.0F, ancestor - 1),
                "Coarser fallback must not inflate visible coverage priority");
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.priority > b.priority; });
    candidates.resize(capacity);
    require(std::all_of(candidates.begin(), candidates.end(), [](const auto &r) { return r.direct; }),
            "Raw refinement priority must favor direct requests over redundant ancestors");
    require(vtAncestorPriority(4.0F, 1) > vtAncestorPriority(0.25F, 0),
            "Important nearby fallback should still outrank tiny distant coverage");

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

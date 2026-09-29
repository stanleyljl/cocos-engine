// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#include "landscape/VTPaging.h"
#include "landscape/VirtualTexture.h"
#include "landscape/LandscapeRenderer.h"
#include "landscape/VTRenderer.h"

// Access internal CPU state without making it part of the engine's public API.
namespace cc::landscape {
struct LandscapePagingTestAccess {
    using SyncCache = LandscapeRenderer::SyncCache;
    static uint32_t cliffReferenceLevel(const LandscapeData &data) {
        return VTRenderer::cliffReferenceLevel(data);
    }
};
}

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

// Test-only oracle for the removed planner's admission policy.
struct ReferenceRequest {
    VTPageAddress page;
    float priority{0};
    bool required{false};
};

LandscapeData cacheData(uint32_t root, uint32_t sectorsX = 1, uint32_t sectorsZ = 1) {
    LandscapeData data;
    data.sectorsX = sectorsX;
    data.sectorsZ = sectorsZ;
    data.maxLevel = std::min(root, config::MAX_LOD_LEVELS - 1);
    data.tileResolution = 129;
    data.sectorSize = static_cast<float>(1U << root);
    data.heightScale = 1;
    return data;
}

ccstd::vector<ReferenceRequest> activeRequests(const VirtualTexture &cache, uint32_t root) {
    ccstd::vector<ReferenceRequest> result;
    for (uint32_t slot : cache.activeSlots()) {
        const auto &p = cache.page(slot);
        if (p.address.level < root) result.push_back({p.address});
    }
    return result;
}

// Frozen pre-refactor oracle, independent of the production planner's helpers.
inline float referenceAncestorPriority(float requiredPagePriority, uint32_t ancestorSteps) {
    // Ancestors provide fallback, not additional visible coverage. Recomputing
    // size/distance for them incorrectly promotes bigger, blurrier pages ahead
    // of the fine page actually requested by the visible patch.
    return std::ldexp(requiredPagePriority, -static_cast<int>(std::min(ancestorSteps, 27U)));
}

// Find a common ancestor bias whose view-wide coverage fits the reserved budget.
// Sector roots need no dynamic slots and are omitted from this set.
inline ccstd::unordered_set<uint64_t> referenceCoveragePages(
    const ccstd::vector<ReferenceRequest> &requests, uint32_t rootLevel, size_t capacity) {
    ccstd::unordered_set<uint64_t> coverage;
    for (uint32_t bias = 0; bias <= rootLevel; ++bias) {
        coverage.clear();
        for (const auto &request : requests) {
            if (!request.required) continue;
            const auto &page = request.page;
            const auto level = std::min(page.level + bias, rootLevel);
            if (level == rootLevel) continue;
            const auto shift = level - page.level;
            coverage.insert(makeNodeKey(level, page.x >> shift, page.z >> shift));
        }
        if (coverage.size() <= capacity) break;
    }
    return coverage;
}

inline void referenceBudget(ccstd::vector<ReferenceRequest> &requests, uint32_t rootLevel, size_t capacity) {
    if (capacity == 0U) {
        requests.clear();
        return;
    }
    if (requests.size() > capacity) {
        // Detail-first truncation alone can drop EVERY ancestor of a visible
        // patch. Its only remaining fallback is then a whole-sector root page.
        // Reserve a bounded, view-wide coverage set before spending on detail.
        // All non-root ancestors already exist in the input request set.
        const size_t coverageBudget = std::max(size_t{1}, capacity / 2U);
        const auto coverage = referenceCoveragePages(requests, rootLevel, coverageBudget);
        float maximumPriority = 0.0F;
        for (const auto &request : requests) maximumPriority = std::max(maximumPriority, request.priority);
        for (auto &request : requests) {
            // Also compose coverage before refinement under the per-frame
            // update budget, so movement does not leave lasting root fallbacks.
            if (coverage.count(request.page.key()) != 0U) request.priority += maximumPriority + 1.0F;
        }
    }
    std::sort(requests.begin(), requests.end(), [](const auto &a, const auto &b) {
        return a.priority != b.priority ? a.priority > b.priority : a.page.key() < b.page.key();
    });
    if (requests.size() > capacity) requests.resize(capacity);
}


// Reference the pre-refactor request loop, including deduplication, priority
// promotion and root order. Exercise one reused planner across changing frames.
void testRequestPlanEquivalence() {
    std::mt19937 generator(0x51A7U);
    const auto random = [&generator]() -> uint32_t { return generator(); };
    VirtualTexture cache;
    for (uint32_t frame = 0; frame < 400; ++frame) {
        const uint32_t root = 1U + random() % 11U;
        const uint32_t sectorsX = frame % 9U == 0 ? 16U : 1U + random() % 4U;
        const uint32_t sectorsZ = frame % 9U == 0 ? 16U : 1U + random() % 4U;
        const size_t roots = static_cast<size_t>(sectorsX) * sectorsZ;
        ccstd::unordered_map<uint64_t, ReferenceRequest> unique;
        require(cache.init(cacheData(root, sectorsX, sectorsZ)), "Cache initialization failed");
        cache.beginRequests();
        const uint32_t count = frame % 7U == 0 ? 0U : random() % 1200U;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t level = random() % (root + 1U);
            const VTPageAddress visible{level, random() % (sectorsX << (root - level)),
                                       random() % (sectorsZ << (root - level))};
            // Include equal/zero priorities and duplicates with different weights.
            for (float weight : {static_cast<float>(random() % 32U) / 8.0F, 0.5F}) {
                cache.request(visible, weight);
                auto page = visible;
                for (; page.level < root; ++page.level, page.x >>= 1U, page.z >>= 1U) {
                    const float priority = referenceAncestorPriority(weight, page.level - visible.level);
                    const uint64_t key = makeNodeKey(page.level, page.x, page.z);
                    auto inserted = unique.emplace(key, ReferenceRequest{page, priority});
                    inserted.first->second.priority = std::max(inserted.first->second.priority, priority);
                    inserted.first->second.required |= page.level == visible.level;
                }
            }
        }
        ccstd::vector<ReferenceRequest> expected;
        for (const auto &entry : unique) expected.push_back(entry.second);
        referenceBudget(expected, root, config::VT_PAGE_COUNT - roots);
        cache.endRequests();
        const auto actual = activeRequests(cache, root);
        require(actual.size() == expected.size(), "Admission count changed");
        require(cache.activeSlots().size() == actual.size() + roots, "All roots must remain active");
        require(cache.activeSlots().size() <= config::VT_PAGE_COUNT, "Physical capacity exceeded");
        for (size_t i = 0; i < expected.size(); ++i) {
            require(actual[i].page.key() == expected[i].page.key(), "Coverage/refinement admission order changed");
        }
    }
    std::cout << "PASS: request planner matches the original loop across 400 changing frames\n";
}

void testNearGroundCoverage() {
    constexpr float sectorSize = 2048.0F;
    const VTPageLayout pageLayout(sectorSize);
    const auto root = pageLayout.rootLevel();
    constexpr size_t capacity = config::VT_PAGE_COUNT - 4U;
    // Near-ground 64 x 64 m view with meter cells. Triplanar density increases
    // demand beyond the physical cache; every visible cell still needs coverage.
    for (float density : {1.0F, 4.0F}) {
        std::map<uint64_t, ReferenceRequest> unique;
        for (uint32_t z = 0; z < 64; ++z) {
            for (uint32_t x = 0; x < 64; ++x) {
                const float dx = x - 31.5F, dz = z - 31.5F;
                const float distance = std::sqrt(dx * dx + dz * dz + 1.7F * 1.7F);
                const auto desired = pageLayout.levelForDistance(distance / density);
                const float priority = density / std::max(distance, 1.0F);
                for (auto level = desired; level < root; ++level) {
                    const VTPageAddress page{level, x >> level, z >> level};
                    const auto key = makeNodeKey(level, page.x, page.z);
                    auto &request = unique[key];
                    request.page = page;
                    request.priority = std::max(request.priority, referenceAncestorPriority(priority, level - desired));
                    request.required |= level == desired;
                }
            }
        }
        ccstd::vector<ReferenceRequest> requests, required;
        VirtualTexture cache;
        require(cache.init(cacheData(root, 2, 2)), "Cache initialization failed");
        cache.beginRequests();
        for (const auto &entry : unique) {
            if (entry.second.required) cache.request(entry.second.page, entry.second.priority);
        }
        for (const auto &entry : unique) {
            requests.push_back(entry.second);
            if (entry.second.required) required.push_back(entry.second);
        }
        const auto maximumGap = [&required, root](const ccstd::vector<ReferenceRequest> &resident) {
            ccstd::unordered_set<uint64_t> keys;
            for (const auto &r : resident) keys.insert(r.page.key());
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
                return a.priority != b.priority ? a.priority > b.priority : a.page.key() < b.page.key();
            });
            truncated.resize(capacity);
            require(maximumGap(truncated) >= 8U, "Regression fixture must expose root-only fallback in old truncation");
        }
        cache.endRequests();
        requests = activeRequests(cache, root);
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

VTPageInputs pageInputs(const VirtualTexture &cache, uint32_t slot, const LandscapeData &data, float version = 0) {
    const auto address = cache.page(slot).address;
    const VTPageLayout layout(data.sectorSize);
    const auto region = data.regionAtGrid(layout.pageOrigin(address), layout.pageSize(address.level));
    VTPageInputs inputs;
    inputs.region = cc::Vec4{region.x, region.z, region.size, 0};
    inputs.splatSource = cc::Vec4{region.x, region.z, region.size, version};
    inputs.normalSources.fill(inputs.splatSource);
    return inputs;
}

void resolveInputs(VirtualTexture &cache, const LandscapeData &data) {
    for (uint32_t slot : cache.activeSlots()) cache.setInputs(slot, pageInputs(cache, slot, data));
}

void testPageLifecycle() {
    const auto data = cacheData(3);
    VirtualTexture cache;
    require(!cache.init(data, 0), "Zero capacity must fail");
    require(!cache.init(cacheData(3, 2, 2), 3), "Roots must fit before initialization");
    require(cache.init(data, 8), "Cache initialization failed");
    const VTPageAddress fine{0, 1, 1};
    ccstd::vector<uint32_t> updates;
    require(!cache.ready() && cache.resolve(fine) < 0, "Unbaked roots must not be sampleable");
    cache.collectUpdates(updates, 8);
    require(updates.empty(), "Unresolved roots must not compose");
    resolveInputs(cache, data);
    cache.collectUpdates(updates, 0);
    require(updates.size() == 1 && updates[0] == 0, "Root publication must bypass detail budget");
    cache.publish(updates);
    require(cache.ready() && cache.resolve(fine) == 0, "Root fallback missing");

    cache.beginRequests();
    cache.request(fine, 1);
    cache.endRequests();
    require(cache.activeSlots().size() == 4, "Expected root, fine page and two ancestors");
    cache.collectUpdates(updates, 8);
    require(updates.empty(), "Admission must not reuse unresolved source layers");
    require(cache.resolve(fine) == 0, "Dirty fine page must fall back to ready root");
    resolveInputs(cache, data);
    cache.collectUpdates(updates, 1);
    require(updates.size() == 1 && cache.page(updates[0]).address.key() == fine.key(),
            "Direct demand must precede optional ancestors");
    const int fineSlot = static_cast<int>(updates[0]);
    const auto revision = cache.revision();
    cache.publish(updates);
    require(cache.resolve(fine) == fineSlot && cache.revision() > revision,
            "Fine mapping must be available immediately after publication");

    // Ready content survives input re-resolution only when the actual inputs match.
    cache.setInputs(fineSlot, pageInputs(cache, fineSlot, data));
    require(cache.resolve(fine) == fineSlot, "Unchanged inputs must preserve ready content");
    auto changed = pageInputs(cache, fineSlot, data);
    changed.normalSources[15].w += 1;
    cache.setInputs(fineSlot, changed);
    require(cache.resolve(fine) == 0, "Changed normal source must invalidate fine content");
    cache.collectUpdates(updates, 0);
    require(updates.empty(), "Zero detail budget must defer fine recomposition");

    cache.invalidate();
    require(cache.resolve(fine) < 0, "Invalidated roots cannot be bound before rendering");
    cache.collectUpdates(updates, 0);
    require(updates.size() == 1 && updates[0] == 0, "Dirty roots must still fit zero detail budget");
    cache.publish(updates);
    cache.collectUpdates(updates, 8);
    cache.publish(updates);
    require(cache.ready(), "All resolved pages should be ready after full publication");

    // End a frame without requests: cached but inactive fine pages are not mappings.
    cache.beginRequests();
    cache.endRequests();
    require(cache.activeSlots().size() == 1 && cache.resolve(fine) == 0,
            "Inactive pages must not leak into current mappings");
    cache.beginRequests();
    cache.request({99, 0, 0}, 1);
    cache.request({0, 999, 0}, 1);
    cache.request(fine, std::numeric_limits<float>::infinity());
    cache.endRequests();
    require(cache.activeSlots().size() == 1, "Invalid demands must not enter the cache");

    require(cache.init(data, 1), "Root-only capacity must be supported");
    cache.beginRequests();
    cache.request(fine, 1);
    cache.endRequests();
    resolveInputs(cache, data);
    cache.collectUpdates(updates, 0);
    cache.publish(updates);
    require(cache.activeSlots().size() == 1 && cache.resolve(fine) == 0,
            "Root-only cache must retain complete fallback coverage");
    std::cout << "PASS: real VT lifecycle, unresolved inputs, invalidation, publication and root-only capacity\n";
}

void testCacheStreaming() {
    constexpr uint32_t ROOT = 5, CAPACITY = 32, ROOTS = 4;
    const auto data = cacheData(ROOT, 2, 2);
    VirtualTexture cache;
    require(cache.init(data, CAPACITY), "Cache initialization failed");
    std::mt19937 random(0xCA5E123U);
    ccstd::vector<uint32_t> updates;
    for (uint32_t frame = 0; frame < 1200; ++frame) {
        ccstd::vector<VTPageAddress> demands;
        cache.beginRequests();
        for (uint32_t i = 0, count = random() % 100; i < count; ++i) {
            const uint32_t level = random() % ROOT;
            const uint32_t side = 2U << (ROOT - level);
            const VTPageAddress page{level, static_cast<uint32_t>(random() % side), static_cast<uint32_t>(random() % side)};
            demands.push_back(page);
            cache.request(page, static_cast<float>(i % 7));
        }
        cache.endRequests();
        require(cache.activeSlots().size() <= CAPACITY, "Cache exceeded its physical slots");
        ccstd::unordered_set<uint32_t> active;
        ccstd::unordered_map<uint64_t, uint32_t> admitted;
        for (uint32_t slot : cache.activeSlots()) {
            require(slot < CAPACITY && active.insert(slot).second, "Duplicate or invalid active slot");
            const auto &p = cache.page(slot);
            require(p.state != VirtualTexture::State::EMPTY && admitted.emplace(p.address.key(), slot).second,
                    "Duplicate page identity or empty active page");
            if (slot < ROOTS) {
                require(p.address.level == ROOT && p.address.x == slot % 2 && p.address.z == slot / 2,
                        "Permanent root slot was evicted");
            }
            // Intentionally leave some fine inputs unresolved to model source preparation.
            if (slot < ROOTS || random() % 3 != 0) {
                cache.setInputs(slot, pageInputs(cache, slot, data, static_cast<float>(frame / 17)));
            }
        }
        if (frame % 23 == 0) cache.invalidate();
        const auto budget = frame % 4;
        cache.collectUpdates(updates, budget);
        uint32_t details = 0;
        for (uint32_t slot : updates) {
            const auto &p = cache.page(slot);
            require(active.count(slot) && p.inputsResolved && p.state == VirtualTexture::State::DIRTY,
                    "Scheduled stale, inactive or ready page");
            if (slot >= ROOTS) ++details;
        }
        require(details <= budget, "Composition exceeded detail budget");
        cache.publish(updates);
        for (auto wanted : demands) {
            int expected = -1;
            for (auto p = wanted; p.level <= ROOT; p = p.parent()) {
                const auto it = admitted.find(p.key());
                if (it != admitted.end() && cache.page(it->second).state == VirtualTexture::State::READY) {
                    expected = static_cast<int>(it->second);
                    break;
                }
            }
            require(expected >= 0 && cache.resolve(wanted) == expected,
                    "Mapping is not the finest active ready ancestor");
        }

        // Request order changes must not let an early allocation evict a later hit.
        const auto previous = admitted;
        cache.beginRequests();
        for (size_t i = demands.size(); i > 0; --i) {
            cache.request(demands[i - 1], static_cast<float>((i - 1) % 7));
        }
        cache.endRequests();
        require(previous.size() == cache.activeSlots().size(), "Demand permutation changed admission");
        for (uint32_t slot : cache.activeSlots()) {
            const auto it = previous.find(cache.page(slot).address.key());
            require(it != previous.end() && it->second == slot, "Admitted page was evicted during ordered allocation");
        }
    }
    std::cout << "PASS: 1200 streaming frames with real eviction, capacity, publication and mapping checks\n";
}

void testSyncCache() {
    LandscapePagingTestAccess::SyncCache cache;
    LandscapePagingTestAccess::SyncCache::Positions positions{1, 2, 3, 0, 0, 0};
    ccstd::vector<QuadNode> selected{{3, 4, 5, -10, 50, 15}, {4, 7, 8, 0, 90, 3}};
    uint64_t tiles = 4, pages = 0;
    require(!cache.matches(positions, tiles, pages, selected, selected), "First frame must initialize bindings");
    cache.store(positions, tiles, pages, selected, selected);
    // An unmoving camera must still advance streamed tiles and every batch of
    // completed material pages. Otherwise it would stay on the root VT forever.
    for (int batch = 0; batch < 32; ++batch) {
        ++pages;
        require(!cache.matches(positions, tiles, pages, selected, selected), "Completed VT pages must refresh direct instance mappings");
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

void testCoordinateLayouts() {
    const auto invalid = NodeRangeLayout::INVALID_INDEX;
    require(NodeRangeLayout{}.nodeCount() == 0, "Default layout must be empty");
    require(NodeRangeLayout{}.globalNodeIndex(0, 0, 0) == invalid, "Empty layout must reject addresses");
    for (uint32_t root = 0; root < config::MAX_LOD_LEVELS; ++root) {
        LandscapeData data;
        data.sectorsX = 2; data.sectorsZ = 3; data.maxLevel = root; data.sectorSize = 1000;
        const NodeRangeLayout layout(data);
        size_t expected = 0;
        // Independently enumerate the serialized storage order. Every address
        // must map exactly once, and local/global addressing must agree.
        for (uint32_t sz = 0; sz < 3; ++sz) for (uint32_t sx = 0; sx < 2; ++sx) {
            uint32_t side = 1U << root;
            for (uint32_t level = 0; level <= root; ++level, side /= 2U) {
                require(data.nodesPerSectorSide(level) == side, "Wrong geometry level dimensions");
                require(data.nodeSize(level) * side == 1000, "Node sizes do not cover the sector");
                for (uint32_t z = 0; z < side; ++z) for (uint32_t x = 0; x < side; ++x) {
                    const auto address = data.nodeInSector(sx, sz, level, x, z);
                    require(layout.sectorNodeIndex(sx, sz, level, x, z) == expected, "Sector storage order changed");
                    require(layout.globalNodeIndex(level, address.x, address.z) == expected++, "Global storage order changed");
                    const auto decoded = NodeAddress::fromKey(address.key());
                    require(decoded.level == level && decoded.x == address.x && decoded.z == address.z, "Node key round trip failed");
                    const auto ancestor = address.ancestor(root);
                    require(ancestor.x == sx && ancestor.z == sz, "Ancestor crossed a sector");
                    const auto region = data.nodeRegion(address);
                    const auto center = data.localToGrid({region.x + region.size / 2, region.z + region.size / 2});
                    require(data.nodeAtGridClamped(level, center).key() == address.key(), "Node/local/grid round trip failed");
                }
                require(layout.sectorNodeIndex(sx, sz, level, side, 0) == invalid, "Local X overflow accepted");
                require(layout.sectorNodeIndex(sx, sz, level, 0, side) == invalid, "Local Z overflow accepted");
            }
        }
        require(layout.nodeCount() == expected, "Flat storage contains holes");
        require(layout.globalNodeIndex(root + 1, 0, 0) == invalid, "Invalid level accepted");
        require(layout.globalNodeIndex(root, 2, 0) == invalid, "Global X overflow accepted");
        require(layout.globalNodeIndex(root, 0, 3) == invalid, "Global Z overflow accepted");
        require(data.nodeSize(root + 1) == 0, "Invalid node level must have zero size");
        const auto nearCorner = data.localToGrid({-1000, -1500});
        const auto farCorner = data.localToGrid({1000, 1500});
        require(nearCorner.x == 0 && nearCorner.z == 0 && farCorner.x == 2000 && farCorner.z == 3000,
                "Non-square landscape center/corner conversion failed");
        require(data.containsGridPoint(nearCorner) && data.containsGridPoint(farCorner), "Outer edge must be sampleable");
        require(!data.containsGridPoint({-0.001, 0}) && !data.containsGridPoint({2000.001, 0}), "Outside sample accepted");
        const auto last = data.nodeAtGridClamped(0, farCorner);
        require(last.x == data.nodesX(0) - 1 && last.z == data.nodesZ(0) - 1, "Far edge must select the last tile");
        require(data.nodeAtGridClamped(0, {-1, -1}).key() == NodeAddress{0, 0, 0}.key(), "Gutter probe must clamp");
        data.maxLevel = config::MAX_LOD_LEVELS;
        require(NodeRangeLayout(data).nodeCount() == 0, "Invalid LOD must produce an empty layout");
        require(layout.nodeCount() == expected, "Layout must own its dimension snapshot");
    }
    const VTPageLayout fractional(1000);
    require(fractional.rootLevel() == 9 && fractional.pageSize(0) == 1.953125F, "Non-power-of-two VT layout changed");
    require(fractional.pageSize(10) == 0, "Invalid VT level must not underflow a shift");
    std::cout << "PASS: coordinate layouts, all geometry levels, non-square sectors and boundary round trips\n";
}

void testAtlasFilterFootprint() {
    const float interior = static_cast<float>(config::VT_PAGE_INTERIOR);
    const float border = static_cast<float>(config::VT_PAGE_BORDER);
    const float margin = static_cast<float>(config::VT_FILTER_MARGIN);
    const float slot = static_cast<float>(config::VT_PAGE_RES);
    std::mt19937 random(0xA41504U);
    std::uniform_real_distribution<float> uv(0.0F, 1.0F);
    std::uniform_real_distribution<float> direction(-1.0F, 1.0F);
    // Mirror the shader in base-level texels. Include exact edges/corners and
    // huge gradients caused by grazing views or a coarse resident fallback.
    for (uint32_t probe = 0; probe < 20000; ++probe) {
        const float u = probe % 4 == 0 ? 0.0F : probe % 4 == 1 ? 1.0F : uv(random);
        const float v = probe % 5 == 0 ? 0.0F : probe % 5 == 1 ? 1.0F : uv(random);
        const float magnitude = std::ldexp(1.0F, static_cast<int>(probe % 17) - 4);
        const float dxU = direction(random) * magnitude, dxV = direction(random) * magnitude;
        const float dyU = direction(random) * magnitude, dyV = direction(random) * magnitude;
        const float roomU = border + std::min(u, 1.0F - u) * interior - margin;
        const float roomV = border + std::min(v, 1.0F - v) * interior - margin;
        const float spanU = std::abs(dxU) + std::abs(dyU);
        const float spanV = std::abs(dxV) + std::abs(dyV);
        const float scale = std::min({1.0F, 2.0F * std::max(roomU, 0.0F) / std::max(spanU, 1e-6F),
                                     2.0F * std::max(roomV, 0.0F) / std::max(spanV, 1e-6F)});
        require(std::isfinite(scale) && scale > 0.0F && scale <= 1.0F, "Invalid atlas gradient scale");
        if (spanU <= 2.0F * roomU && spanV <= 2.0F * roomV) {
            require(scale == 1.0F, "Safe footprints must retain their original gradients");
        }
        for (uint32_t mip = 0; mip < config::VT_MIP_LEVELS; ++mip) {
            const float support = static_cast<float>(1U << mip);
            const float centerU = border + u * interior, centerV = border + v * interior;
            const float extentU = spanU * scale * 0.5F + support;
            const float extentV = spanV * scale * 0.5F + support;
            require(centerU - extentU >= -1e-4F && centerU + extentU <= slot + 1e-4F &&
                    centerV - extentV >= -1e-4F && centerV + extentV <= slot + 1e-4F,
                    "Filter footprint plus mip support escaped the physical VT slot");
        }
    }
    std::cout << "PASS: 20000 atlas filter footprints across all mips\n";
}

int main() {
    testAtlasFilterFootprint();
    testCoordinateLayouts();
    testPassResourceSelection();
    testRequestPlanEquivalence();
    testNearGroundCoverage();
    testPageLifecycle();
    testCacheStreaming();
    testSyncCache();
    constexpr float sectorSize = 4096.0F;
    const VTPageLayout pageLayout(sectorSize);
    const uint32_t root = pageLayout.rootLevel();
    require(root == 12, "VT must have meter pages even when geometry has only nine LODs");
    require(pageLayout.pageSize(0) == 1.0F, "Finest VT page must still cover one meter");
    require(config::VT_PAGE_INTERIOR / pageLayout.pageSize(0) == 512.0F, "Finest VT density must be 512 texels/m");
    require(config::VT_ATLAS_SIZE == 8448 && config::VT_PAGE_COUNT == 256, "Double atlas resolution without losing slots");
    require(config::VT_PAGE_UPDATE_BUDGET * config::VT_PAGE_RES * config::VT_PAGE_RES == 8U * 264U * 264U,
            "Doubling page resolution must preserve the per-frame composition pixel budget");
    for (uint32_t mip = 0; mip < config::VT_MIP_LEVELS; ++mip) {
        const uint32_t interior = config::VT_PAGE_INTERIOR >> mip;
        const uint32_t border = config::VT_PAGE_BORDER >> mip;
        const uint32_t slot = config::VT_PAGE_RES >> mip;
        require(interior + 2U * border == slot && border >= 1U, "VT mips must retain complete gutters");
        require(slot * config::VT_PAGES_PER_SIDE == (config::VT_ATLAS_SIZE >> mip), "Mip slots must exactly tile the atlas");
    }
    require(pageLayout.pageSize(root) == sectorSize, "Root fallback must cover the sector");
    require(pageLayout.levelForDistance(1.5F) == 0, "Near view must request maximum material precision");
    require(pageLayout.levelForDistance(8.0F) == 2, "Material precision must fall independently with distance");

    LandscapeData reference;
    reference.sectorsX = reference.sectorsZ = 2;
    reference.maxLevel = 7;
    reference.minTileLevel = 3;
    require(LandscapePagingTestAccess::cliffReferenceLevel(reference) == 4, "4 km reference must fit 256 source tiles at 2 m spacing");
    for (uint32_t sectors : {1U, 2U, 4U, 8U, 16U}) {
        reference.sectorsX = reference.sectorsZ = sectors;
        const auto level = LandscapePagingTestAccess::cliffReferenceLevel(reference);
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
    const auto oldCliffLevel = pageLayout.levelForDistance(1000 / cliffDensityScale(64, 512));
    const auto newCliffLevel = pageLayout.levelForDistance(1000 / inherited);
    require(oldCliffLevel == newCliffLevel + 2, "Distant narrow wall must retain two extra levels");
    require(cliffDensityScale(0, 512, cliffDensityScale(0, 16)) == 1, "Flat hierarchy must not request extra pages");

    // Within the optional detail budget, direct requests outrank redundant
    // ancestors. The separate coverage test above validates cache truncation.
    struct PriorityRequest { float priority; bool direct; };
    ccstd::vector<PriorityRequest> candidates;
    constexpr size_t capacity = config::VT_PAGE_COUNT - 4;
    for (size_t i = 0; i < capacity; ++i) candidates.push_back({referenceAncestorPriority(1.0F, 0), true});
    for (uint32_t ancestor = 1; ancestor <= 8; ++ancestor) {
        candidates.push_back({referenceAncestorPriority(1.0F, ancestor), false});
        require(referenceAncestorPriority(1.0F, ancestor) < referenceAncestorPriority(1.0F, ancestor - 1),
                "Coarser fallback must not inflate visible coverage priority");
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.priority > b.priority; });
    candidates.resize(capacity);
    require(std::all_of(candidates.begin(), candidates.end(), [](const auto &r) { return r.direct; }),
            "Raw refinement priority must favor direct requests over redundant ancestors");
    require(referenceAncestorPriority(4.0F, 1) > referenceAncestorPriority(0.25F, 0),
            "Important nearby fallback should still outrank tiny distant coverage");

    const auto fine = pageLayout.coveringPage(0, {{17, 29}, {18, 30}});
    require(fine.level == 0 && fine.x == 17 && fine.z == 29, "An unmorphed meter cell must use one fine page");
    const auto morph = pageLayout.coveringPage(0, {{16, 28}, {18, 30}});
    require(morph.level == 1 && morph.x == 8 && morph.z == 14, "A morphing odd cell must use its containing parent");
    const auto coarse = pageLayout.coveringPage(4, {{17, 29}, {18, 30}});
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
                            const auto page = pageLayout.coveringPage(desired,
                                {{minX, minZ}, {sectorOrigin + (x + cells) * cell, sectorOrigin + (z + cells) * cell}});
                            const float size = pageLayout.pageSize(page.level);
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
    const VTPageLayout oddLayout(1000.0F);
    require(config::VT_PAGE_INTERIOR / oddLayout.pageSize(0) <= 512.0F, "Finest page must not exceed 512 texels/m");
    const auto boundary = pageLayout.coveringPage(root, {{8191, 4095}, {8192, 4096}});
    require(boundary.x == 1 && boundary.z == 0, "Boundary must remain in its own permanent sector root");
    std::cout << "PASS: VT density, independent levels, fallback boundaries and " << checked << " morph samples\n";
}

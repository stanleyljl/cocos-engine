// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeRenderer.h"
#include "landscape/Quadtree.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTRenderer.h"
#include "landscape/VirtualTexture.h"

// Access internal CPU state without making it part of the engine's public API.
namespace cc::landscape {
struct LandscapePagingTestAccess {
    using SyncCache = LandscapeRenderer::SyncCache;
    using StreamingBudget = LandscapeRenderer::StreamingBudget;
    static uint32_t cliffReferenceLevel(const LandscapeData &data) {
        return VTRenderer::cliffReferenceLevel(data);
    }
};
} // namespace cc::landscape

#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>

using namespace cc::landscape;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

LandscapeData cacheData(uint32_t root, uint32_t sectorsX = 1, uint32_t sectorsZ = 1) {
    LandscapeData d;
    d.sectorsX = sectorsX;
    d.sectorsZ = sectorsZ;
    d.maxLevel = std::min(root, 8U);
    d.tileResolution = 129;
    d.sectorSize = static_cast<float>(1U << root);
    d.heightScale = 1;
    return d;
}

void testHeightReservations() {
    TilePagePool::Reservations slots;
    slots.data = cacheData(3);
    slots.capacity = 5;
    const NodeAddress root{3, 0, 0};
    require(slots.reserve({root}), "reserve exact height root");
    slots.slots.at(root.key()).ready = true;
    const int layer = slots.resident(root);
    require(slots.resident({0, 0, 0}) == -1, "Missing height source must not resolve to a ready root");
    ccstd::vector<NodeAddress> transition{root, {2, 0, 0}, {2, 1, 0}, {2, 0, 1}, {2, 1, 1}};
    require(slots.reserve(transition), "all-or-nothing height split reservation");
    require(slots.resident(root) == layer && slots.slots.size() == 5, "Displayed height layer moved during preparation");
    for (size_t i = 1; i < transition.size(); ++i) {
        require(slots.resident(transition[i]) == -1, "Reservation was mistaken for uploaded data");
    }
    auto excess = transition;
    excess.push_back({0, 0, 0});
    require(!slots.reserve(excess) && slots.resident(root) == layer && slots.slots.size() == 5, "Capacity refusal modified live slots");
    std::set<uint32_t> layers;
    for (const auto &entry : slots.slots) {
        layers.insert(entry.second.layer);
    }
    require(layers.size() == 5 && *layers.rbegin() < slots.capacity, "Reserved layers overlap or exceed capacity");
    auto repeated = transition;
    repeated.insert(repeated.end(), transition.begin(), transition.end());
    require(slots.reserve(repeated) && slots.working.size() == 5, "Duplicate requests inflated the budget");
    transition.erase(transition.begin());
    require(slots.reserve(transition), "release retired parent");
    for (const auto n : transition) {
        slots.slots.at(n.key()).ready = true;
    }
    require(slots.resident(root) == -1, "Retired height binding survived");
    // The released slot permits a later merge even though all four children
    // remain protected until the replacement parent is uploaded.
    transition.push_back(root);
    require(slots.reserve(transition), "merge transition cannot make progress");
    require(slots.resident(root) == -1, "Reallocated parent incorrectly inherited old ready state");
    std::cout << "PASS: exact height slots, capacity, deduplication, readiness and merge headroom\n";
}

void testExactPageLifecycle() {
    VirtualTexture pages;
    require(pages.init(cacheData(3), 5), "cache init");
    const VTPageAddress root{3, 0, 0}, fine{0, 0, 0};
    require(pages.reserve({root}), "reserve root");
    VTPageInputs inputs{};
    pages.setInputs(pages.activeSlots()[0], inputs);
    ccstd::vector<uint32_t> updates;
    pages.collectUpdates(updates, 1);
    pages.publish(updates);
    const int rootSlot = pages.resolve(root);
    require(rootSlot >= 0 && pages.resolve(fine) == -1, "Missing fine pages must NEVER resolve to a resident ancestor");
    ccstd::vector<VTPageAddress> transition{root, {2, 0, 0}, {2, 1, 0}, {2, 0, 1}, {2, 1, 1}};
    require(pages.reserve(transition), "reserve parent and four children together");
    for (const auto slot : pages.activeSlots()) {
        if (pages.page(slot).state == VirtualTexture::State::DIRTY) {
            pages.setInputs(slot, inputs);
        }
    }
    for (int frame = 0; frame < 3; ++frame) {
        pages.collectUpdates(updates, 1);
        pages.publish(updates);
        require(!pages.ready() && pages.resolve(root) == rootSlot, "Partial child completion must preserve the parent");
    }
    auto excess = transition;
    excess.push_back(fine);
    require(!pages.reserve(excess) && pages.resolve(root) == rootSlot && pages.activeSlots().size() == 5, "Failed admission must not alter the working set");
    pages.collectUpdates(updates, 1);
    pages.publish(updates);
    require(pages.ready(), "All children complete");
    transition.erase(transition.begin());
    require(pages.reserve(transition), "commit children");
    require(pages.resolve(root) == -1 && pages.resolve(fine) == -1, "Neither inactive pages nor ancestors satisfy an exact draw lookup");
    pages.invalidate();
    require(!pages.ready(), "invalidated composition must be rebuilt");
    require(pages.resolve(transition[0]) >= 0, "Previously published pixels survive a pending refresh");
    std::cout << "PASS: exact VT lookup, atomic admission, partial completion, refresh and capacity\n";
}

// Independent raster oracle: full coverage, no overlaps, and 2:1 at EVERY cell
// boundary (including all four sectors), rather than using production neighbors.
void checkCut(const RenderSelection &selection, const LandscapeData &d) {
    const uint32_t w = d.sectorsX << d.maxLevel, h = d.sectorsZ << d.maxLevel;
    std::vector<int> cells(w * h, -1);
    for (const auto key : selection.leaves()) {
        const auto n = NodeAddress::fromKey(key);
        for (uint32_t z = n.z << n.level; z < (n.z + 1) << n.level; ++z) {
            for (uint32_t x = n.x << n.level; x < (n.x + 1) << n.level; ++x) {
                require(cells[z * w + x] == -1, "Draw coverage overlaps");
                cells[z * w + x] = static_cast<int>(n.level);
                require(selection.at(x, z).key() == key, "Draw lookup disagrees with raster coverage");
            }
        }
    }
    for (uint32_t z = 0; z < h; ++z) {
        for (uint32_t x = 0; x < w; ++x) {
            const int level = cells[z * w + x];
            require(level >= 0, "Draw coverage has a hole");
            if (x) {
                require(std::abs(level - cells[z * w + x - 1]) <= 1, "Unbalanced horizontal sector edge");
            }
            if (z) {
                require(std::abs(level - cells[(z - 1) * w + x]) <= 1, "Unbalanced vertical sector edge");
            }
        }
    }
}

// Independently reconstruct nested height tiles and compare both sides of every
// stitched edge. Quantization is applied to stored samples before interpolation.
double sourceHeight(const LandscapeData &d, NodeAddress geometry, double x, double z) {
    const uint32_t level = std::max(geometry.level, d.minTileLevel), shift = level - geometry.level;
    const double size = d.nodeSize(level), originX = (geometry.x >> shift) * size, originZ = (geometry.z >> shift) * size;
    const double cells = d.tileResolution - 1;
    const double gx = std::clamp((x - originX) / size * cells, 0.0, cells), gz = std::clamp((z - originZ) / size * cells, 0.0, cells);
    const auto stored = [&](double ix, double iz) {
        const double wx = originX + ix * size / cells, wz = originZ + iz * size / cells;
        return std::round((0.5 + 0.2 * std::sin(wx * 2.7) + 0.2 * std::cos(wz * 3.1)) * 65535.0);
    };
    const double x0 = std::floor(gx), z0 = std::floor(gz), x1 = std::min(x0 + 1, cells), z1 = std::min(z0 + 1, cells);
    const double tx = gx - x0, tz = gz - z0;
    return ((1 - tx) * stored(x0, z0) + tx * stored(x1, z0)) * (1 - tz) + ((1 - tx) * stored(x0, z1) + tx * stored(x1, z1)) * tz;
}

void checkStitchedEdges(const RenderSelection &selection, const LandscapeData &d) {
    ccstd::vector<QuadNode> nodes;
    for (const auto key : selection.leaves()) {
        const auto n = NodeAddress::fromKey(key);
        nodes.push_back({n.level, n.x, n.z, 0, 1});
    }
    for (const auto &node : nodes) {
        const auto n = node.address();
        const double size = d.nodeSize(n.level), x = n.x * size, z = n.z * size, step = size / 16;
        const uint8_t mask = selection.edgeMask(n);
        for (uint32_t edge = 0; edge < 4; ++edge) {
            if ((edge == 0 && x == 0) || (edge == 1 && x + size == d.width()) || (edge == 2 && z == 0) || (edge == 3 && z + size == d.depth())) {
                continue;
            }
            for (uint32_t j = 0; j <= 16; ++j) {
                const double along = (mask & (1U << edge)) ? j - (j & 1) : j;
                const double px = edge < 2 ? x + (edge == 1 ? size : 0) : x + along * step;
                const double pz = edge < 2 ? z + along * step : z + (edge == 3 ? size : 0);
                const double probeX = edge < 2 ? px + (edge == 0 ? -1 : 1) * 1e-5 : x + std::clamp(static_cast<double>(j), 0.001, 15.999) * step;
                const double probeZ = edge < 2 ? z + std::clamp(static_cast<double>(j), 0.001, 15.999) * step : pz + (edge == 2 ? -1 : 1) * 1e-5;
                bool found = false;
                for (const auto &peer : nodes) {
                    const double peerSize = d.nodeSize(peer.level), peerX = peer.ix * peerSize, peerZ = peer.iz * peerSize;
                    if (probeX < peerX || probeX >= peerX + peerSize || probeZ < peerZ || probeZ >= peerZ + peerSize) {
                        continue;
                    }
                    found = true;
                    require(((mask & (1U << edge)) != 0) == (peer.level > n.level), "Edge mask does not describe the actual neighbor");
                    require(std::abs(sourceHeight(d, n, px, pz) - sourceHeight(d, peer.address(), px, pz)) < 1e-6, "Exact height sources disagree at a stitched vertex");
                    const double coordinate = edge < 2 ? (pz - peerZ) : (px - peerX);
                    const double lattice = coordinate / (peerSize / 16);
                    require(std::abs(lattice - std::round(lattice)) < 1e-6, "Fine edge did not collapse onto coarse lattice");
                    break;
                }
                require(found, "Shared edge has no neighbor");
            }
        }
    }
}

void testLocalTransitions() {
    auto d = cacheData(4, 2, 2);
    d.minTileLevel = 2;
    RenderSelection selection;
    selection.init(d);
    TilePagePool::Reservations heights;
    heights.data = d;
    heights.capacity = 64;
    VirtualTexture pages;
    require(pages.init(d, 64), "transaction pages init");
    RenderSelection::Change pending;
    std::mt19937 random(731);
    size_t commits = 0, refused = 0;
    for (uint32_t frame = 0; frame < 1600; ++frame) {
        const uint32_t quadrant = frame / 200;
        const uint32_t x = quadrant & 1 ? 16 : 15, z = quadrant & 2 ? 16 : 15;
        selection.target({{0, x, z, 0, 1}});
        auto resources = [&](const RenderSelection::Change &change) {
            ccstd::vector<NodeAddress> result;
            for (const auto key : selection.leaves()) {
                auto n = NodeAddress::fromKey(key);
                result.push_back(n.ancestor(std::max(n.level, d.minTileLevel)));
            }
            for (auto n : change.added) {
                result.push_back(n.ancestor(std::max(n.level, d.minTileLevel)));
            }
            return result;
        };
        auto material = [&](const RenderSelection::Change &change) {
            ccstd::vector<VTPageAddress> result;
            for (const auto key : selection.leaves()) {
                const auto n = NodeAddress::fromKey(key);
                result.push_back({n.level, n.x, n.z});
            }
            for (auto n : change.added) {
                result.push_back({n.level, n.x, n.z});
            }
            return result;
        };
        if (pending.empty()) {
            for (const auto &request : selection.requests()) {
                RenderSelection::Change change;
                if (!selection.plan(request, change)) {
                    continue;
                }
                if (!heights.canReserve(resources(change)) || !pages.canReserve(material(change)) || frame % 19 == 0) {
                    ++refused;
                    continue;
                }
                pending = std::move(change);
                break;
            }
        }
        const auto before = selection.leaves();
        require(heights.reserve(resources(pending)) && pages.reserve(material(pending)), "Admitted local resources did not fit");
        for (auto &entry : heights.slots) {
            if (random() % 3) {
                entry.second.ready = true;
            }
        }
        for (const auto slot : pages.activeSlots()) {
            if (pages.page(slot).state == VirtualTexture::State::DIRTY && random() % 3) {
                pages.setInputs(slot, {});
            }
        }
        ccstd::vector<uint32_t> updates;
        pages.collectUpdates(updates, 2);
        pages.publish(updates);
        const bool ready = std::all_of(heights.working.begin(), heights.working.end(), [&](auto n) { return heights.resident(n) >= 0; });
        if (ready && pages.ready() && !pending.empty()) {
            selection.commit(pending);
            // Verify the production commit changed precisely the requested set.
            auto expected = before;
            for (auto n : pending.removed) {
                expected.erase(n.key());
            }
            for (auto n : pending.added) {
                expected.insert(n.key());
            }
            require(selection.leaves() == expected, "Commit touched nodes outside the local transaction");
            pending = {};
            ++commits;
        } else {
            require(selection.leaves() == before, "Pending preparation changed displayed geometry");
        }
        checkCut(selection, d);
        if (frame % 16 == 0) {
            checkStitchedEdges(selection, d);
        }
        if (frame > 15) {
            for (const auto key : selection.leaves()) {
                const auto n = NodeAddress::fromKey(key);
                require(heights.resident(n.ancestor(std::max(n.level, d.minTileLevel))) >= 0 &&
                            pages.resolve({n.level, n.x, n.z}) >= 0,
                        "Committed geometry lost an exact source/page");
            }
        }
    }
    require(commits > 20 && refused > 0, "Insufficient local transition coverage");
    std::cout << "PASS: 1600 streaming frames, local deltas, cross-sector balance, stitched heights and delayed exact resources\n";
}

void testIndexedDemand() {
    const auto d = cacheData(5, 2, 2);
    RenderSelection selection;
    selection.init(d);
    // Construct a uniform coarse cut, then compare indexed requests against a
    // brute-force spatial oracle with thousands of targets (including duplicates).
    for (;;) {
        bool changed = false;
        for (auto key : selection.leaves()) {
            const auto n = NodeAddress::fromKey(key);
            if (n.level <= 2) {
                continue;
            }
            RenderSelection::Change change;
            require(selection.plan({n, true, 1}, change), "Uniform subdivision");
            selection.commit(change);
            changed = true;
            break;
        }
        if (!changed) {
            break;
        }
    }
    ccstd::vector<QuadNode> target;
    std::mt19937 random(53);
    for (uint32_t i = 0; i < 4096; ++i) {
        target.push_back({0, random() % 48U, random() % 48U, 0, 1});
    }
    selection.target(target);
    ccstd::unordered_set<uint64_t> indexed;
    for (const auto &request : selection.requests()) {
        if (request.split) {
            indexed.insert(request.node.key());
        }
    }
    for (auto key : selection.leaves()) {
        const auto n = NodeAddress::fromKey(key);
        const bool expected = std::any_of(target.begin(), target.end(), [&](const QuadNode &t) {
            return t.level < n.level && t.address().ancestor(n.level).key() == key;
        });
        require((indexed.count(key) != 0) == expected, "Indexed demand differs from spatial oracle");
    }
    // Actual visibility traverses committed leaves, independently of targets.
    ccstd::unordered_set<uint64_t> coverage;
    selection.cull([](NodeAddress) { return true; }, [&](NodeAddress n) { require(coverage.insert(n.key()).second, "Pass coverage overlaps"); });
    require(coverage == selection.leaves(), "Pass selection misses committed coverage");
    std::cout << "PASS: indexed 4096-target demand and committed pass coverage\n";
}

void testCommittedVisibility() {
    const auto d = cacheData(5, 2, 2);
    RenderSelection selection;
    selection.init(d);
    // A coarse mesh bridges a small valley: its bounds reach Y=10, whereas
    // desired fine metadata in the small view region is flat at Y=0.
    // The frustum-like box at Y=[1,4] rejects the fine target, not the coarse draw.
    const auto visible = [&](double x0, double z0, double x1, double z1) {
        ccstd::unordered_set<uint64_t> result;
        selection.cull([&](NodeAddress n) {
            const double size = d.nodeSize(n.level);
            const double lowX = n.x * size, lowZ = n.z * size;
            const double maxY = n.level == 0 ? 0.0 : 10.0;
            return lowX < x1 && lowX + size > x0 && lowZ < z1 && lowZ + size > z0 && maxY >= 1.0; }, [&](NodeAddress n) { require(result.insert(n.key()).second, "Duplicate visible leaf"); });
        return result;
    };
    selection.target({}); // The distance/target traversal selected nothing.
    const auto coarse = visible(0.25, 0.25, 0.75, 0.75);
    require(coarse.size() == 1 && coarse.count(NodeAddress{5, 0, 0}.key()),
            "An empty fine selection hid visible coarse geometry");
    selection.target({{0, 0, 0, 0, 0}});
    RenderSelection::Change pending;
    const auto requests = selection.requests();
    require(!requests.empty() && selection.plan(requests.front(), pending), "Visibility transition setup");
    require(visible(0.25, 0.25, 0.75, 0.75) == coarse,
            "Unpublished refinement changed pass visibility");
    // Commit refinement until the actual draw reaches the flat fine region.
    for (uint32_t round = 0; round < 100; ++round) {
        bool changed = false;
        for (const auto &request : selection.requests()) {
            RenderSelection::Change change;
            if (selection.plan(request, change)) {
                selection.commit(change);
                changed = true;
                break;
            }
        }
        if (!changed) {
            break;
        }
    }
    require(selection.at(0, 0).level == 0 && visible(0.25, 0.25, 0.75, 0.75).empty(),
            "Culling must follow the committed geometry's height bounds");
    // Independent shadow/camera frusta must still find a different sector even
    // when it is absent from the target list. Rejected subtrees stop at the root.
    const auto shadow = visible(32.25, 32.25, 32.75, 32.75);
    require(shadow.size() == 1 && shadow.count(NodeAddress{5, 1, 1}.key()),
            "Pass visibility leaked the main camera's target selection");
    uint32_t visited = 0, emitted = 0;
    selection.cull([&](NodeAddress) { ++visited; return false; }, [&](NodeAddress) { ++emitted; });
    require(visited == d.sectorsX * d.sectorsZ && emitted == 0,
            "Invisible sectors must be pruned without scanning their leaves");
    std::cout << "PASS: actual-cut culling, empty targets, pending refinement, independent passes and subtree pruning\n";
}

void testStitchedPageBounds() {
    const VTPageLayout layout(4096);
    // Near-camera patch at x=1, z=0: the top edge can snap x=1 to x=0 even
    // before the old distance morph threshold. Reproduce the former undersized page.
    const auto unsafe = layout.coveringPage(0, {1, 0}, {2, 1});
    require(unsafe.level == 0 && unsafe.x == 1, "Near-edge regression setup");
    const auto safe = layout.coveringPatch(0, {0, 0}, 1, 1, 0, 1);
    require(safe.level == 1 && safe.x == 0 && safe.z == 0,
            "Near-camera stitching escaped its explicitly planned VT page");
    const auto vertical = layout.coveringPatch(0, {4096, 4096}, 1, 0, 1, 1);
    require(vertical.level == 1 && vertical.x == 2048 && vertical.z == 2048,
            "Vertical/cross-sector stitch coverage differs from horizontal coverage");
    std::cout << "PASS: near-camera VT coverage reserves possible neighbor stitching\n";
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

void testCoordinateLayouts() {
    const auto invalid = NodeIndexLayout::INVALID_INDEX;
    require(NodeIndexLayout{}.nodeCount() == 0, "Default layout must be empty");
    require(NodeIndexLayout{}.globalNodeIndex(0, 0, 0) == invalid, "Empty layout must reject addresses");
    for (uint32_t root = 0; root < config::MAX_LOD_LEVELS; ++root) {
        LandscapeData data;
        data.sectorsX = 2;
        data.sectorsZ = 3;
        data.maxLevel = root;
        data.sectorSize = 1000;
        const NodeIndexLayout layout(data);
        size_t expected = 0;
        // Independently enumerate the serialized storage order. Every address
        // must map exactly once, and local/global addressing must agree.
        for (uint32_t sz = 0; sz < 3; ++sz) {
            for (uint32_t sx = 0; sx < 2; ++sx) {
                uint32_t side = 1U << root;
                for (uint32_t level = 0; level <= root; ++level, side /= 2U) {
                    require(data.nodesPerSectorSide(level) == side, "Wrong geometry level dimensions");
                    require(data.nodeSize(level) * side == 1000, "Node sizes do not cover the sector");
                    for (uint32_t z = 0; z < side; ++z) {
                        for (uint32_t x = 0; x < side; ++x) {
                            const auto address = data.nodeInSector(sx, sz, level, x, z);
                            require(layout.sectorNodeIndex(sx, sz, level, x, z) == expected, "Sector storage order changed");
                            require(layout.globalNodeIndex(level, address.x, address.z) == expected++, "Global storage order changed");
                            const auto decoded = NodeAddress::fromKey(address.key());
                            require(decoded.level == level && decoded.x == address.x && decoded.z == address.z, "Node key round trip failed");
                            const auto ancestor = address.ancestor(root);
                            require(ancestor.x == sx && ancestor.z == sz, "Ancestor crossed a sector");
                            const auto region = data.nodeRegion(address);
                            const LandscapePoint center{region.x + region.size / 2, region.z + region.size / 2};
                            require(data.nodeAtClamped(level, center).key() == address.key(), "Node/position round trip failed");
                        }
                    }
                    require(layout.sectorNodeIndex(sx, sz, level, side, 0) == invalid, "Local X overflow accepted");
                    require(layout.sectorNodeIndex(sx, sz, level, 0, side) == invalid, "Local Z overflow accepted");
                }
            }
        }
        require(layout.nodeCount() == expected, "Flat storage contains holes");
        require(layout.globalNodeIndex(root + 1, 0, 0) == invalid, "Invalid level accepted");
        require(layout.globalNodeIndex(root, 2, 0) == invalid, "Global X overflow accepted");
        require(layout.globalNodeIndex(root, 0, 3) == invalid, "Global Z overflow accepted");
        require(data.nodeSize(root + 1) == 0, "Invalid node level must have zero size");
        const LandscapePoint nearCorner{0, 0};
        const LandscapePoint farCorner{data.width(), data.depth()};
        require(farCorner.x == 2000 && farCorner.z == 3000, "Non-square landscape dimensions failed");
        require(data.contains(nearCorner) && data.contains(farCorner), "Outer edge must be sampleable");
        require(!data.contains({-0.001F, 0}) && !data.contains({2000.001F, 0}), "Outside sample accepted");
        const auto last = data.nodeAtClamped(0, farCorner);
        require(last.x == data.nodesX(0) - 1 && last.z == data.nodesZ(0) - 1, "Far edge must select the last tile");
        require(data.nodeAtClamped(0, {-1, -1}).key() == NodeAddress{0, 0, 0}.key(), "Gutter probe must clamp");
        // Adjacent float values must stay on opposite sides of a sector edge.
        for (uint32_t level = 0; level <= root; ++level) {
            const uint32_t side = data.nodesPerSectorSide(level);
            const float before = std::nextafter(data.sectorSize, 0.0F);
            const float after = std::nextafter(data.sectorSize, data.width());
            require(data.nodeAtClamped(level, {before, before}).x == side - 1U &&
                    data.nodeAtClamped(level, {before, before}).z == side - 1U,
                    "Float position before sector edge selected the next sector");
            require(data.nodeAtClamped(level, {data.sectorSize, after}).x == side &&
                    data.nodeAtClamped(level, {after, data.sectorSize}).z == side,
                    "Float position at/after sector edge selected the previous sector");
        }
        data.maxLevel = config::MAX_LOD_LEVELS;
        require(NodeIndexLayout(data).nodeCount() == 0, "Invalid LOD must produce an empty layout");
        require(layout.nodeCount() == expected, "Layout must own its dimension snapshot");
    }
    const VTPageLayout fractional(1000);
    require(fractional.rootLevel() == 9 && fractional.pageSize(0) == 1.953125F, "Non-power-of-two VT layout changed");
    require(fractional.pageSize(10) == 0, "Invalid VT level must not underflow a shift");
    for (uint32_t level = 0; level <= fractional.rootLevel(); ++level) {
        const float size = fractional.pageSize(level);
        const LandscapePoint min{2000.0F - size, 3000.0F - size};
        const auto page = fractional.coveringPage(level, min, {2000.0F, 3000.0F});
        require(page.level == level && page.x * size == min.x && page.z * size == min.z,
                "Float VT page at a distant sector edge changed coverage");
    }
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
    // huge gradients caused by grazing views or coarse material precision.
    for (uint32_t probe = 0; probe < 20000; ++probe) {
        const float u = probe % 4 == 0 ? 0.0F : probe % 4 == 1 ? 1.0F
                                                               : uv(random);
        const float v = probe % 5 == 0 ? 0.0F : probe % 5 == 1 ? 1.0F
                                                               : uv(random);
        const float magnitude = std::ldexp(1.0F, static_cast<int>(probe % 17) - 4);
        const float dxU = direction(random) * magnitude, dxV = direction(random) * magnitude;
        const float dyU = direction(random) * magnitude, dyV = direction(random) * magnitude;
        const float roomU = border + std::min(u, 1.0F - u) * interior - margin;
        const float roomV = border + std::min(v, 1.0F - v) * interior - margin;
        const float spanU = std::abs(dxU) + std::abs(dyU);
        const float spanV = std::abs(dxV) + std::abs(dyV);
        const float scale = std::min({1.0F, 2.0F * std::max(roomU, 0.0F) / std::max(spanU, 1e-6F),
                                      2.0F * std::max(roomV, 0.0F) / std::max(spanV, 1e-6F)});
        require(scale > 0.0F && scale <= 1.0F, "Invalid atlas gradient scale");
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

void testFrameStreamingBudget() {
    LandscapePagingTestAccess::StreamingBudget budget;
    VirtualTexture pages;
    require(pages.init(cacheData(3), 16), "streaming budget cache init");
    require(budget.begin(42, 100.0), "first sync must start a frame budget");
    ccstd::vector<VTPageAddress> active;
    ccstd::vector<uint32_t> updates;
    uint32_t composed = 0, committed = 0;
    // Two small transactions finish in one frame. The third stays pending
    // after consuming exactly the remaining pages, preserving earlier bindings.
    for (uint32_t count : {3U, 3U, 4U}) {
        require(budget.nextStep(100.5), "ready transactions must share one frame");
        for (uint32_t i = 0; i < count; ++i) {
            const auto index = static_cast<uint32_t>(active.size());
            active.push_back({0, index % 8, index / 8});
        }
        require(pages.reserve(active), "transaction reservation failed");
        for (const auto slot : pages.activeSlots()) {
            if (pages.page(slot).state == VirtualTexture::State::DIRTY) {
                pages.setInputs(slot, VTPageInputs{});
            }
        }
        pages.collectUpdates(updates, budget.pages);
        budget.pages -= static_cast<uint32_t>(updates.size());
        composed += static_cast<uint32_t>(updates.size());
        pages.publish(updates);
        committed += pages.ready();
    }
    require(composed == 8 && committed == 2 && !pages.ready(), "transactions exceeded the shared eight-page budget");
    require(pages.resolve(active.front()) >= 0 && pages.resolve(active.back()) == -1,
            "partial composition damaged old bindings or published unfinished pages");
    budget.attempts -= 7;
    require(!budget.begin(42, 101.0) && budget.pages == 0 && budget.attempts == config::STREAMING_ADMISSION_BUDGET - 7,
            "another camera replenished the same frame's budgets");
    pages.collectUpdates(updates, budget.pages);
    require(updates.empty(), "zero remaining budget still composed pages");
    require(budget.begin(43, 200.0) && budget.nextStep(200.5), "next frame must resume pending work");
    pages.collectUpdates(updates, budget.pages);
    budget.pages -= static_cast<uint32_t>(updates.size());
    pages.publish(updates);
    require(updates.size() == 2 && pages.ready() && budget.pages == 6, "pending transaction did not resume exactly");
    require(!budget.nextStep(202.0), "time slice expiration must stop further transactions");
    require(!budget.begin(43, 203.0) && !budget.nextStep(203.0), "same-frame sync bypassed the deadline");
    require(budget.begin(44, 300.0), "new frame must reset the step budget");
    budget.pages = 0;
    for (uint32_t i = 0; i < config::STREAMING_STEP_BUDGET; ++i) {
        require(budget.nextStep(300.5), "cached transactions must proceed without new page composition");
    }
    require(!budget.nextStep(300.5), "cached transactions bypassed the step limit");
    require(budget.begin(45, 400.0) && budget.nextStep(403.0) && !budget.nextStep(403.0),
            "an expensive upload/selection must allow one progress step, not unbounded work");
    std::cout << "PASS: shared frame budget, multiple commits, partial composition, pass reuse and time/step limits\n";
}

int main() {
    testHeightReservations();
    testExactPageLifecycle();
    testLocalTransitions();
    testIndexedDemand();
    testCommittedVisibility();
    testStitchedPageBounds();
    testSyncCache();
    testAtlasFilterFootprint();
    testCoordinateLayouts();
    testFrameStreamingBudget();
    constexpr float sectorSize = 4096.0F;
    const VTPageLayout pageLayout(sectorSize);
    const uint32_t root = pageLayout.rootLevel();
    require(root == 12, "VT must have meter pages even when geometry has only nine LODs");
    require(pageLayout.pageSize(0) == 1.0F, "Finest VT page must still cover one meter");
    require(config::VT_PAGE_INTERIOR / pageLayout.pageSize(0) == 256.0F, "Finest VT density must be 256 texels/m");
    require(config::VT_ATLAS_SIZE == 4352 && config::VT_PAGE_COUNT == 256, "Smaller pages must retain all 256 physical slots");
    require(config::VT_PAGE_UPDATE_BUDGET * config::VT_PAGE_RES * config::VT_PAGE_RES == 8U * 272U * 272U,
            "Eight 256-texel pages must retain eight-texel filtering gutters");
    for (uint32_t mip = 0; mip < config::VT_MIP_LEVELS; ++mip) {
        const uint32_t interior = config::VT_PAGE_INTERIOR >> mip;
        const uint32_t border = config::VT_PAGE_BORDER >> mip;
        const uint32_t slot = config::VT_PAGE_RES >> mip;
        require(interior + 2U * border == slot && border >= 1U, "VT mips must retain complete gutters");
        require(slot * config::VT_PAGES_PER_SIDE == (config::VT_ATLAS_SIZE >> mip), "Mip slots must exactly tile the atlas");
    }
    require(pageLayout.pageSize(root) == sectorSize, "Root page must cover the sector");
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
    const auto fine = pageLayout.coveringPage(0, {17, 29}, {18, 30});
    require(fine.level == 0 && fine.x == 17 && fine.z == 29, "An unmorphed meter cell must use one fine page");
    const auto morph = pageLayout.coveringPage(0, {16, 28}, {18, 30});
    require(morph.level == 1 && morph.x == 8 && morph.z == 14, "A morphing odd cell must use its containing parent");
    const auto coarse = pageLayout.coveringPage(4, {17, 29}, {18, 30});
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
                            const auto page = pageLayout.coveringPatch(desired, {sectorOrigin, sectorOrigin}, cell, x, z, cells);
                            const float size = pageLayout.pageSize(page.level);
                            for (uint32_t vz = z; vz <= z + cells; ++vz) {
                                for (uint32_t vx = x; vx <= x + cells; ++vx) {
                                    for (float amount : {0.0F, 1.0F}) {
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
    require(config::VT_PAGE_INTERIOR / oddLayout.pageSize(0) <= 256.0F, "Finest page must not exceed 256 texels/m");
    const auto boundary = pageLayout.coveringPage(root, {8191, 4095}, {8192, 4096});
    require(boundary.x == 1 && boundary.z == 0, "Boundary must remain in its own permanent sector root");
    std::cout << "PASS: VT density, independent levels, page boundaries and " << checked << " stitch samples\n";
}

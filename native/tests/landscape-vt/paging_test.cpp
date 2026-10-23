// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeAsset.h"
#include "landscape/Quadtree.h"
#include "landscape/SourceDataManager.h"
#include "landscape/VTPageProcessing.h"
#include "landscape/VTRenderer.h"
#include "base/Log.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderTexture.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/assets/Texture2D.h"
#include "renderer/gfx-base/GFXCommandBuffer.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXInputAssembler.h"
#include "renderer/gfx-base/GFXPipelineState.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDescriptorSetLayout.h"
#include "renderer/gfx-base/GFXPipelineLayout.h"
#include "renderer/gfx-base/GFXShader.h"
#include "renderer/gfx-base/GFXRenderPass.h"
#include "core/geometry/AABB.h"
#include "renderer/gfx-base/GFXTexture.h"

#include <cstdlib>

// This executable exercises the real SourceDataManager reservation methods without
// a device or asset loader. Tree tests use explicit targets rather than camera
// selection. Fail immediately if a test enters any of these unprovided paths.
namespace cc {
Root *Root::getInstance() {
    std::abort();
}
LogLevel Log::slogLevel = LogLevel::INFO;
void Log::logMessage(LogType, LogLevel, const char *, ...) {
    std::abort();
}
void gfx::Texture::initialize(const gfx::TextureInfo &) {
    std::abort();
}
void gfx::CommandBuffer::initialize(const gfx::CommandBufferInfo &) {
    std::abort();
}
bool geometry::AABB::aabbFrustum(const geometry::Frustum &) const {
    std::abort();
}
bool landscape::LandscapeAsset::loadTileSet(uint32_t, uint32_t, uint32_t, gfx::Format, TileData &) const {
    std::abort();
}
bool landscape::LandscapeAsset::requestTile(uint32_t, uint32_t, uint32_t, gfx::Format) {
    std::abort();
}
bool landscape::LandscapeAsset::takeReadyTile(TileData &) {
    std::abort();
}
} // namespace cc

// The CPU suite exercises VTRenderer's real residency algorithms without a GPU.
// Device/processing entry points abort if reached; app regressions cover rendering.
namespace cc::landscape {
VTPageComposer::VTPageComposer() {
    std::abort();
}
bool VTPageComposer::init(const LandscapeAsset &, SourceDataManager &, const MaterialLibrary &, DecalRenderer &, bool) {
    std::abort();
}
bool VTPageComposer::prepareInputs(uint32_t, LandscapeRegion, const VTPageSources &, const SourceDataManager &, VTPageInstance &) {
    std::abort();
}
void VTPageComposer::uploadInputs() {
    std::abort();
}
void VTPageComposer::record(gfx::CommandBuffer &, const VTPageInstance *, uint32_t, bool) {
    std::abort();
}
void VTPageComposer::setGlobalColorMap(Texture2D *) {
    std::abort();
}
bool VTPageComposer::setGlobalColorStrength(float) {
    std::abort();
}
bool VTPageComposer::setHeightBlendEnabled(bool) {
    std::abort();
}
VTPageMipGenerator::VTPageMipGenerator() {
    std::abort();
}
bool VTPageMipGenerator::init(const VTPageComposer &, bool) {
    std::abort();
}
void VTPageMipGenerator::record(gfx::CommandBuffer &, const VTPageComposer &, uint32_t, bool) {
    std::abort();
}
VTPageCompressor::VTPageCompressor() {
    std::abort();
}
bool VTPageCompressor::supported() {
    std::abort();
}
bool VTPageCompressor::init(gfx::Device *, RenderTexture &) {
    std::abort();
}
Texture2D *VTPageCompressor::atlas() const {
    std::abort();
}
gfx::Texture *VTPageCompressor::albedo() const {
    std::abort();
}
gfx::Texture *VTPageCompressor::normalRoughnessAO() const {
    std::abort();
}
void VTPageCompressor::begin(gfx::CommandBuffer &, bool) {
    std::abort();
}
void VTPageCompressor::record(gfx::CommandBuffer &, uint32_t) {
    std::abort();
}
void VTPageCompressor::end(gfx::CommandBuffer &) {
    std::abort();
}
VTPageComposer::~VTPageComposer() = default;
VTPageMipGenerator::~VTPageMipGenerator() = default;
VTPageCompressor::~VTPageCompressor() = default;
} // namespace cc::landscape

#include <iostream>
#include <map>
#include <random>
#include <set>

using namespace cc::landscape;

namespace {
// Stack-owned CPU fixtures expose protected state operations only to this suite.
// Production objects retain their complete GPU initialization and submission paths.
class SourceDataFixture final : public SourceDataManager {
public:
    using SourceDataManager::initSlots;
    using SourceDataManager::reservedSources;
    using SourceDataManager::reservedLayer;
    using SourceDataManager::publish;
};

class VTResidencyFixture final : public VTRenderer {
public:
    using VTRenderer::initPages;
    using VTRenderer::publish;
};

class VTPageTableFixture final : public VTPageTable {
public:
    using VTPageTable::resizeEntries;
};
} // namespace

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// Compare topology independently of the material records now owned by CurrentTree.
ccstd::unordered_set<uint64_t> leafKeys(const CurrentTree &tree) {
    ccstd::unordered_set<uint64_t> result;
    for (const auto &entry : tree.nodes()) {
        result.insert(entry.first);
    }
    return result;
}

ccstd::vector<NodeAddress> addedNodes(const CurrentTree::Change &change) {
    return change.added;
}

// Topology-only tests supply explicit synthetic bounds; production supplies
// asset bounds and VT mappings through LandscapeSchedule's admitted transaction.
void commitTopology(CurrentTree &tree, const CurrentTree::Change &change, const LandscapeData &data) {
    ccstd::unordered_map<uint64_t, CurrentTree::EdgeMasks> oldEdges;
    for (const auto &entry : tree.nodes()) {
        oldEdges.emplace(entry.first, tree.edgeMasks(entry.second.geometry.address()));
    }
    tree.clearChanges();
    ccstd::vector<CurrentTree::Node> prepared;
    for (const auto node : change.added) {
        prepared.push_back({{node.level, node.x, node.z, data.minHeight(), data.maxHeight()}, {}});
    }
    tree.commit(change, prepared);
    for (const auto node : change.removed) {
        require(tree.changedNodes().count(node.key()) != 0, "Renderer was not notified of a removed node");
    }
    for (const auto &entry : tree.nodes()) {
        const auto old = oldEdges.find(entry.first);
        const auto edges = tree.edgeMasks(entry.second.geometry.address());
        if (old == oldEdges.end() || old->second.coarser != edges.coarser || old->second.finer != edges.finer) {
            require(tree.changedNodes().count(entry.first) != 0, "Renderer would retain stale morph edge constraints");
        }
    }
}

ccstd::vector<CurrentTree::Candidate> topologyCandidates(const CurrentTree &tree, const ExpectedTree &expected) {
    auto result = tree.candidates(expected);
    result.erase(std::remove_if(result.begin(), result.end(), [](const auto &candidate) {
        return candidate.operation == CurrentTree::Operation::REFRESH;
    }), result.end());
    return result;
}

ExpectedTree demand(uint32_t maxLevel, const ccstd::vector<QuadNode> &geometry, const ccstd::vector<QuadNode> &surface = {}) {
    ExpectedTree expected(maxLevel);
    expected.include(geometry, false);
    expected.include(surface, true);
    return expected;
}

// Model completion of the GPU work represented by this CPU-only fixture.
void publishPages(VTResidencyFixture &renderer, const ccstd::vector<uint32_t> &slots) {
    for (const auto slot : slots) {
        renderer.publish(slot);
    }
}

void collectUpdates(const VTRenderer &pages, ccstd::vector<uint32_t> &updates, uint32_t budget) {
    updates.clear();
    for (const auto slot : pages.reservedSlots()) {
        if (updates.size() == budget) {
            break;
        }
        if (pages.page(slot).state != VTRenderer::PageState::READY) {
            updates.push_back(slot);
        }
    }
}

LandscapeData cacheData(uint32_t root, uint32_t sectorsX = 1, uint32_t sectorsZ = 1) {
    LandscapeData d;
    d.sectorsX = sectorsX;
    d.sectorsZ = sectorsZ;
    d.maxLevel = std::min(root, 8U);
    d.sectorSize = static_cast<float>(1U << root);
    d.heightScale = 1;
    return d;
}

void testSectorCountLimit() {
    for (const auto dimensions : {std::pair<uint32_t, uint32_t>{8, 8}, {64, 1}, {1, 64}}) {
        const auto d = cacheData(4, dimensions.first, dimensions.second);
        require(d.valid() && NodeIndexLayout(d).nodeCount() > 0, "64 sectors must be accepted on either axis");
        SourceDataFixture sources;
        require(sources.initSlots(d, config::PAGE_POOL_LAYERS), "Initialize source reservations");
        VTResidencyFixture pages;
        require(pages.initPages(d), "maximum sector count must initialize VT");
        ccstd::vector<NodeAddress> roots;
        ccstd::vector<VTPageAddress> rootPages;
        const VTPageLayout layout(d.sectorSize);
        for (uint32_t z = 0; z < d.sectorsZ; ++z) {
            for (uint32_t x = 0; x < d.sectorsX; ++x) {
                roots.push_back({d.maxLevel, x, z});
                rootPages.push_back({layout.rootLevel(), x, z});
            }
        }
        require(sources.reserve(roots) && pages.reserve(rootPages), "maximum root coverage must fit both caches");
    }
    for (const auto dimensions : {std::pair<uint32_t, uint32_t>{65, 1}, {1, 65}, {9, 8}, {65536, 65536}}) {
        const auto d = cacheData(4, dimensions.first, dimensions.second);
        require(!d.valid() && NodeIndexLayout(d).nodeCount() == 0, "excess sector count must be rejected without overflow");
    }
}

void testHeightReservations() {
    SourceDataFixture slots;
    require(slots.initSlots(cacheData(3), 5), "Initialize source reservations");
    const NodeAddress root{3, 0, 0};
    require(slots.reserve({root}), "reserve exact height root");
    slots.publish(root.key());
    const int layer = slots.resident(root);
    require(slots.resident({0, 0, 0}) == -1, "Missing height source must not resolve to a ready root");
    ccstd::vector<NodeAddress> transition{root, {2, 0, 0}, {2, 1, 0}, {2, 0, 1}, {2, 1, 1}};
    require(slots.reserve(transition), "all-or-nothing height split reservation");
    require(slots.resident(root) == layer && slots.reservedSources().size() == 5, "Displayed height layer moved during preparation");
    for (size_t i = 1; i < transition.size(); ++i) {
        require(slots.resident(transition[i]) == -1, "Reservation was mistaken for uploaded data");
    }
    auto excess = transition;
    excess.push_back({0, 0, 0});
    require(!slots.reserve(excess) && slots.resident(root) == layer && slots.reservedSources().size() == 5, "Capacity refusal modified live slots");
    std::set<uint32_t> layers;
    for (const auto source : slots.reservedSources()) {
        layers.insert(static_cast<uint32_t>(slots.reservedLayer(source)));
    }
    require(layers.size() == 5 && *layers.rbegin() < slots.layerCount(), "Reserved layers overlap or exceed capacity");
    require(slots.reserve(transition) && slots.resident(root) == layer && slots.reservedSources().size() == 5,
            "Reserving the same unique working set changed its bindings");
    transition.erase(transition.begin());
    require(slots.reserve(transition), "release retired parent");
    for (const auto n : transition) {
        slots.publish(n.key());
    }
    require(slots.resident(root) == -1, "Retired height binding survived");
    // The released slot permits a later merge even though all four children
    // remain protected until the replacement parent is uploaded.
    transition.push_back(root);
    require(slots.reserve(transition), "merge transition cannot make progress");
    require(slots.resident(root) == -1, "Reallocated parent incorrectly inherited old ready state");
    std::cout << "PASS: exact height slots, capacity, stable reservations, readiness and merge headroom\n";
}

void testExactPageLifecycle() {
    VTResidencyFixture pages;
    require(pages.initPages(cacheData(3), 5), "cache init");
    const VTPageAddress root{3, 0, 0}, fine{0, 0, 0};
    require(pages.reserve({root}), "reserve root");
    ccstd::vector<uint32_t> updates;
    collectUpdates(pages, updates, 1);
    publishPages(pages, updates);
    const int rootSlot = pages.resolve(root);
    require(rootSlot >= 0 && pages.resolve(fine) == -1, "Missing fine pages must NEVER resolve to a resident ancestor");
    ccstd::vector<VTPageAddress> transition{root, {2, 0, 0}, {2, 1, 0}, {2, 0, 1}, {2, 1, 1}};
    require(pages.reserve(transition), "reserve parent and four children together");
    for (int frame = 0; frame < 3; ++frame) {
        collectUpdates(pages, updates, 1);
        publishPages(pages, updates);
        require(!pages.ready() && pages.resolve(root) == rootSlot, "Partial child completion must preserve the parent");
    }
    auto excess = transition;
    excess.push_back(fine);
    require(!pages.reserve(excess) && pages.resolve(root) == rootSlot && pages.reservedSlots().size() == 5, "Failed admission must not alter the working set");
    collectUpdates(pages, updates, 1);
    publishPages(pages, updates);
    require(pages.ready(), "All children complete");
    transition.erase(transition.begin());
    require(pages.reserve(transition), "commit children");
    require(pages.resolve(root) == -1 && pages.resolve(fine) == -1, "Neither inactive pages nor ancestors satisfy an exact draw lookup");
    pages.invalidate();
    require(!pages.ready(), "invalidated composition must be rebuilt");
    require(pages.resolve(transition[0]) >= 0, "Previously published pixels survive a pending refresh");
    std::cout << "PASS: exact VT lookup, atomic admission, partial completion, refresh and capacity\n";
}

void testPageRefreshAndReuse() {
    VTResidencyFixture pages;
    require(pages.initPages(cacheData(3), 2), "refresh cache init");
    const VTPageAddress a{0, 0, 0};
    const VTPageAddress b{1, 1, 2};
    const VTPageAddress c{0, 3, 4};
    require(pages.reserve({a, b}), "reserve incomplete and complete pages together");
    const auto aSlot = pages.reservedSlots()[0];
    const auto bSlot = pages.reservedSlots()[1];
    publishPages(pages, {bSlot});
    pages.invalidate();
    pages.invalidate();
    require(pages.page(aSlot).state == VTRenderer::PageState::UNCOMPOSED && pages.resolve(a) == -1,
            "invalidating an unfinished page must not make it drawable");
    require(pages.page(bSlot).state == VTRenderer::PageState::OUTDATED && pages.resolve(b) == static_cast<int>(bSlot),
            "repeated invalidation must retain the previous complete pixels");
    const auto region = pages.region(b);
    require(region.x == 2 && region.z == 4 && region.size == 2,
            "atlas mapping must derive from the page address, not temporary source inputs");
    require(!pages.reserve({a, b, c}) && pages.reservedSlots().size() == 2 && pages.needsComposition(a) && pages.needsComposition(b),
            "capacity refusal changed a pending refresh");
    require(!pages.reserve({{4, 0, 0}}) && !pages.reserve({{0, 8, 0}}) && pages.resolve(b) == static_cast<int>(bSlot),
            "invalid addresses changed live page bindings");
    ccstd::vector<uint32_t> updates;
    collectUpdates(pages, updates, 0);
    publishPages(pages, updates);
    require(updates.empty() && pages.needsComposition(a) && pages.needsComposition(b), "zero budget changed page state");
    collectUpdates(pages, updates, 2);
    require(updates.size() == 2, "both unfinished and stale pages require composition");
    // The renderer can skip a missing source and publish a later ready page.
    publishPages(pages, {bSlot});
    require(!pages.ready() && pages.resolve(a) == -1 && !pages.needsComposition(b),
            "partial composition published an unresolved page");
    require(pages.reserve({c, b}) && pages.reservedSlots()[0] == aSlot, "retired slot was not reused");
    require(pages.resolve(c) == -1 && pages.page(aSlot).state == VTRenderer::PageState::UNCOMPOSED,
            "replacement inherited retired page pixels");
    publishPages(pages, {aSlot});
    require(pages.ready(), "replacement page did not complete");
    require(pages.reserve({b}) && pages.resolve(c) == -1 && !pages.needsComposition(c),
            "inactive cached pages must stay reusable but cannot satisfy draw bindings");
    pages.invalidate();
    require(pages.reserve({b, c}) && pages.resolve(c) == static_cast<int>(aSlot) && pages.needsComposition(c),
            "reactivated stale pages lost their previous pixels or skipped refresh");
    collectUpdates(pages, updates, 2);
    publishPages(pages, updates);
    require(pages.ready(), "reactivated pages did not finish refresh");
    std::cout << "PASS: pending/stale pages, skipped sources, invalid admission, zero budget and slot reuse\n";
}

void testRegionalInvalidation() {
    VTResidencyFixture pages;
    require(pages.initPages(cacheData(3), 4), "regional cache init");
    const VTPageAddress changed{0, 0, 0};
    const VTPageAddress gutter{0, 1, 0};
    const VTPageAddress distant{0, 6, 6};
    const VTPageAddress unfinished{0, 0, 1};
    require(pages.reserve({changed, gutter, distant, unfinished}), "reserve regional pages");
    const auto changedSlot = pages.reservedSlots()[0];
    const auto gutterSlot = pages.reservedSlots()[1];
    const auto distantSlot = pages.reservedSlots()[2];
    const auto unfinishedSlot = pages.reservedSlots()[3];
    publishPages(pages, {changedSlot, gutterSlot, distantSlot});
    require(pages.reserve({changed, distant, unfinished}), "cache neighboring page outside working set");
    // The decal ends before X=1, but is still inside the neighboring page's gutter.
    pages.invalidate({0.96F, 0.25F, 0.02F});
    require(pages.page(changedSlot).state == VTRenderer::PageState::OUTDATED &&
                pages.page(gutterSlot).state == VTRenderer::PageState::OUTDATED,
            "regional edits must invalidate intersecting and cached gutter pages");
    require(pages.page(distantSlot).state == VTRenderer::PageState::READY &&
                pages.page(unfinishedSlot).state == VTRenderer::PageState::UNCOMPOSED,
            "regional edits must preserve unrelated and unpublished pages");
    require(pages.needsComposition(changed) && pages.resolve(changed) == static_cast<int>(changedSlot),
            "regional refresh must remain pending while retaining displayed pixels");
    pages.invalidate({0.96F, 0.25F, 0.02F});
    require(pages.page(changedSlot).state == VTRenderer::PageState::OUTDATED &&
                pages.page(gutterSlot).state == VTRenderer::PageState::OUTDATED &&
                pages.page(distantSlot).state == VTRenderer::PageState::READY,
            "repeated edits must retain stale pixels and preserve unrelated pages");
    require(pages.reserve({changed, gutter, distant, unfinished}), "restore cached gutter page");
    require(pages.needsComposition(gutter), "reactivated gutter page must not reuse outdated decal pixels");
    publishPages(pages, {changedSlot, gutterSlot, unfinishedSlot});
    require(pages.ready(), "regional refresh completes");
    pages.invalidate({0.96F, 0.25F, 0.02F});
    require(pages.needsComposition(changed) && pages.needsComposition(gutter) && !pages.needsComposition(distant),
            "decal removal must refresh the same footprint without rebuilding unrelated pages");
}

// Independent raster oracle: full coverage, no overlaps, and 2:1 at EVERY cell
// boundary (including all four sectors), rather than using production neighbors.
void checkCut(const CurrentTree &selection, const LandscapeData &d) {
    const uint32_t w = d.sectorsX << d.maxLevel, h = d.sectorsZ << d.maxLevel;
    std::vector<int> cells(w * h, -1);
    for (const auto key : leafKeys(selection)) {
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
    const double cells = config::SOURCE_TILE_RESOLUTION - 1;
    const double gx = std::clamp((x - originX) / size * cells, 0.0, cells), gz = std::clamp((z - originZ) / size * cells, 0.0, cells);
    const auto stored = [&](double ix, double iz) {
        const double wx = originX + ix * size / cells, wz = originZ + iz * size / cells;
        return std::round((0.5 + 0.2 * std::sin(wx * 2.7) + 0.2 * std::cos(wz * 3.1)) * 65535.0);
    };
    const double x0 = std::floor(gx), z0 = std::floor(gz), x1 = std::min(x0 + 1, cells), z1 = std::min(z0 + 1, cells);
    const double tx = gx - x0, tz = gz - z0;
    return ((1 - tx) * stored(x0, z0) + tx * stored(x1, z0)) * (1 - tz) + ((1 - tx) * stored(x0, z1) + tx * stored(x1, z1)) * tz;
}

void checkStitchedEdges(const CurrentTree &selection, const LandscapeData &d) {
    ccstd::vector<QuadNode> nodes;
    for (const auto key : leafKeys(selection)) {
        const auto n = NodeAddress::fromKey(key);
        nodes.push_back({n.level, n.x, n.z, 0, 1});
    }
    for (const auto &node : nodes) {
        const auto n = node.address();
        const double size = d.nodeSize(n.level), x = n.x * size, z = n.z * size, step = size / 16;
        const auto masks = selection.edgeMasks(n);
        for (uint32_t edge = 0; edge < 4; ++edge) {
            if ((edge == 0 && x == 0) || (edge == 1 && x + size == d.width()) || (edge == 2 && z == 0) || (edge == 3 && z + size == d.depth())) {
                continue;
            }
            for (uint32_t j = 0; j <= 16; ++j) {
                const double along = (masks.coarser & (1U << edge)) ? j - (j & 1) : j;
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
                    require(((masks.coarser & (1U << edge)) != 0) == (peer.level > n.level), "Coarser edge mask does not describe the actual neighbor");
                    require(((masks.finer & (1U << edge)) != 0) == (peer.level < n.level), "Finer edge mask does not describe the actual neighbor");
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

// The same balanced transactions must converge under a synchronous warmup or
// delayed streaming, even when either cache cannot hold the requested detail.
void testCapacityLimitedTransitions() {
    const auto d = cacheData(4, 2, 2);
    // Four resident roots plus four replacement children need eight slots
    // during an atomic split, even though the resulting cut has seven leaves.
    for (const auto capacities : {std::pair<uint32_t, uint32_t>{4, 256}, {256, 4}, {4, 4},
                                 {7, 256}, {256, 7}, {7, 7}, {8, 8}}) {
        CurrentTree selection;
        selection.init(d);
        ExpectedTree expected(d.maxLevel);
        SourceDataFixture heights;
        require(heights.initSlots(d, capacities.first), "Initialize source reservations");
        VTResidencyFixture pages;
        require(pages.initPages(d, capacities.second), "root-only VT capacity must initialize");
        ccstd::vector<NodeAddress> roots;
        ccstd::vector<VTPageAddress> materials;
        for (const auto key : leafKeys(selection)) {
            const auto n = NodeAddress::fromKey(key);
            roots.push_back(n); materials.push_back({n.level, n.x, n.z});
        }
        require(heights.reserve(roots) && pages.reserve(materials), "exact root capacity must fit");
        expected = demand(d.maxLevel, {QuadNode{0, 15, 15, 0, 1}}, {});
        bool planned = false;
        for (const auto &request : topologyCandidates(selection, expected)) {
            CurrentTree::Change change;
            if (!selection.plan(request, change)) continue;
            planned = true;
            auto sources = roots;
            auto pending = materials;
            for (const auto n : addedNodes(change)) {
                sources.push_back(n); pending.push_back({n.level, n.x, n.z});
            }
            const bool fits = heights.canReserve(sources) && pages.canReserve(pending);
            require(fits == (capacities.first == 8 && capacities.second == 8), "atomic split capacity boundary");
            checkCut(selection, d); checkStitchedEdges(selection, d);
        }
        require(planned && leafKeys(selection).size() == 4, "capacity probe must retain complete roots");
    }
    for (const auto capacities : {std::pair<uint32_t, uint32_t>{12, 256}, {256, 12}, {12, 12},
                                 {19, 256}, {256, 19}, {19, 19}, {20, 256}, {256, 20}, {20, 20}}) {
        ccstd::unordered_set<uint64_t> warmupCut;
        for (const bool delayed : {false, true}) {
            CurrentTree selection;
            selection.init(d);
            ExpectedTree expected(d.maxLevel);
            SourceDataFixture heights;
            require(heights.initSlots(d, capacities.first), "Initialize source reservations");
            VTResidencyFixture pages;
            require(pages.initPages(d, capacities.second), "capacity test VT init");
            const auto reserve = [&](const CurrentTree::Change &change) {
                ccstd::vector<NodeAddress> sources;
                ccstd::vector<VTPageAddress> materials;
                const auto add = [&](NodeAddress n) {
                    sources.push_back(n);
                    materials.push_back({n.level, n.x, n.z});
                };
                for (const auto key : leafKeys(selection)) add(NodeAddress::fromKey(key));
                for (const auto node : addedNodes(change)) add(node);
                if (!heights.canReserve(sources) || !pages.canReserve(materials)) return false;
                return heights.reserve(sources) && pages.reserve(materials);
            };
            const auto complete = [&](uint32_t budget) {
                for (const auto source : heights.reservedSources()) {
                    heights.publish(source.key());
                }
                ccstd::vector<uint32_t> updates;
                collectUpdates(pages, updates, budget);
                publishPages(pages, updates);
            };
            require(reserve({}), "minimum root coverage must fit");
            complete(capacities.second);
            size_t refused = 0, commits = 0;
            for (const auto target : {QuadNode{0, 15, 15, 0, 1}, QuadNode{0, 24, 24, 0, 1},
                                     QuadNode{0, 0, 0, 0, 1}, QuadNode{0, 31, 0, 0, 1},
                                     QuadNode{0, 0, 31, 0, 1}, QuadNode{0, 31, 31, 0, 1},
                                     QuadNode{0, 16, 16, 0, 1}, QuadNode{0, 15, 15, 0, 1}}) {
                expected = demand(d.maxLevel, {target}, {});
                bool settled = false;
                for (uint32_t step = 0; step < 256; ++step) {
                    bool changed = false;
                    for (const auto &request : topologyCandidates(selection, expected)) {
                        CurrentTree::Change change;
                        if (!selection.plan(request, change)) continue;
                        const auto before = leafKeys(selection);
                        if (!reserve(change)) {
                            ++refused;
                            require(leafKeys(selection) == before, "capacity refusal partially split neighbors");
                            checkCut(selection, d);
                            checkStitchedEdges(selection, d);
                            continue;
                        }
                        do {
                            complete(delayed ? 1U : capacities.second);
                            require(leafKeys(selection) == before, "incomplete pages changed committed coverage");
                        } while (!pages.ready());
                        commitTopology(selection, change, d);
                        require(reserve({}), "committed cut exceeds capacity");
                        checkCut(selection, d);
                        checkStitchedEdges(selection, d);
                        ++commits;
                        changed = true;
                        break;
                    }
                    if (!changed) {
                        settled = true;
                        break;
                    }
                }
                require(settled && pages.ready(), "capacity-limited cut must settle with complete material coverage");
                // Corner targets can reach full detail with fewer balancing
                // neighbors. Capacity refusal is checked across the route below.
                if (target.ix == 15 && target.iz == 15) {
                    require(selection.at(target.ix, target.iz).level > target.level, "central target must exceed cache capacity");
                }
                for (const auto key : leafKeys(selection)) {
                    const auto n = NodeAddress::fromKey(key);
                    require(heights.resident(n) >= 0 && pages.resolve({n.level, n.x, n.z}) >= 0,
                            "capacity-limited coverage lost a published resource");
                }
            }
            require(refused > 0 && commits > 1, "capacity test must exercise refusal and recovery");
            if (!delayed) warmupCut = leafKeys(selection);
            else require(leafKeys(selection) == warmupCut, "warmup and streaming produced different balanced cuts");
        }
    }
    std::cout << "PASS: source/VT/both capacity limits, warmup/streaming parity, balanced coarse coverage and movement\n";
}

void testLocalTransitions() {
    auto d = cacheData(4, 2, 2);
    d.minTileLevel = 2;
    CurrentTree selection;
    selection.init(d);
    ExpectedTree expected(d.maxLevel);
    SourceDataFixture heights;
    require(heights.initSlots(d, 64), "Initialize source reservations");
    VTResidencyFixture pages;
    require(pages.initPages(d, 64), "transaction pages init");
    CurrentTree::Change pending;
    std::mt19937 random(731);
    size_t commits = 0, refused = 0;
    for (uint32_t frame = 0; frame < 1600; ++frame) {
        const uint32_t quadrant = frame / 200;
        const uint32_t x = quadrant & 1 ? 16 : 15, z = quadrant & 2 ? 16 : 15;
        expected = demand(d.maxLevel, {{0, x, z, 0, 1}}, {});
        auto resources = [&](const CurrentTree::Change &change) {
            ccstd::vector<NodeAddress> result;
            ccstd::unordered_set<uint64_t> unique;
            const auto add = [&](NodeAddress node) {
                const auto source = node.ancestor(std::max(node.level, d.minTileLevel));
                if (unique.insert(source.key()).second) {
                    result.push_back(source);
                }
            };
            for (const auto key : leafKeys(selection)) {
                add(NodeAddress::fromKey(key));
            }
            for (const auto node : addedNodes(change)) {
                add(node);
            }
            return result;
        };
        auto material = [&](const CurrentTree::Change &change) {
            ccstd::vector<VTPageAddress> result;
            for (const auto key : leafKeys(selection)) {
                const auto n = NodeAddress::fromKey(key);
                result.push_back({n.level, n.x, n.z});
            }
            for (auto n : addedNodes(change)) {
                result.push_back({n.level, n.x, n.z});
            }
            return result;
        };
        if (pending.empty()) {
            for (const auto &request : topologyCandidates(selection, expected)) {
                CurrentTree::Change change;
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
        const auto before = leafKeys(selection);
        require(heights.reserve(resources(pending)) && pages.reserve(material(pending)), "Admitted local resources did not fit");
        for (const auto source : heights.reservedSources()) {
            if (random() % 3) {
                heights.publish(source.key());
            }
        }
        ccstd::vector<uint32_t> candidates;
        collectUpdates(pages, candidates, static_cast<uint32_t>(pages.capacity()));
        ccstd::vector<uint32_t> updates;
        for (const auto slot : candidates) {
            // Model missing source data outside the page cache, as the renderer does.
            if (random() % 3 && updates.size() < 2) {
                updates.push_back(slot);
            }
        }
        publishPages(pages, updates);
        const bool ready = std::all_of(heights.reservedSources().begin(), heights.reservedSources().end(), [&](auto n) { return heights.resident(n) >= 0; });
        if (ready && pages.ready() && !pending.empty()) {
            commitTopology(selection, pending, d);
            // Verify the production commit changed precisely the requested set.
            auto expected = before;
            for (auto n : pending.removed) {
                expected.erase(n.key());
            }
            for (auto n : addedNodes(pending)) {
                expected.insert(n.key());
            }
            require(leafKeys(selection) == expected, "Commit touched nodes outside the local transaction");
            pending = {};
            ++commits;
        } else {
            require(leafKeys(selection) == before, "Pending preparation changed displayed geometry");
        }
        checkCut(selection, d);
        if (frame % 16 == 0) {
            checkStitchedEdges(selection, d);
        }
        if (frame > 15) {
            for (const auto key : leafKeys(selection)) {
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
    CurrentTree selection;
    selection.init(d);
    ExpectedTree expected(d.maxLevel);
    // Construct a uniform coarse cut, then compare indexed requests against a
    // brute-force spatial oracle with thousands of targets (including duplicates).
    for (;;) {
        bool changed = false;
        for (auto key : leafKeys(selection)) {
            const auto n = NodeAddress::fromKey(key);
            if (n.level <= 2) {
                continue;
            }
            CurrentTree::Change change;
            require(selection.plan({n, CurrentTree::Operation::SPLIT}, change), "Uniform subdivision");
            commitTopology(selection, change, d);
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
    expected = demand(d.maxLevel, target, {});
    ccstd::unordered_set<uint64_t> indexed;
    for (const auto &request : topologyCandidates(selection, expected)) {
        if (request.operation == CurrentTree::Operation::SPLIT) {
            indexed.insert(request.node.key());
        }
    }
    for (auto key : leafKeys(selection)) {
        const auto n = NodeAddress::fromKey(key);
        const bool expected = std::any_of(target.begin(), target.end(), [&](const QuadNode &t) {
            return t.level < n.level && t.address().ancestor(n.level).key() == key;
        });
        require((indexed.count(key) != 0) == expected, "Indexed demand differs from spatial oracle");
    }
    // Actual visibility traverses committed leaves, independently of targets.
    ccstd::unordered_set<uint64_t> coverage;
    selection.cull([&](NodeAddress n, bool leaf) {
        if (leaf) {
            require(coverage.insert(n.key()).second, "Pass coverage overlaps");
        }
        return true;
    });
    require(coverage == leafKeys(selection), "Pass selection misses committed coverage");
    std::cout << "PASS: indexed 4096-target demand and committed pass coverage\n";
}

void testCommittedVisibility() {
    const auto d = cacheData(5, 2, 2);
    CurrentTree selection;
    selection.init(d);
    ExpectedTree expected(d.maxLevel);
    // A coarse mesh bridges a small valley: its bounds reach Y=10, whereas
    // desired fine metadata in the small view region is flat at Y=0.
    // The frustum-like box at Y=[1,4] rejects the fine target, not the coarse draw.
    const auto visible = [&](double x0, double z0, double x1, double z1) {
        ccstd::unordered_set<uint64_t> result;
        selection.cull([&](NodeAddress n, bool leaf) {
            const double size = d.nodeSize(n.level);
            const double lowX = n.x * size;
            const double lowZ = n.z * size;
            const double maxY = n.level == 0 ? 0.0 : 10.0;
            const bool intersects = lowX < x1 && lowX + size > x0 && lowZ < z1 && lowZ + size > z0 && maxY >= 1.0;
            if (intersects && leaf) {
                require(result.insert(n.key()).second, "Duplicate visible leaf");
            }
            return intersects;
        });
        return result;
    };
    expected = demand(d.maxLevel, {}, {}); // The distance/target traversal selected nothing.
    const auto coarse = visible(0.25, 0.25, 0.75, 0.75);
    require(coarse.size() == 1 && coarse.count(NodeAddress{5, 0, 0}.key()),
            "An empty fine selection hid visible coarse geometry");
    expected = demand(d.maxLevel, {{0, 0, 0, 0, 0}}, {});
    CurrentTree::Change pending;
    const auto requests = topologyCandidates(selection, expected);
    require(!requests.empty() && selection.plan(requests.front(), pending), "Visibility transition setup");
    require(visible(0.25, 0.25, 0.75, 0.75) == coarse,
            "Unpublished refinement changed pass visibility");
    // Commit refinement until the actual draw reaches the flat fine region.
    for (uint32_t round = 0; round < 100; ++round) {
        bool changed = false;
        for (const auto &request : topologyCandidates(selection, expected)) {
            CurrentTree::Change change;
            if (selection.plan(request, change)) {
                commitTopology(selection, change, d);
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
    uint32_t visited = 0;
    selection.cull([&](NodeAddress n, bool leaf) {
        ++visited;
        require(n.level == d.maxLevel, "Rejected root still traversed its children");
        require(leaf == (leafKeys(selection).count(n.key()) != 0), "Incorrect committed leaf classification");
        return false;
    });
    require(visited == d.sectorsX * d.sectorsZ,
            "Invisible sectors must be pruned without scanning their leaves");
    std::cout << "PASS: actual-cut culling, empty targets, pending refinement, independent passes and subtree pruning\n";
}

void testStitchedPageBounds() {
    const VTPageLayout layout(4096);
    // Near-camera cell at x=1, z=0: the top edge can snap x=1 to x=0 even
    // before the old distance morph threshold. Reproduce the former undersized page.
    const auto unsafe = layout.coveringPage(0, {1, 0}, {2, 1});
    require(unsafe.level == 0 && unsafe.x == 1, "Near-edge regression setup");
    const auto horizontalMapping = layout.buildGridMapping({0, 0, 16}, 0, 0, cc::Vec3{1, 0, 0});
    const auto safe = horizontalMapping.pages[horizontalMapping.cells[1]];
    require(safe.level == 1 && safe.x == 0 && safe.z == 0,
            "Near-camera stitching escaped its explicitly planned VT page");
    const auto verticalMapping = layout.buildGridMapping({4096, 4096, 16}, 0, 0, cc::Vec3{4096, 0, 4097});
    const auto vertical = verticalMapping.pages[verticalMapping.cells[16]];
    require(vertical.level == 1 && vertical.x == 2048 && vertical.z == 2048,
            "Vertical/cross-sector stitch coverage differs from horizontal coverage");
    // Reconstruct page origins exactly as the fragment shader does, across
    // nodes, distance bands and non-power-of-two sector dimensions.
    for (const float sectorSize : {1000.0F, 2048.0F, 4096.0F}) {
        const VTPageLayout localLayout(sectorSize);
        for (const uint32_t divisions : {1U, 8U, 128U}) {
            const float size = sectorSize / divisions;
            const LandscapeRegion region{sectorSize + size, sectorSize + size, size};
            // The root must remain within its sector as required by buildGridMapping.
            const LandscapeRegion node = divisions == 1 ? LandscapeRegion{sectorSize, sectorSize, size} : region;
            for (const float distance : {0.0F, 10.0F, 100.0F, 10000.0F}) {
                const auto entries = localLayout.buildGridMapping(node, 0, 10, {node.x, distance, node.z});
                std::unordered_set<uint64_t> unique;
                for (const auto page : entries.pages) {
                    require(unique.insert(page.key()).second, "Local mapping contains duplicate page addresses");
                }
                for (uint32_t z = 0; z < 16; ++z) {
                    for (uint32_t x = 0; x < 16; ++x) {
                        const auto index = entries.cells[z * 16 + x];
                        require(index < entries.pages.size(), "Local cell references an absent page");
                        const auto page = entries.pages[index];
                        const float pageSize = localLayout.pageSize(page.level);
                        const float cellSize = size / 16;
                        require(std::floor((node.x + (x + 0.5F) * cellSize) / pageSize) == page.x &&
                                std::floor((node.z + (z + 0.5F) * cellSize) / pageSize) == page.z,
                                "Local lookup reconstructed a different page origin");
                        require(node.x + x * cellSize >= page.x * pageSize &&
                                node.z + z * cellSize >= page.z * pageSize &&
                                node.x + (x + 1) * cellSize <= (page.x + 1) * pageSize &&
                                node.z + (z + 1) * cellSize <= (page.z + 1) * pageSize,
                                "Local mapping did not cover the complete grid cell");
                    }
                }
            }
        }
    }
    std::cout << "PASS: local grid mappings preserve cell coverage, stitched bounds and shader page origins\n";
}

void testExpectedTree() {
    const auto data = cacheData(3);
    CurrentTree current;
    current.init(data);
    ExpectedTree expected(data.maxLevel);
    const auto original = leafKeys(current);
    const ccstd::vector<QuadNode> target{{0, 1, 1, 0, 1}};
    const auto next = demand(data.maxLevel, target, target);
    require(!(expected == next), "new demand must invalidate the plan");
    expected = next;
    require(expected == demand(data.maxLevel, target, target), "unchanged demand must not restart planning");
    require(expected == demand(data.maxLevel, {target.front(), target.front()}, target), "duplicate demand is semantically unchanged");
    require(expected.visible({3, 0, 0}) && expected.visible({0, 1, 1}) && !expected.visible({0, 7, 7}),
            "visibility must overlap the selected path, not unrelated siblings");
    require(leafKeys(current) == original, "changing demand must not mutate current coverage");
    const auto requests = topologyCandidates(current, expected);
    require(!requests.empty(), "expected refinement must produce a local request");
    CurrentTree::Change change;
    require(current.plan(requests.front(), change), "local balanced change");
    require(leafKeys(current) == original, "planning must retain coverage until resources are ready");
    ccstd::vector<CurrentTree::Node> prepared;
    for (const auto node : change.added) {
        prepared.push_back({{node.level, node.x, node.z, 0.25F, 1.0F}, {}});
    }
    const auto preparedKey = change.added.front().key();
    const VTPageAddress preparedPage{1, 0, 0};
    prepared.front().material.pages.push_back(preparedPage);
    current.commit(change, prepared);
    require(current.nodes().at(preparedKey).material.pages.front().key() == preparedPage.key() &&
                current.nodes().at(preparedKey).geometry.minY == 0.25F,
            "commit must publish prepared geometry and materials together");
    checkCut(current, data);
    checkStitchedEdges(current, data);
    current.clearChanges();
    const auto beforeMaterial = leafKeys(current);
    require(current.plan({NodeAddress::fromKey(preparedKey), CurrentTree::Operation::REFRESH}, change), "material refresh plan");
    prepared = {current.nodes().at(preparedKey)};
    prepared.front().material.pages = {{2, 0, 0}};
    require(current.nodes().at(preparedKey).material.pages.front().key() == preparedPage.key(),
            "preparing materials must retain the displayed mapping");
    current.commit(change, prepared);
    require(leafKeys(current) == beforeMaterial &&
                current.nodes().at(preparedKey).material.pages.front().level == 2 &&
                current.changedNodes().size() == 1 && current.changedNodes().count(preparedKey) == 1,
            "material-only commits must preserve topology and notify only their owner");
    require(current.plan({{2, 1, 0}, CurrentTree::Operation::SPLIT}, change), "refine one child before an incomplete sibling merge");
    commitTopology(current, change, data);
    require(!current.plan({{3, 0, 0}, CurrentTree::Operation::MERGE}, change) && change.empty(),
            "a rejected merge must not retain partially collected removals");
    const auto displayed = leafKeys(current);
    expected = demand(data.maxLevel, {}, {});
    require(leafKeys(current) == displayed, "a new target must not remove displayed nodes");
    require(!topologyCandidates(current, expected).empty(), "empty demand must allow resource-releasing merges");
    expected = demand(data.maxLevel, {}, {{2, 0, 0, 0, 1}});
    require(expected.visible({0, 3, 3}) && expected.visible({3, 0, 0}) && !expected.visible({0, 4, 4}),
            "coarse color demand must cover descendants without covering its siblings");
    const ccstd::vector<QuadNode> shadow{{0, 7, 7, 0, 1}};
    const ccstd::vector<QuadNode> color{{1, 0, 0, 0, 1}};
    ExpectedTree passes(data.maxLevel);
    passes.include(shadow, false);
    passes.include(color, true);
    passes.include(shadow, false);
    ExpectedTree reversed(data.maxLevel);
    reversed.include(color, true);
    reversed.include(shadow, false);
    require(passes == reversed, "pass order and duplicate shadow demand must not restart planning");
    require(passes.refinementPriority({1, 3, 3}) != 0 && !passes.visible({0, 7, 7}) && passes.visible({0, 0, 0}),
            "shadow refinement must not acquire color-pass priority");
    ccstd::unordered_set<uint64_t> refresh;
    for (const auto candidate : current.candidates(passes)) {
        if (candidate.operation == CurrentTree::Operation::REFRESH) {
            require(refresh.insert(candidate.node.key()).second, "duplicate material candidate");
        }
    }
    require(refresh == leafKeys(current), "every displayed leaf must retain a material update candidate");
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

void testPageCompositionBudget() {
    uint32_t remainingPages = config::VT_PAGE_UPDATE_BUDGET;
    VTResidencyFixture pages;
    require(pages.initPages(cacheData(3), 16), "streaming budget cache init");
    ccstd::vector<VTPageAddress> active;
    ccstd::vector<uint32_t> updates;
    uint32_t composed = 0;
    uint32_t committed = 0;
    // Two small transactions finish in one frame. The third stays pending
    // after consuming exactly the remaining pages, preserving earlier bindings.
    for (uint32_t count : {3U, 3U, 4U}) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto index = static_cast<uint32_t>(active.size());
            active.push_back({0, index % 8, index / 8});
        }
        require(pages.reserve(active), "transaction reservation failed");
        collectUpdates(pages, updates, remainingPages);
        remainingPages -= static_cast<uint32_t>(updates.size());
        composed += static_cast<uint32_t>(updates.size());
        publishPages(pages, updates);
        committed += pages.ready();
    }
    require(composed == 8 && committed == 2 && !pages.ready(), "transactions exceeded the shared eight-page budget");
    require(pages.resolve(active.front()) >= 0 && pages.resolve(active.back()) == -1,
            "partial composition damaged old bindings or published unfinished pages");
    collectUpdates(pages, updates, remainingPages);
    require(updates.empty(), "zero remaining budget still composed pages");
    remainingPages = config::VT_PAGE_UPDATE_BUDGET;
    collectUpdates(pages, updates, remainingPages);
    remainingPages -= static_cast<uint32_t>(updates.size());
    publishPages(pages, updates);
    require(updates.size() == 2 && pages.ready() && remainingPages == 6, "pending transaction did not resume exactly");
    std::cout << "PASS: shared page allowance, multiple commits and partial composition\n";
}

void testCompositionSources() {
    auto data = cacheData(7, 2, 2);
    data.sectorSize = 2048.0F;
    data.minTileLevel = 3;
    VTResidencyFixture pages;
    require(pages.initPages(data), "Initialize source selection without a source data manager");
    const auto interior = pages.selectSources({8, 2, 2}, true);
    require(interior.splat.key() == NodeAddress{4, 2, 2}.key(), "Splat source must cover the entire page");
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t col = 0; col < 2; ++col) {
            require(interior.normals[row * 2 + col].key() == NodeAddress{3, col + 4, row + 4}.key(), "Far normal sources must contain only the 2x2 interior tiles");
        }
    }
    ccstd::vector<NodeAddress> sources;
    interior.appendTo(sources);
    require(sources.size() == 5 && sources.front().key() == interior.splat.key(), "Reserve one splat and four padded normal tiles");
    interior.appendTo(sources);
    require(sources.size() == 5, "Appending to existing reservations must not duplicate sources");

    const auto corner = pages.selectSources({8, 0, 0}, true);
    sources.clear();
    corner.appendTo(sources);
    require(sources.size() == 5, "Landscape corners also need only four interior normals plus splat");
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t col = 0; col < 2; ++col) {
            require(corner.normals[row * 2 + col].key() == NodeAddress{3, col, row}.key(), "Outer boundary padding must not request tiles outside the landscape");
        }
    }
    const auto coarse = pages.selectSources({8, 2, 2}, false);
    sources.clear();
    coarse.appendTo(sources);
    require(sources.size() == 1, "Base precision shares one padded normal and splat tile without gutter neighbors");
    for (const auto normal : coarse.normals) {
        require(normal.key() == coarse.splat.key(), "All base-precision quadrants must share one source");
    }
    const auto fine = pages.selectSources({0, 17, 29}, true);
    sources.clear();
    fine.appendTo(sources);
    require(sources.size() == 1 && sources.front().key() == NodeAddress{3, 0, 0}.key(), "Sub-tile pages must respect the minimum source level");

    data.minTileLevel = 0;
    require(pages.initPages(data), "Reinitialize source precision tests");
    const auto near = pages.selectSources({5, 4, 4}, true);
    const auto nearDisabled = pages.selectSources({5, 4, 4}, false);
    for (size_t i = 0; i < near.normals.size(); ++i) {
        require(near.normals[i].key() == nearDisabled.normals[i].key(), "Far detail must not change near page sources");
    }
    require(near.normals[0].level == 1, "Near pages use one page-sized interior normal tile");
    require(pages.selectSources({6, 2, 2}, true).normals[0].level == 1, "Far detail starts at VT L6 with half-page sources");
    require(pages.selectSources({6, 2, 2}, false).normals[0].level == 2, "Disabling far detail retains page-sized normal sources");

    // Mirror the shader lookup at interior, edge and corner samples, including
    // the full gutter. Each padded tile must cover the point without neighbor reads.
    const float border = static_cast<float>(config::VT_PAGE_BORDER) / config::VT_PAGE_INTERIOR;
    const float samples[] = {-border, 0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 1.0F + border};
    const VTPageLayout layout(data.sectorSize);
    for (uint32_t level = 0; level <= layout.rootLevel(); ++level) {
        const float size = layout.pageSize(level);
        const uint32_t last = static_cast<uint32_t>(data.width() / size) - 1;
        for (const uint32_t x : {0U, last / 2, last}) {
            for (const uint32_t z : {0U, last / 2, last}) {
                for (const bool detailed : {false, true}) {
                    const auto inputs = pages.selectSources({level, x, z}, detailed);
                    for (const float u : samples) {
                        for (const float v : samples) {
                            const auto col = static_cast<uint32_t>(std::clamp(static_cast<int>(std::floor(u * 2)), 0, 1));
                            const auto row = static_cast<uint32_t>(std::clamp(static_cast<int>(std::floor(v * 2)), 0, 1));
                            const auto region = data.nodeRegion(inputs.normals[row * 2 + col]);
                            const float worldX = (x + u) * size;
                            const float worldZ = (z + v) * size;
                            const float padding = region.size * config::SOURCE_NORMAL_BORDER / (config::SOURCE_TILE_RESOLUTION - 1);
                            require(worldX >= region.x - padding && worldX <= region.x + region.size + padding && worldZ >= region.z - padding && worldZ <= region.z + region.size + padding, "Padded normal lookup must cover all gutter edges and corners at every VT level");
                        }
                    }
                }
            }
        }
    }

    const ccstd::vector<VTPageAddress> toggledPages{{5, 4, 4}, {6, 2, 2}};
    require(pages.reserve(toggledPages), "Reserve near and far toggle test pages");
    publishPages(pages, pages.reservedSlots());
    pages.setRefineFarDetail(false);
    require(!pages.getRefineFarDetail(), "Far detail toggle state must update");
    require(!pages.needsComposition(toggledPages[0]) && pages.needsComposition(toggledPages[1]), "Far detail must invalidate only far pages");
    require(pages.resolve(toggledPages[1]) >= 0, "Far pages remain drawable while their new normals are prepared");
    publishPages(pages, pages.reservedSlots());
    pages.setRefineFarDetail(false);
    require(!pages.needsComposition(toggledPages[1]), "Unchanged toggle must not invalidate pages");
    pages.setRefineFarDetail(true);
    require(!pages.needsComposition(toggledPages[0]) && pages.needsComposition(toggledPages[1]), "Re-enabling far detail must refresh only far pages");
}

void testPageTableUpdatesAndReuse() {
    VTResidencyFixture pages;
    require(pages.initPages(cacheData(8)), "Page table cache initialization failed");
    VTGridMapping mapping;
    for (uint32_t i = 0; i < VTGridMapping::CELL_COUNT; ++i) {
        mapping.pages.push_back({4, i % 16, i / 16});
        mapping.cells[i] = static_cast<uint8_t>(i);
    }
    require(pages.reserve(mapping.pages), "All 256 physical pages must fit");
    publishPages(pages, pages.reservedSlots());
    VTPageTableFixture table;
    table.resizeEntries(4 * VTGridMapping::CELL_COUNT);
    const auto first = table.allocate();
    const auto second = table.allocate();
    require(first == 0 && second == 1, "Page table allocations must be consecutive");
    const auto original = mapping;
    require(table.update(first, original, pages), "Initialize neighboring grid mapping");
    require(table.update(second, mapping, pages), "New grid mapping was not detected");
    require(!table.update(second, mapping, pages), "Identical grid mapping must avoid another upload");
    require(!table.update(first, original, pages), "Mapping overwrote a neighboring grid");
    std::reverse(mapping.cells.begin(), mapping.cells.end());
    require(table.update(second, mapping, pages), "Changed mapping must be uploaded");
    require(!table.update(second, mapping, pages), "Updated mapping must remain stable");
    require(!table.update(first, original, pages), "Changed mapping overwrote a neighboring grid");
    table.release(first);
    require(table.allocate() == first, "Released grid mapping slot must be reused");
    require(table.update(first, mapping, pages), "Reused mapping slot was not written");
    table.destroy();
    table.resizeEntries(1 * VTGridMapping::CELL_COUNT);
    require(table.allocate() == 0, "Page table destroy must reset slot allocation");
}

void testVTDistanceThresholds() {
    for (const float sectorSize : {0.125F, 1.0F, 1.5F, 3.0F, 129.0F, 1000.25F, 16384.0F}) {
        const VTPageLayout layout(sectorSize);
        require(layout.levelForDistance(0.0F) == 0, "Zero distance must select the finest page");
        for (uint32_t level = 0; level <= layout.rootLevel(); ++level) {
            const float threshold = layout.pageSize(level) * 2.0F;
            require(layout.levelForDistance(threshold) == level, "Exact distance threshold must keep the finer page");
            require(layout.levelForDistance(std::nextafter(threshold, 0.0F)) == level, "Distance just below the threshold changed level");
            require(layout.levelForDistance(std::nextafter(threshold, std::numeric_limits<float>::max())) == std::min(level + 1, layout.rootLevel()), "Distance just above the threshold must select the coarser page");
        }
    }
}

int main() {
    testVTDistanceThresholds();
    testPageTableUpdatesAndReuse();
    testCompositionSources();
    testSectorCountLimit();
    testHeightReservations();
    testExactPageLifecycle();
    testPageRefreshAndReuse();
    testRegionalInvalidation();
    testCapacityLimitedTransitions();
    testLocalTransitions();
    testIndexedDemand();
    testCommittedVisibility();
    testStitchedPageBounds();
    testExpectedTree();
    testAtlasFilterFootprint();
    testCoordinateLayouts();
    testPageCompositionBudget();
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
                            const auto page = pageLayout.coveringPage(desired,
                                {sectorOrigin + (x - (x & 1U)) * cell, sectorOrigin + (z - (z & 1U)) * cell},
                                {sectorOrigin + (x + cells) * cell, sectorOrigin + (z + cells) * cell});
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

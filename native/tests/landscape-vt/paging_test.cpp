// Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.
#include "landscape/Asset.h"
#include "landscape/Quadtree.h"
#include "landscape/Residency.h"
#include "landscape/VTPageStages.h"
#include "landscape/VTRenderer.h"
#include "base/Log.h"
#include "base/std/container/unordered_map.h"
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
#include "core/geometry/Frustum.h"
#include "renderer/gfx-base/GFXTexture.h"

#include <cstdlib>

// This executable exercises the real LandscapeResidency reservation methods without
// a device or asset loader. Tree tests supply synthetic metadata and camera
// frusta. Fail immediately if a test enters any of these unprovided paths.
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
// This executable supplies synthetic assets and never starts asset IO.
LandscapeAsset::LandscapeAsset() = default;
LandscapeAsset::~LandscapeAsset() = default;

VTPageComposer::VTPageComposer() {
    std::abort();
}
bool VTPageComposer::init(const LandscapeAsset &, LandscapeResidency &, const MaterialLibrary &, DecalRenderer &, bool) {
    std::abort();
}
bool VTPageComposer::prepareInputs(uint32_t, LandscapeRegion, const VTPageSources &, const LandscapeResidency &, VTPageInstance &) {
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
bool VTPageComposer::setTriplanarEnabled(bool) {
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
#include <optional>
#include <random>
#include <set>

using namespace cc::landscape;

namespace {
// Stack-owned CPU fixtures expose protected state operations only to this suite.
// Production objects retain their complete GPU initialization and submission paths.
class ResidencyFixture final : public LandscapeResidency {
public:
    using LandscapeResidency::initSlots;
    using LandscapeResidency::publish;
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

VTGridMapping testMapping(NodeAddress address) {
    VTGridMapping mapping;
    mapping.pages = {{address.level, address.x, address.z}};
    return mapping;
}

class TreeFixture final : public Quadtree {
public:
    void init(const LandscapeData &data, bool initializeResources = true) {
        const ccstd::vector<HeightRange> ranges(NodeIndexLayout(data).nodeCount(), {data.minHeight(), data.maxHeight()});
        LandscapeAsset asset;
        // The fixture owns this non-const asset. Populate its synthetic metadata
        // through the existing views, then exercise the public initialization path.
        const_cast<LandscapeData &>(asset.data()) = data;
        const_cast<ccstd::vector<HeightRange> &>(asset.heightRanges()) = ranges;
        Quadtree::init(asset);
        if (initializeResources) {
            for (uint32_t z = 0; z < data.sectorsZ; ++z) {
                for (uint32_t x = 0; x < data.sectorsX; ++x) {
                    const NodeAddress address{data.maxLevel, x, z};
                    require(publish(address, testMapping(address)), "Initialize synthetic sector resources");
                }
            }
            require(selectRenderables(), "Initial sector coverage");
        }
    }
};

// Test inputs describe narrow camera frusta, not a second production tree.
struct CameraView {
    ccstd::vector<QuadNode> geometry;
    ccstd::vector<QuadNode> surface;
};

void applyView(Quadtree &tree, const CameraView &view) {
    ccstd::unordered_map<uint64_t, uint8_t> previous;
    tree.forEachRenderable([&](const Quadtree::Node &node) {
        const auto address = node.geometry.address();
        previous.emplace(address.key(), node.edgeMask);
    });
    tree.clearChangedNodes();
    const auto &targets = view.geometry.empty() ? view.surface : view.geometry;
    // An empty view represents a distant camera, not a hidden nearby terrain.
    cc::Vec3 camera{0, tree.lodMorphRanges().back().end * 4, 0};
    if (!targets.empty()) {
        const auto area = tree.region(targets.front().address());
        camera.set(area.x + area.size * 0.5F, 0.5F, area.z + area.size * 0.5F);
    }
    tree.beginUpdate(camera, cc::Vec3::ZERO);
    for (const auto *pass : {&view.geometry, &view.surface}) {
        for (const auto &target : *pass) {
            const auto area = tree.region(target.address());
            cc::geometry::Frustum frustum;
            frustum.planes[0]->n.set(1, 0, 0);
            frustum.planes[0]->d = area.x + 0.01F;
            frustum.planes[1]->n.set(-1, 0, 0);
            frustum.planes[1]->d = -(area.x + area.size - 0.01F);
            frustum.planes[2]->n.set(0, 0, 1);
            frustum.planes[2]->d = area.z + 0.01F;
            frustum.planes[3]->n.set(0, 0, -1);
            frustum.planes[3]->d = -(area.z + area.size - 0.01F);
            frustum.planes[4]->n.set(0, 1, 0);
            frustum.planes[4]->d = -1000;
            frustum.planes[5]->n.set(0, -1, 0);
            frustum.planes[5]->d = -1000;
            tree.addPass(frustum, pass == &view.surface);
        }
    }
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    // Target updates may delete old children. Subsequent selection must report their
    // retired draws and changed shared edges before the renderer consumes them.
    for (const auto &entry : previous) {
        if (!tree.findRenderable(NodeAddress::fromKey(entry.first))) {
            require(tree.changedNodes().count(entry.first) != 0, "Deleted target draw lost its retirement notification");
        }
    }
    tree.forEachRenderable([&](const Quadtree::Node &node) {
        const auto address = node.geometry.address();
        const auto old = previous.find(address.key());
        const auto edges = node.edgeMask;
        if (old == previous.end() || old->second != edges) {
            require(tree.changedNodes().count(address.key()) != 0, "Target change lost a morph edge notification");
        }
    });
}

// Compare topology independently of the material records now owned by Quadtree.
ccstd::unordered_set<uint64_t> renderableKeys(const Quadtree &tree) {
    ccstd::unordered_set<uint64_t> result;
    tree.forEachRenderable([&](const Quadtree::Node &node) {
        result.insert(node.geometry.address().key());
    });
    return result;
}

ccstd::unordered_set<uint64_t> targetKeys(const Quadtree &tree) {
    ccstd::unordered_set<uint64_t> result;
    tree.forEachNode([&](const Quadtree::Node &node) {
        if (!node.children) {
            result.insert(node.geometry.address().key());
        }
        return true;
    });
    return result;
}

// Inspect coverage through public traversal rather than exposing engine internals.
NodeAddress rootAddress(const Quadtree &tree) {
    NodeAddress root;
    tree.forEachNode([&](const Quadtree::Node &node) {
        root = node.geometry.address();
        return false;
    });
    return root;
}

NodeAddress renderableAt(const Quadtree &tree, uint32_t x, uint32_t z) {
    NodeAddress result;
    bool found = false;
    tree.forEachRenderable([&](const Quadtree::Node &node) {
        const auto address = node.geometry.address();
        if ((x >> address.level) == address.x && (z >> address.level) == address.z) {
            require(!found, "Multiple render nodes cover the same point");
            result = address;
            found = true;
        }
    });
    require(found, "No render node covers the requested point");
    return result;
}

// Verify changed-node notifications independently of the production selection.
void selectAndCheckChanges(Quadtree &tree) {
    ccstd::unordered_map<uint64_t, uint8_t> previous;
    tree.forEachRenderable([&](const Quadtree::Node &node) {
        const auto address = node.geometry.address();
        previous.emplace(address.key(), node.edgeMask);
    });
    tree.clearChangedNodes();
    require(tree.selectRenderables(), "Complete ready roots must always provide coverage");
    for (const auto &entry : previous) {
        if (!tree.findRenderable(NodeAddress::fromKey(entry.first))) {
            require(tree.changedNodes().count(entry.first) != 0, "Removed draw was not reported");
        }
    }
    tree.forEachRenderable([&](const Quadtree::Node &node) {
        const auto address = node.geometry.address();
        const auto old = previous.find(address.key());
        const auto edges = node.edgeMask;
        require(node.ready, "Selected node is not ready");
        if (old == previous.end() || old->second != edges) {
            require(tree.changedNodes().count(address.key()) != 0, "Morph neighbor notification was lost");
        }
    });
}

void completeTarget(Quadtree &tree) {
    for (uint32_t step = 0; step < 4096; ++step) {
        bool loaded = false;
        for (const auto &request : tree.requests()) {
            if (!tree.find(request)->ready) {
                require(tree.publish(request, testMapping(request)), "Parent must precede child readiness");
                loaded = true;
            }
        }
        selectAndCheckChanges(tree);
        if (!loaded) {
            require(renderableKeys(tree) == targetKeys(tree), "Ready selection must converge to its balanced target");
            return;
        }
    }
    require(false, "Target loading did not converge");
}

CameraView testView(const ccstd::vector<QuadNode> &geometry, const ccstd::vector<QuadNode> &surface = {}) {
    return {geometry, surface};
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
        ResidencyFixture residency;
        require(residency.initSlots(d, config::PAGE_POOL_LAYERS), "Initialize source reservations");
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
        require(residency.reserve(roots) && pages.reserve(rootPages), "maximum root coverage must fit both caches");
    }
    for (const auto dimensions : {std::pair<uint32_t, uint32_t>{65, 1}, {1, 65}, {9, 8}, {65536, 65536}}) {
        const auto d = cacheData(4, dimensions.first, dimensions.second);
        require(!d.valid() && NodeIndexLayout(d).nodeCount() == 0, "excess sector count must be rejected without overflow");
    }
}

void testHeightReservations() {
    ResidencyFixture residency;
    require(residency.initSlots(cacheData(3), 5), "Initialize source reservations");
    const NodeAddress root{3, 0, 0};
    require(residency.reserve({root}), "reserve exact height root");
    residency.publish(root.key());
    const int layer = residency.resident(root);
    require(residency.resident({0, 0, 0}) == -1, "Missing height source must not resolve to a ready root");
    ccstd::vector<NodeAddress> transition{root, {2, 0, 0}, {2, 1, 0}, {2, 0, 1}, {2, 1, 1}};
    require(residency.reserve(transition), "all-or-nothing height split reservation");
    require(residency.resident(root) == layer && !residency.ready(), "Displayed height layer moved or incomplete reservation became ready");
    for (size_t i = 1; i < transition.size(); ++i) {
        require(residency.resident(transition[i]) == -1, "Reservation was mistaken for uploaded data");
    }
    auto excess = transition;
    excess.push_back({0, 0, 0});
    require(!residency.reserve(excess) && residency.resident(root) == layer && !residency.ready(), "Capacity refusal modified live slots");
    std::set<uint32_t> layers;
    ccstd::vector<int> bindings;
    // Publish exactly the admitted inputs. Missing reservations fail publication;
    // extra unfinished reservations keep ready() false. Inspect real draw bindings.
    for (const auto source : transition) {
        residency.publish(source.key());
        const int binding = residency.resident(source);
        require(binding >= 0, "Admitted source did not become resident");
        bindings.push_back(binding);
        layers.insert(static_cast<uint32_t>(binding));
    }
    require(residency.ready() && layers.size() == 5 && *layers.rbegin() < residency.layerCount(), "Reserved layers overlap, exceed capacity or retain unexpected pending inputs");
    require(residency.reserve(transition) && residency.ready() && residency.resident(root) == layer,
            "Reserving the same unique working set changed its bindings");
    for (size_t i = 0; i < transition.size(); ++i) {
        require(residency.resident(transition[i]) == bindings[i], "Repeated reservation changed a child binding");
    }
    transition.erase(transition.begin());
    require(residency.reserve(transition), "release retired parent");
    for (const auto n : transition) {
        residency.publish(n.key());
    }
    require(residency.resident(root) == -1, "Retired height binding survived");
    // The released slot permits a later merge even though all four children
    // remain protected until the replacement parent is uploaded.
    transition.push_back(root);
    require(residency.reserve(transition), "merge transition cannot make progress");
    require(residency.resident(root) == -1, "Reallocated parent incorrectly inherited old ready state");
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

// Independent raster oracle: full coverage, no overlaps, 2:1 at EVERY cell
// boundary, and no avoidable coarse draws. Does not use production edge masks.
void checkCut(const Quadtree &selection, const LandscapeData &d, bool target = false) {
    const auto keys = target ? targetKeys(selection) : renderableKeys(selection);
    const uint32_t w = d.sectorsX << d.maxLevel, h = d.sectorsZ << d.maxLevel;
    std::vector<int> cells(w * h, -1);
    for (const auto key : keys) {
        const auto n = NodeAddress::fromKey(key);
        if (!target) {
            require(selection.findRenderable(n) != nullptr, "Draw lookup disagrees with traversal");
        }
        for (uint32_t z = n.z << n.level; z < (n.z + 1) << n.level; ++z) {
            for (uint32_t x = n.x << n.level; x < (n.x + 1) << n.level; ++x) {
                require(cells[z * w + x] == -1, "Draw coverage overlaps");
                cells[z * w + x] = static_cast<int>(n.level);
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
    if (target) {
        return;
    }
    for (const auto key : keys) {
        const auto address = NodeAddress::fromKey(key);
        const auto &node = *selection.find(address);
        if (!node.children || !std::all_of(node.children.get(), node.children.get() + 4, [](const Quadtree::Node &child) { return child.ready; })) {
            continue;
        }
        const uint32_t x = address.x << address.level;
        const uint32_t z = address.z << address.level;
        const uint32_t size = 1U << address.level;
        const int level = static_cast<int>(address.level);
        bool blocked = false;
        for (uint32_t offset = 0; offset < size; ++offset) {
            blocked |= x > 0 && cells[(z + offset) * w + x - 1] > level;
            blocked |= x + size < w && cells[(z + offset) * w + x + size] > level;
            blocked |= z > 0 && cells[(z - 1) * w + x + offset] > level;
            blocked |= z + size < h && cells[(z + size) * w + x + offset] > level;
        }
        require(blocked, "Ready detail was omitted without a coarser neighbor blocking refinement");
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

void checkStitchedEdges(const Quadtree &selection, const LandscapeData &d) {
    ccstd::vector<QuadNode> nodes;
    for (const auto key : renderableKeys(selection)) {
        const auto n = NodeAddress::fromKey(key);
        nodes.push_back({n.level, n.x, n.z, 0, 1});
    }
    for (const auto &node : nodes) {
        const auto n = node.address();
        const double size = d.nodeSize(n.level), x = n.x * size, z = n.z * size, step = size / 16;
        const auto mask = selection.findRenderable(n)->edgeMask;
        for (uint32_t edge = 0; edge < 4; ++edge) {
            if ((edge == 0 && x == 0) || (edge == 1 && x + size == d.width()) || (edge == 2 && z == 0) || (edge == 3 && z + size == d.depth())) {
                continue;
            }
            for (uint32_t j = 0; j <= 16; ++j) {
                const double along = (mask & (1U << (edge * 2))) ? j - (j & 1) : j;
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
                    require(((mask & (1U << (edge * 2))) != 0) == (peer.level > n.level), "Coarser edge mask does not describe the actual neighbor");
                    require(((mask & (2U << (edge * 2))) != 0) == (peer.level < n.level), "Finer edge mask does not describe the actual neighbor");
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

// CPU resource driver: real reservations and tree selection, synthetic IO/GPU
// completion. This fixture deliberately has no split/merge or topology planner.
class StreamingFixture {
public:
    StreamingFixture(LandscapeData data, uint32_t sourceCapacity, uint32_t pageCapacity)
    : data(data) {
        tree.init(data);
        require(residency.initSlots(data, sourceCapacity), "Initialize source cache");
        require(pages.initPages(data, pageCapacity), "Initialize VT cache");
        require(reserve(std::nullopt), "Coarse coverage must fit");
        complete(256);
    }

    void setView(const CameraView &view) {
        applyView(tree, view);
        selectAndCheckChanges(tree);
        if (pending && !tree.find(*pending)) {
            pending.reset();
        }
        require(reserve(pending), "Target coarsening must not increase reservations");
    }

    bool request() {
        if (pending) {
            return true;
        }
        for (const auto &entry : tree.requests()) {
            if (tree.find(entry)->ready) {
                continue;
            }
            if (reserve(entry)) {
                pending = entry;
                return true;
            }
            ++refused;
        }
        return false;
    }

    void complete(uint32_t budget, std::mt19937 *random = nullptr) {
        for (const auto source : sources) {
            if (!random || (*random)() % 3 != 0) {
                residency.publish(source.key());
            }
        }
        if (residency.ready()) {
            ccstd::vector<uint32_t> updates;
            collectUpdates(pages, updates, budget);
            publishPages(pages, updates);
        }
        if (pending && residency.ready() && pages.ready()) {
            require(tree.publish(*pending, testMapping(*pending)), "Ready resource publication failed");
            pending.reset();
            ++published;
            selectAndCheckChanges(tree);
            require(reserve(std::nullopt), "Ready ancestors must fit after publication");
        }
    }

    void check() const {
        checkCut(tree, data);
        tree.forEachNode([&](const Quadtree::Node &node) {
            if (!node.ready) {
                return true;
            }
            require(node.loaded, "Ready node must retain its loaded source data");
            const auto address = node.geometry.address();
            if (address.level < data.maxLevel) {
                require(tree.find(address.ancestor(address.level + 1))->ready, "Ready ancestor invariant violated");
            }
            const auto source = address.ancestor(std::max(address.level, data.minTileLevel));
            require(residency.resident(source) >= 0, "Ready node lost its exact height source");
            for (const auto page : node.material.pages) {
                require(pages.resolve(page) >= 0, "Ready ancestor lost its material page");
            }
            return true;
        });
    }

    bool reserve(std::optional<NodeAddress> demand) {
        ccstd::vector<NodeAddress> required;
        ccstd::vector<VTPageAddress> material;
        ccstd::unordered_set<uint64_t> unique;
        const auto add = [&](NodeAddress address) {
            const auto source = address.ancestor(std::max(address.level, data.minTileLevel));
            if (unique.insert(source.key()).second) {
                required.push_back(source);
            }
            material.push_back({address.level, address.x, address.z});
        };
        tree.forEachNode([&](const Quadtree::Node &node) {
            if (node.ready) {
                add(node.geometry.address());
            }
            return true;
        });
        if (demand) {
            add(*demand);
        }
        if (!residency.canReserve(required) || !pages.canReserve(material)) {
            return false;
        }
        require(residency.reserve(required) && pages.reserve(material), "Validated reservation failed");
        sources = std::move(required);
        return true;
    }

    LandscapeData data;
    TreeFixture tree;
    ResidencyFixture residency;
    VTResidencyFixture pages;
    std::optional<NodeAddress> pending;
    ccstd::vector<NodeAddress> sources;
    size_t refused{0};
    size_t published{0};
};

void testCapacityLimitedTransitions() {
    const auto data = cacheData(4, 2, 2);
    for (const auto capacities : {std::pair<uint32_t, uint32_t>{4, 256}, {256, 4}, {4, 4},
                                 {7, 256}, {256, 7}, {7, 7}, {8, 8}, {12, 256}, {256, 12},
                                 {12, 12}, {19, 19}, {20, 256}, {256, 20}, {20, 20}}) {
        ccstd::unordered_set<uint64_t> immediateCompletion;
        for (const bool delayed : {false, true}) {
            StreamingFixture stream(data, capacities.first, capacities.second);
            for (const auto target : {QuadNode{0, 15, 15, 0, 1}, QuadNode{0, 24, 24, 0, 1},
                                     QuadNode{0, 0, 0, 0, 1}, QuadNode{0, 31, 0, 0, 1},
                                     QuadNode{0, 0, 31, 0, 1}, QuadNode{0, 31, 31, 0, 1}}) {
                stream.setView(testView({target}));
                uint32_t step = 0;
                while (stream.request()) {
                    require(++step < 1024, "Capacity-limited loading failed to settle");
                    const auto before = renderableKeys(stream.tree);
                    const bool missingPages = !stream.pages.ready();
                    stream.complete(0);
                    if (missingPages) {
                        require(renderableKeys(stream.tree) == before, "Missing pages changed coverage");
                    }
                    stream.complete(delayed ? 1U : 256U);
                    stream.check();
                }
                stream.check();
                checkStitchedEdges(stream.tree, data);
                if (std::min(capacities.first, capacities.second) < 8) {
                    require(renderableKeys(stream.tree).size() == 4, "Incomplete sibling resources must retain all sector roots");
                }
            }
            require(stream.refused > 0, "Capacity test never refused a demand");
            if (!delayed) {
                immediateCompletion = renderableKeys(stream.tree);
            } else {
                require(renderableKeys(stream.tree) == immediateCompletion, "Immediate and delayed completion diverged");
            }
        }
    }
    std::cout << "PASS: ancestor reservations, source/VT limits, delayed completion and moving views\n";
}

void testStreamingSelection() {
    auto data = cacheData(4, 2, 2);
    data.minTileLevel = 2;
    StreamingFixture stream(data, 64, 64);
    std::mt19937 random(731);
    for (uint32_t frame = 0; frame < 1600; ++frame) {
        const uint32_t quadrant = frame / 80;
        const uint32_t x = quadrant & 1 ? 31 : 0;
        const uint32_t z = quadrant & 2 ? 31 : 0;
        stream.setView(testView({{0, x, z, 0, 1}}));
        stream.request();
        stream.complete(frame % 19 == 0 ? 0U : 1U, &random);
        stream.check();
        if (frame % 16 == 0) {
            checkStitchedEdges(stream.tree, data);
        }
    }
    require(stream.published > 20, "Streaming test did not exercise enough asynchronous completions");
    std::cout << "PASS: 1600 frames, moving targets, delayed IO/pages, ancestor retention and stitched edges\n";
}

void testSingleRoot() {
    for (const auto dimensions : {std::pair<uint32_t, uint32_t>{1, 1}, {2, 2}, {3, 3}, {3, 5}, {64, 1}, {1, 64}, {8, 8}}) {
        const auto d = cacheData(3, dimensions.first, dimensions.second);
        TreeFixture tree;
        tree.init(d);
        const auto childMask = [&](NodeAddress address) {
            const auto *node = tree.find(address);
            require(node && node->children, "Management node must own four children");
            uint8_t mask = 0;
            for (uint32_t q = 0; q < 4; ++q) {
                if (tree.find(node->children[q].geometry.address())) {
                    mask |= static_cast<uint8_t>(1U << q);
                } else {
                    require(!node->children[q].children && !node->children[q].ready, "Empty sector must not allocate descendants or load resources");
                }
            }
            return mask;
        };
        uint32_t side = 1;
        uint32_t levels = 0;
        while (side < std::max(d.sectorsX, d.sectorsZ)) {
            side *= 2;
            ++levels;
        }
        require(rootAddress(tree).level == d.maxLevel + levels, "Incorrect padded global root");
        uint32_t renderableCount = 0;
        tree.cull([&](NodeAddress node, bool renderable) {
            if (renderable) {
                ++renderableCount;
                require(node.level == d.maxLevel && node.x < d.sectorsX && node.z < d.sectorsZ, "Absent sector became a renderable");
            }
            if (node.level > d.maxLevel) {
                require(childMask(node) != 0, "Empty management branch");
            }
            return true;
        });
        require(renderableCount == d.sectorsX * d.sectorsZ, "Single root does not cover all sectors");
        if (d.sectorsX == 3 && d.sectorsZ == 3) {
            require(childMask(rootAddress(tree)) == 15, "3x3 root must have four valid quadrants");
            require(childMask({d.maxLevel + 1, 0, 0}) == 15, "3x3 upper-left group");
            require(childMask({d.maxLevel + 1, 1, 0}) == 5, "3x3 upper-right group");
            require(childMask({d.maxLevel + 1, 0, 1}) == 3, "3x3 lower-left group");
            require(childMask({d.maxLevel + 1, 1, 1}) == 1, "3x3 lower-right group");
        }
        if (levels) {
            require(!tree.publish(rootAddress(tree), testMapping(rootAddress(tree))), "Management root must not own render resources");
        }
        checkCut(tree, d);
    }
    std::cout << "PASS: single padded root, 3x3 child masks, rectangular and 64-sector coverage\n";
}

void testReadySelection() {
    const auto data = cacheData(3, 3, 3);
    TreeFixture tree;
    tree.init(data, false);
    require(!tree.selectRenderables() && renderableKeys(tree).size() == 0, "Unready roots must not publish partial landscape coverage");
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            const NodeAddress address{3, x, z};
            require(tree.publish(address, testMapping(address)), "Coarse resource publication failed");
            if (x != 2 || z != 2) {
                require(!tree.selectRenderables(), "Initialization published incomplete roots");
            }
        }
    }
    selectAndCheckChanges(tree);
    const auto roots = renderableKeys(tree);
    const auto near = testView({{0, 7, 0, 0, 1}});
    applyView(tree, near);
    const auto target = targetKeys(tree);
    require(target.count(NodeAddress{0, 7, 0}.key()) != 0 && target.size() > roots.size(), "Distance target must be complete before loading");
    require(renderableKeys(tree) == roots, "Target creation changed the rendered coverage");
    require(!tree.publish({0, 7, 0}, testMapping({0, 7, 0})), "Child became ready before its ancestor");
    auto &sector = *tree.find({3, 0, 0});
    for (uint32_t q = 0; q < 4; ++q) {
        require(!sector.children[q].loaded && !sector.children[q].ready, "New target nodes must start unloaded");
        sector.children[q].loaded = true;
    }
    selectAndCheckChanges(tree);
    require(renderableKeys(tree) == roots, "Loaded data without composition must not replace the parent");
    for (uint32_t q = 0; q < 3; ++q) {
        const auto address = sector.children[q].geometry.address();
        require(tree.publish(address, testMapping(address)), "Publish partial sibling resources");
    }
    selectAndCheckChanges(tree);
    require(renderableKeys(tree) == roots, "Three ready children must not replace their parent");
    const auto fourth = sector.children[3].geometry.address();
    require(tree.publish(fourth, testMapping(fourth)), "Publish last sibling");
    selectAndCheckChanges(tree);
    require(sector.ready && !sector.renderable && !sector.material.pages.empty(), "Refinement released its ready ancestor");
    checkCut(tree, data);
    // Load only one sector deeply. Neighbor roots stay coarse, so selection must
    // retain coarse render nodes despite ready fine resources at the boundary.
    for (uint32_t round = 0; round < 4; ++round) {
        for (const auto &request : tree.requests()) {
            const auto owner = request.ancestor(data.maxLevel);
            if (owner.x == 0 && owner.z == 0 && !tree.find(request)->ready) {
                require(tree.publish(request, testMapping(request)), "Load fine boundary resources");
            }
        }
        selectAndCheckChanges(tree);
        checkCut(tree, data);
        checkStitchedEdges(tree, data);
    }
    require(tree.find({0, 7, 0})->ready && renderableAt(tree, 7, 0).level >= 2, "Selection refined past a coarse neighbor at the sector boundary");
    require(targetKeys(tree) == target, "Resource readiness rewrote the distance target");
    completeTarget(tree);
    checkCut(tree, data);
    checkStitchedEdges(tree, data);
    require(renderableKeys(tree) == target, "Ready balanced target must retain all requested detail");
    applyView(tree, {});
    selectAndCheckChanges(tree);
    require(renderableKeys(tree) == roots && !sector.children && sector.ready, "Coarsening must retain the parent and reclaim descendants");
    applyView(tree, near);
    const NodeAddress pending{2, 0, 0};
    require(tree.find(pending) && !tree.find(pending)->ready, "Prepare moving-view load");
    applyView(tree, {});
    selectAndCheckChanges(tree);
    require(!tree.find(pending) && !tree.publish(pending, testMapping(pending)), "Retired request resurrected an obsolete node");
    applyView(tree, near);
    completeTarget(tree);
    require(renderableKeys(tree) == target, "Returning camera did not converge to the same target");
    checkCut(tree, data, true);
    std::cout << "PASS: ready ancestors, partial siblings, balanced selection and moving targets\n";
}

void testUpdateStages() {
    const auto data = cacheData(3, 3, 3);
    TreeFixture tree;
    tree.init(data, false);
    tree.beginUpdate({7.5F, 0.5F, 7.5F}, cc::Vec3::ZERO);
    tree.updateTarget();
    require(!tree.sectorRootsReady() && renderableKeys(tree).size() == 0, "Target stage published unready draws");
    require(targetKeys(tree).size() > data.sectorsX * data.sectorsZ, "Target stage waited for resources");
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            const NodeAddress address{data.maxLevel, x, z};
            require(tree.publish(address, testMapping(address)), "Prepare initial coverage without selecting draws");
        }
    }
    require(tree.sectorRootsReady() && renderableKeys(tree).size() == 0, "Resource readiness must be independent of render selection");
    // Several publications may share one resource stage. No intermediate draw
    // selection is needed to request the next generation of resources.
    for (uint32_t level = 0; level < data.maxLevel; ++level) {
        for (const auto &request : tree.requests()) {
            if (!tree.find(request)->ready) {
                require(tree.publish(request, testMapping(request)), "Prepare descendants before selecting draws");
            }
        }
        require(renderableKeys(tree).size() == 0, "Resource publication implicitly selected draws");
    }
    require(tree.selectRenderables(), "Select the prepared render set once");
    require(tree.renderSelectionChanged() && renderableKeys(tree) == targetKeys(tree), "Batched publication did not reach the prepared target");
    checkCut(tree, data);
    checkStitchedEdges(tree, data);
    const auto old = renderableKeys(tree);
    tree.clearChangedNodes();
    tree.beginUpdate({0, tree.lodMorphRanges().back().end * 4, 0}, cc::Vec3::ZERO);
    tree.updateTarget();
    require(tree.changedNodes().empty(), "Target update performed render synchronization");
    require(tree.selectRenderables(), "Select coarse coverage after target deletion");
    for (const auto key : old) {
        if (!tree.findRenderable(NodeAddress::fromKey(key))) {
            require(tree.changedNodes().count(key) != 0, "Stage boundary lost a retired draw notification");
        }
    }
    checkCut(tree, data);
    tree.clearChangedNodes();
    require(tree.selectRenderables() && !tree.renderSelectionChanged() && tree.changedNodes().empty(), "Stable selection repeated draw changes");
    std::cout << "PASS: separate target, resource and selection stages, batched readiness and retired draws\n";
}

void testCommittedVisibility() {
    const auto d = cacheData(5, 2, 2);
    TreeFixture selection;
    selection.init(d);
    CameraView view;
    // A coarse mesh bridges a small valley: its bounds reach Y=10, whereas
    // desired fine metadata in the small view region is flat at Y=0.
    // The frustum-like box at Y=[1,4] rejects the fine target, not the coarse draw.
    const auto visible = [&](double x0, double z0, double x1, double z1) {
        ccstd::unordered_set<uint64_t> result;
        selection.cull([&](NodeAddress n, bool renderable) {
            const double size = selection.region(n).size;
            const double lowX = n.x * size;
            const double lowZ = n.z * size;
            const double maxY = n.level == 0 ? 0.0 : 10.0;
            const bool intersects = lowX < x1 && lowX + size > x0 && lowZ < z1 && lowZ + size > z0 && maxY >= 1.0;
            if (intersects && renderable) {
                require(result.insert(n.key()).second, "Duplicate visible renderable");
            }
            return intersects;
        });
        return result;
    };
    view = testView({}, {}); // Initial coverage consists of ready sector roots.
    const auto coarse = visible(0.25, 0.25, 0.75, 0.75);
    require(coarse.size() == 1 && coarse.count(NodeAddress{5, 0, 0}.key()),
            "An empty fine selection hid visible coarse geometry");
    view = testView({{0, 0, 0, 0, 0}}, {});
    applyView(selection, view);
    require(visible(0.25, 0.25, 0.75, 0.75) == coarse,
            "Unready target changed pass visibility");
    completeTarget(selection);
    require(renderableAt(selection, 0, 0).level == 0 && visible(0.25, 0.25, 0.75, 0.75).empty(),
            "Culling must follow the committed geometry's height bounds");
    // Independent shadow/camera frusta must still find a distant coarse sector.
    // Rejected subtrees stop at the root.
    const auto shadow = visible(32.25, 32.25, 32.75, 32.75);
    require(shadow.size() == 1 && shadow.count(NodeAddress{5, 1, 1}.key()),
            "Pass visibility leaked the main camera's target selection");
    uint32_t visited = 0;
    selection.cull([&](NodeAddress n, bool renderable) {
        ++visited;
        require(n.level == rootAddress(selection).level, "Rejected root still traversed its children");
        require(renderable == (renderableKeys(selection).count(n.key()) != 0), "Incorrect committed renderable classification");
        return false;
    });
    require(visited == 1,
            "Invisible sectors must be pruned without scanning their renderable nodes");
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

void testMaterialPublication() {
    const auto data = cacheData(3);
    TreeFixture tree;
    tree.init(data);
    const auto near = testView({{0, 1, 1, 0, 1}});
    applyView(tree, near);
    require(tree.viewChanged(), "New view must invalidate scheduling");
    applyView(tree, near);
    require(!tree.viewChanged(), "Identical view must preserve settled scheduling");
    completeTarget(tree);
    const auto before = renderableKeys(tree);
    const auto address = NodeAddress::fromKey(*before.begin());
    auto *node = tree.find(address);
    const auto oldMaterial = node->material;
    auto replacement = testMapping(address.ancestor(data.maxLevel));
    require(node->ready && node->material.pages.front().key() == oldMaterial.pages.front().key(), "Prepared mapping changed a drawable node");
    tree.clearChangedNodes();
    require(tree.publish(address, replacement), "Material refresh publication failed");
    require(renderableKeys(tree) == before && tree.changedNodes().size() == 1 && tree.changedNodes().count(address.key()), "Material refresh changed topology or missed notification");
    require(node->material.pages.front().key() == replacement.pages.front().key(), "New mapping was not published");
    ccstd::unordered_set<uint64_t> refresh;
    for (const auto &request : tree.requests()) {
        if (tree.find(request)->ready) {
            require(refresh.insert(request.key()).second, "Duplicate material refresh request");
        }
    }
    require(refresh == before, "Every displayed node must support material refresh");
    applyView(tree, testView({{0, 7, 7, 0, 1}}));
    require(tree.viewChanged(), "Changed pass must invalidate scheduling");
    selectAndCheckChanges(tree);
    checkCut(tree, data);
    std::cout << "PASS: atomic material publication, retained mappings, stable views and refresh requests\n";
}

void testDistanceAndPasses() {
    const auto data = cacheData(3, 3, 3);
    TreeFixture tree;
    tree.init(data);
    cc::geometry::Frustum hidden;
    for (auto *plane : hidden.planes) {
        plane->n.set(1, 0, 0);
        plane->d = 100000.0F;
    }
    const cc::Vec3 camera{0.5F, 0.5F, 0.5F};
    tree.beginUpdate(camera, cc::Vec3::ZERO);
    tree.addPass(hidden, false);
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    auto requests = tree.requests();
    const auto hiddenRequests = requests;
    const auto target = targetKeys(tree);
    require(std::any_of(requests.begin(), requests.end(), [&](const auto &c) { return !tree.find(c)->ready; }), "Hidden terrain lost distance-required subdivision");
    tree.beginUpdate(camera, cc::Vec3::ZERO);
    tree.addPass(hidden, true);
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    requests = tree.requests();
    require(requests.size() == hiddenRequests.size() && std::equal(requests.begin(), requests.end(), hiddenRequests.begin(), [](NodeAddress a, NodeAddress b) { return a.key() == b.key(); }), "Main pass changed LOD request order");
    require(targetKeys(tree) == target, "Main pass changed target topology");
    cc::geometry::Frustum visible;
    for (auto *plane : visible.planes) {
        plane->n.set(1, 0, 0);
        plane->d = -100000.0F;
    }
    tree.beginUpdate(camera, cc::Vec3::ZERO);
    tree.addPass(visible, false);
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    require(tree.viewChanged(), "Pass change without translation must invalidate scheduling");
    require(targetKeys(tree) == target, "Shadow visibility changed target topology");
    requests = tree.requests();
    require(requests.size() == hiddenRequests.size() && std::equal(requests.begin(), requests.end(), hiddenRequests.begin(), [](NodeAddress a, NodeAddress b) { return a.key() == b.key(); }), "Shadow visibility changed LOD request order");
    tree.beginUpdate(camera, cc::Vec3::ZERO);
    tree.addPass(visible, false);
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    require(!tree.viewChanged(), "Stable pass repeated scheduling");
    // Compare requested children to an independent distance-to-child oracle.
    for (const float height : {0.5F, 2.0F, 5.0F, 10.0F, 100.0F}) {
        tree.beginUpdate({12, height, 12}, cc::Vec3::ZERO);
        tree.addPass(visible, false);
        tree.updateTarget();
        require(tree.selectRenderables(), "Select draws after updating the distance target");
        ccstd::unordered_set<uint64_t> splits;
        for (const auto &candidate : tree.requests()) {
            if (!tree.find(candidate)->ready) {
                splits.insert(candidate.ancestor(data.maxLevel).key());
            }
        }
        tree.forEachRenderable([&](const Quadtree::Node &renderable) {
            const auto node = renderable.geometry.address();
            bool refine = false;
            for (uint32_t q = 0; q < 4; ++q) {
                const NodeAddress child{node.level - 1, node.x * 2 + (q & 1), node.z * 2 + (q >> 1)};
                const auto area = tree.region(child);
                const float dx = std::max({area.x - 12, 12 - area.x - area.size, 0.0F});
                const float dz = std::max({area.z - 12, 12 - area.z - area.size, 0.0F});
                const float dy = std::max(height - 1, 0.0F);
                refine |= std::sqrt(dx * dx + dy * dy + dz * dz) <= tree.lodMorphRanges()[child.level].end;
            }
            require(!refine || splits.count(node.key()) != 0, "Target omitted distance-required subdivision");
            require((splits.count(node.key()) != 0) == static_cast<bool>(renderable.children), "Candidates differ from the balanced target");
        });
    }
    // A translation of the entire terrain and pass must preserve local requests.
    tree.beginUpdate(camera, cc::Vec3::ZERO);
    tree.addPass(visible, false);
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    const auto before = tree.requests();
    const cc::Vec3 offset{137, 29, -81};
    for (auto *plane : visible.planes) {
        plane->d += offset.x;
    }
    tree.beginUpdate(camera + offset, offset);
    tree.addPass(visible, false);
    tree.updateTarget();
    require(tree.selectRenderables(), "Select draws after updating the distance target");
    const auto after = tree.requests();
    require(before.size() == after.size(), "World translation changed local subdivision");
    for (size_t i = 0; i < before.size(); ++i) {
        require(before[i].key() == after[i].key(), "Translated candidate ordering changed");
    }
    std::cout << "PASS: distance oracle, pass-independent targets and requests, stable views and world translation\n";
}

void testCameraRotation() {
    const auto data = cacheData(3, 3, 3);
    TreeFixture tree;
    tree.init(data);
    const cc::Vec3 camera{7.5F, 0.5F, 7.5F};
    const auto update = [&](uint32_t direction, bool shadow) {
        cc::geometry::Frustum frustum;
        for (auto *plane : frustum.planes) {
            plane->n.set(0, 1, 0);
            plane->d = -1000;
        }
        // Rotate a 90-degree horizontal view around the same camera position.
        const float angle = static_cast<float>(direction) * 0.785398163F;
        for (uint32_t side = 0; side < 2; ++side) {
            const float normal = angle + (side == 0 ? -0.785398163F : 0.785398163F);
            auto &plane = *frustum.planes[side];
            plane.n.set(std::cos(normal), 0, std::sin(normal));
            plane.d = camera.x * plane.n.x + camera.z * plane.n.z;
        }
        tree.beginUpdate(camera, cc::Vec3::ZERO);
        tree.addPass(frustum, true);
        if (shadow) {
            tree.addPass(frustum, false);
        }
        tree.updateTarget();
        require(tree.selectRenderables(), "Select draws after updating the distance target");
    };
    update(0, false);
    // Repeat with no fine data, incomplete siblings, and fully loaded detail.
    for (uint32_t phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            auto &sector = *tree.find({data.maxLevel, 0, 0});
            for (uint32_t q = 0; q < 3; ++q) {
                const auto address = sector.children[q].geometry.address();
                require(tree.publish(address, testMapping(address)), "Rotation setup could not publish partial siblings");
            }
            selectAndCheckChanges(tree);
        } else if (phase == 2) {
            completeTarget(tree);
        }
        const auto target = targetKeys(tree);
        const auto draws = renderableKeys(tree);
        ccstd::unordered_map<uint64_t, const Quadtree::Node *> nodes;
        ccstd::unordered_set<uint64_t> ready;
        tree.forEachNode([&](const Quadtree::Node &node) {
            const auto key = node.geometry.address().key();
            nodes.emplace(key, &node);
            if (node.ready) {
                ready.insert(key);
            }
            return true;
        });
        tree.clearChangedNodes();
        for (uint32_t direction = 0; direction <= 8; ++direction) {
            update(direction, direction % 2 != 0);
            require(targetKeys(tree) == target, "Camera rotation or shadow pass changed the distance target");
            require(renderableKeys(tree) == draws, "Camera rotation coarsened ready terrain");
            require(tree.changedNodes().empty(), "Camera rotation changed draw materials or morph edges");
            for (const auto &entry : nodes) {
                const auto *node = tree.find(NodeAddress::fromKey(entry.first));
                require(node == entry.second, "Camera rotation retired an existing node or pending request");
                require(node->ready == (ready.count(entry.first) != 0), "Camera rotation changed resource readiness");
                if (node->ready) {
                    require(node->material.pages.size() == 1 && node->material.pages.front().key() == entry.first, "Camera rotation replaced a ready mapping");
                }
            }
            checkCut(tree, data);
            checkStitchedEdges(tree, data);
        }
    }
    std::cout << "PASS: full camera rotation, shadow changes, pending siblings and ready detail retention\n";
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
                            const NodeAddress address{level, sx * side + x, sz * side + z};
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
    testStreamingSelection();
    testSingleRoot();
    testReadySelection();
    testUpdateStages();
    testCommittedVisibility();
    testStitchedPageBounds();
    testMaterialPublication();
    testDistanceAndPasses();
    testCameraRotation();
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

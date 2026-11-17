// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Asset.h"
#include "landscape/Quadtree.h"
#include "landscape/Residency.h"
#include "landscape/VTRenderer.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/assets/RenderTexture.h"
#include "core/assets/Texture2D.h"
#include "renderer/gfx-base/GFXCommandBuffer.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDescriptorSetLayout.h"
#include "renderer/gfx-base/GFXPipelineLayout.h"
#include "renderer/gfx-base/GFXShader.h"
#include "renderer/gfx-base/GFXRenderPass.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXInputAssembler.h"
#include "renderer/gfx-base/GFXPipelineState.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "base/Log.h"
#include "base/StringUtil.h"
#include <cstdarg>
#include <cstdio>
#include "landscape/Renderer.h"
#include "landscape/DecalRenderer.h"
#include "landscape/MaterialLibrary.h"
#include "core/scene-graph/Node.h"
#include "scene/Model.h"
#include <optional>
// Inspect committed state without exposing mutable Quadtree access in the runtime API.
#define private public
#include "landscape/Schedule.h"
#undef private
#include <algorithm>
#include <cmath>
#include "core/geometry/Frustum.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace cc::landscape;

namespace {
// Deterministic IO/GPU boundary doubles. Schedule, Quadtree and VT layout are
// real; allocation/eviction are independently exercised in paging_test.cpp.
struct Backend {
    uint32_t sourceCapacity{0};
    uint32_t pageCapacity{0};
    uint32_t uploads{0};
    uint32_t compositions{0};
    uint32_t updates{0};
    bool preparing{false};
    LandscapeResidency *residency{nullptr};
    ccstd::vector<NodeAddress> working;
    ccstd::vector<VTPageAddress> pages;
    std::map<uint64_t, bool> loaded;
    std::map<uint64_t, bool> composed;
    std::map<uint64_t, uint8_t> draws;
} backend;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string addressText(NodeAddress address) {
    return std::to_string(address.level) + ":" + std::to_string(address.x) + ":" + std::to_string(address.z);
}

std::string targetState(const std::string &state) {
    const auto begin = state.find(" nodes=") + 7;
    std::istringstream nodes(state.substr(begin, state.find(" sources=") - begin));
    std::string node;
    std::string result;
    while (std::getline(nodes, node, ',')) {
        for (uint32_t i = 0; i < 3; ++i) {
            node.resize(node.rfind(':'));
        }
        result += node + ',';
    }
    return result;
}
} // namespace

namespace cc {
LogLevel Log::slogLevel = LogLevel::INFO;
void Log::logMessage(LogType, LogLevel level, const char *, ...) {
    require(level >= LogLevel::INFO, "Unexpected terrain warning or error");
}
ccstd::string StringUtil::format(const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    const auto length = std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    require(length >= 0 && static_cast<size_t>(length) < sizeof(text), "Diagnostic formatting failed");
    return text;
}
} // namespace cc

// This CPU backend never creates JS-backed instance buffers.
namespace se {
void RefCounter::decRef() {
    std::abort();
}
void Object::unroot() {
    std::abort();
}
} // namespace se

namespace cc::landscape {
LandscapeRenderer::LandscapeRenderer()
: _residency(std::make_unique<LandscapeResidency>())
, _vtRenderer(std::make_unique<VTRenderer>()) {}
LandscapeRenderer::~LandscapeRenderer() = default;
DecalRenderer::~DecalRenderer() = default;
MaterialLibrary::~MaterialLibrary() = default;
void LandscapeRenderer::setLodMorphRanges(const ccstd::vector<Vec4> &) {}
void LandscapeRenderer::setViewPosition(const Vec3 &) {}
void LandscapeRenderer::prepareRenderData(uint32_t, gfx::Texture *, const ccstd::vector<LandscapeRenderNode> &updated, const ccstd::vector<uint64_t> &removed) {
    require(backend.preparing, "Tick synchronized draw models");
    for (const auto key : removed) {
        backend.draws.erase(key);
    }
    for (const auto &node : updated) {
        require(_residency->resolve(node.geometry->address()).layer >= 0, "Renderer received missing geometry");
        for (const auto page : node.material->pages) {
            require(!_vtRenderer->needsComposition(page), "Renderer received incomplete material");
        }
        backend.draws[node.geometry->address().key()] = node.edgeMask;
    }
}
bool LandscapeRenderer::collectNodeModels(const QuadNode &, const LandscapeRegion &, bool, const geometry::Frustum &, bool, ccstd::vector<const scene::Model *> &) const {
    return true;
}
LandscapeAsset::LandscapeAsset() = default;
LandscapeAsset::~LandscapeAsset() = default;
LandscapeResidency::LandscapeResidency()
: _capacity(backend.sourceCapacity) {
    backend.residency = this;
}
LandscapeResidency::~LandscapeResidency() = default;
bool LandscapeResidency::canFit(const ccstd::vector<NodeAddress> &sources) const {
    return sources.size() <= backend.sourceCapacity;
}
void LandscapeResidency::request(NodeAddress source) {
    const auto [entry, inserted] = _requests.try_emplace(source.key(), Request{source, 0});
    ++entry->second.count;
}
void LandscapeResidency::release(NodeAddress source) {
    const auto entry = _requests.find(source.key());
    require(entry != _requests.end(), "Source demand released twice");
    if (--entry->second.count == 0) {
        _requests.erase(entry);
    }
}
bool LandscapeResidency::canRequest(const ccstd::vector<NodeAddress> &sources) const {
    ccstd::unordered_set<uint64_t> keys;
    for (const auto &entry : _requests) {
        keys.insert(entry.first);
    }
    for (const auto source : sources) {
        keys.insert(source.key());
    }
    return keys.size() <= backend.sourceCapacity;
}
bool LandscapeResidency::update(uint32_t budget) {
    backend.working.clear();
    std::map<uint64_t, bool> next;
    for (const auto &entry : _requests) {
        backend.working.push_back(entry.second.address);
        next[entry.first] = backend.loaded[entry.first];
    }
    std::sort(backend.working.begin(), backend.working.end(), [](NodeAddress a, NodeAddress b) {
        return a.key() < b.key();
    });
    require(canFit(backend.working), "Scheduler exceeded source capacity");
    backend.loaded = std::move(next);
    require(backend.preparing, "Tick uploaded GPU resources");
    ++backend.updates;
    budget = std::min(budget, backend.uploads);
    for (const auto address : backend.working) {
        if (budget && !backend.loaded[address.key()]) {
            backend.loaded[address.key()] = true;
            --budget;
        }
    }
    return true;
}
bool LandscapeResidency::ready() const {
    return std::all_of(_requests.begin(), _requests.end(), [](const auto &entry) {
        const auto found = backend.loaded.find(entry.first);
        return found != backend.loaded.end() && found->second;
    });
}
LandscapeResidency::Tile LandscapeResidency::resolve(NodeAddress node) const {
    const auto found = backend.loaded.find(node.key());
    return {node, found != backend.loaded.end() && found->second ? 0 : -1};
}
VTPageTable::VTPageTable() = default;
VTPageTable::~VTPageTable() = default;
bool VTPageTable::canFit(uint32_t) const {
    require(!backend.preparing, "Rendering planned page table capacity");
    return true;
}
bool VTPageTable::ensureCapacity(uint32_t) {
    require(backend.preparing, "Tick allocated GPU resources");
    return true;
}
VTPageComposer::~VTPageComposer() = default;
VTPageMipGenerator::~VTPageMipGenerator() = default;
VTPageCompressor::~VTPageCompressor() = default;
VTRenderer::VTRenderer()
: _residency(backend.residency) {}
VTRenderer::~VTRenderer() = default;
VTCapacityQuery::VTCapacityQuery(const VTRenderer &renderer, const LandscapeResidency &residency)
: _renderer(renderer)
, _residency(residency) {}
bool VTCapacityQuery::canPrepare(const ccstd::vector<VTPageAddress> &pages, const ccstd::vector<NodeAddress> &sources) {
    if (!_residency.canFit(sources) || !_renderer.canFit(pages)) {
        return false;
    }
    // Match the boundary double's single shared composition input.
    const NodeAddress input{2, 0, 0};
    const bool retained = std::any_of(sources.begin(), sources.end(), [&](NodeAddress source) {
        return source.key() == input.key();
    });
    return pages.empty() || sources.size() + (retained ? 0 : 1) <= _residency.layerCount();
}
void VTRenderer::setTriplanarEnabled(bool enabled) {
    _triplanarEnabled = enabled;
}
bool VTRenderer::canFit(const ccstd::vector<VTPageAddress> &pages) const {
    return pages.size() <= backend.pageCapacity;
}
void VTRenderer::request(VTPageAddress address) {
    const auto [entry, inserted] = _requests.try_emplace(address.key(), Request{address, 0});
    ++entry->second.count;
    _requestsDirty |= inserted;
}
void VTRenderer::release(VTPageAddress address) {
    const auto entry = _requests.find(address.key());
    require(entry != _requests.end(), "Page demand released twice");
    if (--entry->second.count == 0) {
        _requests.erase(entry);
        _requestsDirty = true;
    }
}
bool VTRenderer::update(uint32_t budget) {
    require(backend.preparing, "Tick processed page tasks");
    backend.pages.clear();
    std::map<uint64_t, bool> next;
    for (const auto &entry : _requests) {
        backend.pages.push_back(entry.second.address);
        next[entry.first] = backend.composed[entry.first];
    }
    std::sort(backend.pages.begin(), backend.pages.end(), [](VTPageAddress a, VTPageAddress b) {
        return a.key() < b.key();
    });
    require(canFit(backend.pages), "Scheduler exceeded page capacity");
    backend.composed = std::move(next);
    _requestsDirty = false;
    auto &residency = *backend.residency;
    for (const auto source : _sourceRequests) {
        residency.release(source);
    }
    _sourceRequests.clear();
    const NodeAddress input{2, 0, 0};
    if (!ready() && !backend.pages.empty() && residency.canRequest({input})) {
        residency.request(input);
        _sourceRequests.push_back(input);
        renderPages(residency, backend.pages, budget);
    }
    return true;
}
bool VTRenderer::ready() const {
    return !_requestsDirty && !backend.pages.empty() && std::all_of(backend.composed.begin(), backend.composed.end(), [](const auto &entry) {
        return entry.second;
    });
}
bool VTRenderer::needsComposition(VTPageAddress page) const {
    const auto found = backend.composed.find(page.key());
    return found == backend.composed.end() || !found->second;
}
VTPageSources VTRenderer::selectSources(VTPageAddress, bool, bool) const {
    // A shared synthetic source deliberately exercises temporary input retention.
    VTPageSources result;
    result.splat = {2, 0, 0};
    result.normals.fill(result.splat);
    return result;
}
uint32_t VTRenderer::renderPages(LandscapeResidency &residency, const ccstd::vector<VTPageAddress> &pages, uint32_t budget) {
    require(backend.preparing, "Tick recorded VT composition");
    if (residency.resolve({2, 0, 0}).layer < 0) {
        return 0;
    }
    budget = std::min(budget, backend.compositions);
    uint32_t count = 0;
    for (const auto page : pages) {
        if (count < budget && needsComposition(page)) {
            backend.composed[page.key()] = true;
            ++count;
        }
    }
    return count;
}
} // namespace cc::landscape

// Read the same literal cases/baseline used by Jest; no second copy can drift.
std::string readParityBlock(const char *name) {
    std::ifstream source(std::string(LANDSCAPE_PARITY_TESTS) + "/schedule.test.ts");
    require(source.good(), "Missing TypeScript parity test");
    const std::string begin = std::string("const ") + name + " = String.raw`";
    std::string line;
    std::string result;
    bool reading = false;
    while (std::getline(source, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!reading) {
            reading = line == begin;
        } else if (line == "`;") {
            return result;
        } else {
            result += line + '\n';
        }
    }
    require(false, "Missing or unterminated TypeScript parity block");
    return {};
}

void testCameraRotation() {
    LandscapeAsset asset;
    auto &data = const_cast<LandscapeData &>(asset.data());
    data.maxTileLevel = 3;
    data.sectorSize = 8;
    data.sectorsX = 3;
    data.sectorsZ = 3;
    data.heightScale = 1;
    auto &ranges = const_cast<ccstd::vector<ccstd::vector<HeightRange>> &>(asset.heightRanges());
    ranges.resize(data.maxTileLevel + 1);
    for (uint32_t level = 0; level <= data.maxTileLevel; ++level) {
        ranges[level].assign(static_cast<size_t>(data.nodesX(level)) * data.nodesZ(level), {0, 1});
    }
    LandscapeRenderer renderer;
    LandscapeSchedule schedule(asset, renderer, 1.0F);
    auto &tree = *schedule._quadtree;
    const auto mapping = [](NodeAddress address) {
        VTGridMapping result;
        result.pages = {{address.level, address.x, address.z}};
        return result;
    };
    for (uint32_t z = 0; z < data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < data.sectorsX; ++x) {
            const NodeAddress address{data.maxTileLevel, x, z};
            require(tree.publish(address, mapping(address)), "Initialize synthetic sector resources");
        }
    }
    const auto targetKeys = [](const Quadtree &value) {
        ccstd::unordered_set<uint64_t> result;
        value.forEachNode([&](const Quadtree::Node &node) {
            if (!node.refined) {
                result.insert(node.geometry.address().key());
            }
            return node.refined;
        });
        return result;
    };
    const auto renderState = [&]() {
        std::map<uint64_t, uint8_t> result;
        tree.forEachRenderable([&](const Quadtree::Node &node) {
            result.emplace(node.geometry.address().key(), node.edgeMask);
        });
        return result;
    };
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
        tree.updateTarget(camera, cc::Vec3::ZERO);
        require(tree.selectRenderables(), "Select draws after updating the distance target");
        const auto requests = tree.requests();
        const bool selectionChanged = tree.renderSelectionChanged();
        const auto changed = tree.changedNodes();
        schedule.beginPasses();
        schedule.addPass(frustum, true);
        if (shadow) {
            schedule.addPass(frustum, false);
        }
        schedule.endPasses();
        const auto after = tree.requests();
        require(requests.size() == after.size() && std::equal(requests.begin(), requests.end(), after.begin(), [](NodeAddress a, NodeAddress b) { return a.key() == b.key(); }), "Pass collection changed request order");
        require(tree.renderSelectionChanged() == selectionChanged && tree.changedNodes() == changed, "Pass collection changed committed draws or morph edges");
    };
    update(0, false);
    // Repeat with no fine data, incomplete siblings, and fully loaded detail.
    for (uint32_t phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            auto &sector = *tree.find({data.maxTileLevel, 0, 0});
            for (uint32_t q = 0; q < 3; ++q) {
                const auto address = sector.children[q].geometry.address();
                require(tree.publish(address, mapping(address)), "Rotation setup could not publish partial siblings");
            }
            require(tree.selectRenderables(), "Select partially loaded draws");
        } else if (phase == 2) {
            bool complete = false;
            for (uint32_t step = 0; step < 4096 && !complete; ++step) {
                complete = true;
                for (const auto address : tree.requests()) {
                    if (!tree.find(address)->ready()) {
                        require(tree.publish(address, mapping(address)), "Publish ready target nodes");
                        complete = false;
                    }
                }
                require(tree.selectRenderables(), "Select loaded draws");
            }
            require(complete, "Target loading did not converge");
        }
        const auto target = targetKeys(tree);
        const auto draws = renderState();
        ccstd::unordered_map<uint64_t, const Quadtree::Node *> nodes;
        ccstd::unordered_set<uint64_t> ready;
        tree.forEachNode([&](const Quadtree::Node &node) {
            const auto key = node.geometry.address().key();
            nodes.emplace(key, &node);
            if (node.ready()) {
                ready.insert(key);
            }
            return true;
        });
        tree.clearChangedNodes();
        for (uint32_t direction = 0; direction <= 8; ++direction) {
            update(direction, direction % 2 != 0);
            require(targetKeys(tree) == target, "Camera rotation or shadow pass changed the distance target");
            require(renderState() == draws, "Camera rotation coarsened ready terrain");
            require(tree.changedNodes().empty(), "Camera rotation changed draw materials or morph edges");
            for (const auto &entry : nodes) {
                const auto *node = tree.find(NodeAddress::fromKey(entry.first));
                require(node == entry.second, "Camera rotation retired an existing node or pending request");
                require(node->ready() == (ready.count(entry.first) != 0), "Camera rotation changed resource readiness");
                if (node->ready()) {
                    require(node->material.pages.size() == 1 && node->material.pages.front().key() == entry.first, "Camera rotation replaced a ready mapping");
                }
            }
        }
    }
    std::cout << "PASS: full camera rotation, shadow changes, pending siblings and ready detail retention\n";
}

int main(int argc, char **argv) {
    std::vector<std::string> expectedStates;
    std::istringstream baseline(readParityBlock("PARITY_EXPECTED"));
    std::string expected;
    while (std::getline(baseline, expected)) {
        if (expected.find("state ") == 0) {
            expectedStates.push_back(expected);
        }
    }
    std::istringstream input(readParityBlock("PARITY_CASES"));
    std::string line;
    while (std::getline(input, line) && (line.empty() || line[0] == '#')) {}
    std::istringstream capacities(line);
    std::string command;
    capacities >> command >> backend.sourceCapacity >> backend.pageCapacity;
    require(command == "capacity", "Expected backend capacities");
    LandscapeAsset asset;
    auto &data = const_cast<LandscapeData &>(asset.data());
    data.sectorsX = 2;
    data.sectorsZ = 1;
    data.maxTileLevel = 2;
    data.sectorSize = 4;
    data.heightScale = 1;
    auto &ranges = const_cast<ccstd::vector<ccstd::vector<HeightRange>> &>(asset.heightRanges());
    ranges.resize(data.maxTileLevel + 1U);
    for (uint32_t level = 0; level <= data.maxTileLevel; ++level) {
        ranges[level].assign(static_cast<size_t>(data.nodesX(level)) * data.nodesZ(level), {0, 1});
    }
    LandscapeRenderer renderer;
    auto &residency = renderer.residency();
    auto &pages = renderer.vtRenderer();
    const bool triplanar = argc == 2 && std::string(argv[1]) == "--triplanar";
    pages.setTriplanarEnabled(triplanar);
    LandscapeSchedule schedule(asset, renderer, 1.0F);
    const auto &tree = *schedule._quadtree;
    std::ostringstream trace;
    trace << line << '\n';
    uint32_t frame = 0;
    bool hadCoverage = false;
    std::string settledState;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        trace << line << '\n';
        std::istringstream row(line);
        row >> command;
        if (command == "invalidate") {
            for (auto &entry : backend.composed) {
                entry.second = false;
            }
            continue;
        }
        require(command == "frame", "Unknown scheduling command");
        cc::Vec3 camera;
        bool repeat;
        row >> camera.x >> camera.y >> camera.z >> backend.uploads >> backend.compositions >> repeat;
        std::map<uint64_t, uint8_t> committed;
        tree.forEachRenderable([&](const Quadtree::Node &node) {
            committed.emplace(node.geometry.address().key(), node.edgeMask);
        });
        schedule.update(camera, cc::Vec3::ZERO);
        std::map<uint64_t, uint8_t> afterTick;
        tree.forEachRenderable([&](const Quadtree::Node &node) {
            afterTick.emplace(node.geometry.address().key(), node.edgeMask);
        });
        require(afterTick == committed, "Tick changed committed draws or morph edges");
        backend.preparing = true;
        schedule.prepareRenderData(++frame);
        if (repeat) {
            const auto updates = backend.updates;
            schedule.prepareRenderData(frame);
            require(updates == backend.updates, "Duplicate frame performed resource work");
        }
        schedule.beginPasses();
        schedule.endPasses();
        const bool coverage = tree.sectorRootsReady();
        std::map<uint64_t, uint8_t> selected;
        tree.forEachRenderable([&](const Quadtree::Node &node) {
            selected.emplace(node.geometry.address().key(), node.edgeMask);
        });
        require(backend.draws == selected, "Renderer draws or morph edges differ from committed state");
        backend.preparing = false;
        require(!hadCoverage || coverage, "Pending resources removed committed coverage");
        hadCoverage |= coverage;
        std::vector<std::string> nodes;
        tree.forEachNode([&](const Quadtree::Node &node) {
            if (node.geometry.level <= data.maxTileLevel) {
                nodes.push_back(addressText(node.geometry.address()) + ":" + std::to_string(residency.resolve(node.geometry.address()).layer >= 0) + ":" + std::to_string(node.ready()) + ":" + std::to_string(node.renderable));
            }
            if (node.ready()) {
                require(residency.resolve(node.geometry.address()).layer >= 0, "Published geometry lost its source");
            }
            return true;
        });
        std::sort(nodes.begin(), nodes.end());
        std::ostringstream state;
        state << "state " << coverage << ' ' << schedule.isRenderStateStable() << " nodes=";
        for (const auto &node : nodes) {
            state << node << ',';
        }
        state << " sources=";
        for (const auto address : backend.working) {
            state << addressText(address) << ':' << backend.loaded[address.key()] << ',';
        }
        state << " pages=";
        for (const auto page : backend.pages) {
            state << addressText({page.level, page.x, page.z}) << ':' << backend.composed[page.key()] << ',';
        }
        settledState = state.str() + '\n';
        require(targetState(settledState) == targetState(expectedStates.at(frame - 1)), "Tick/render split changed target topology");
        trace << settledState;
    }
    require(hadCoverage && schedule.isRenderStateStable(), "Scenario never reached complete stable coverage");
    if (argc == 2 && std::string(argv[1]) == "--dump-parity") {
        std::cout << trace.str();
        return 0;
    }
    // Triplanar changes composition inputs, not the distance-based page allocation.
    require(settledState == expectedStates.back() + '\n', "Native settled scheduling differs from the shared TS/C++ baseline; inspect --dump-parity");
    testCameraRotation();
    std::cout << "PASS: shared scheduling, delayed resources, cancellation, material refresh and repeated frames\n";
}

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/Asset.h"
#include "landscape/Quadtree.h"
#include "landscape/Schedule.h"
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
#include <algorithm>
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
    ccstd::vector<NodeAddress> working;
    ccstd::vector<VTPageAddress> pages;
    std::map<uint64_t, bool> loaded;
    std::map<uint64_t, bool> composed;
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
} // namespace

namespace cc {
LogLevel Log::slogLevel = LogLevel::INFO;
void Log::logMessage(LogType, LogLevel, const char *, ...) {
    std::abort();
}
} // namespace cc

namespace cc::landscape {
LandscapeAsset::LandscapeAsset() = default;
LandscapeAsset::~LandscapeAsset() = default;
LandscapeResidency::LandscapeResidency() : _capacity(backend.sourceCapacity) {}
LandscapeResidency::~LandscapeResidency() = default;
bool LandscapeResidency::canReserve(const ccstd::vector<NodeAddress> &sources) const {
    return sources.size() <= backend.sourceCapacity;
}
bool LandscapeResidency::reserve(const ccstd::vector<NodeAddress> &sources) {
    require(canReserve(sources), "Scheduler exceeded source capacity");
    backend.working = sources;
    std::map<uint64_t, bool> next;
    for (const auto address : sources) {
        next[address.key()] = backend.loaded[address.key()];
    }
    backend.loaded = std::move(next);
    return true;
}
bool LandscapeResidency::loadReserved() {
    return true;
}
void LandscapeResidency::update(uint32_t budget) {
    ++backend.updates;
    budget = std::min(budget, backend.uploads);
    for (const auto address : backend.working) {
        if (budget && !backend.loaded[address.key()]) {
            backend.loaded[address.key()] = true;
            --budget;
        }
    }
}
bool LandscapeResidency::ready() const {
    return std::all_of(backend.loaded.begin(), backend.loaded.end(), [](const auto &entry) {
        return entry.second;
    });
}
LandscapeResidency::Tile LandscapeResidency::resolve(NodeAddress node) const {
    const auto found = backend.loaded.find(node.key());
    return {node, found != backend.loaded.end() && found->second ? 0 : -1};
}
VTPageTable::VTPageTable() = default;
VTPageTable::~VTPageTable() = default;
bool VTPageTable::ensureCapacity(uint32_t) {
    return true;
}
VTPageComposer::~VTPageComposer() = default;
VTPageMipGenerator::~VTPageMipGenerator() = default;
VTPageCompressor::~VTPageCompressor() = default;
VTRenderer::VTRenderer() = default;
VTRenderer::~VTRenderer() = default;
bool VTRenderer::canReserve(const ccstd::vector<VTPageAddress> &pages) const {
    return pages.size() <= backend.pageCapacity;
}
bool VTRenderer::reserve(const ccstd::vector<VTPageAddress> &pages) {
    require(canReserve(pages), "Scheduler exceeded page capacity");
    backend.pages = pages;
    std::map<uint64_t, bool> next;
    for (const auto page : pages) {
        next[page.key()] = backend.composed[page.key()];
    }
    backend.composed = std::move(next);
    return true;
}
bool VTRenderer::ready() const {
    return !backend.pages.empty() && std::all_of(backend.composed.begin(), backend.composed.end(), [](const auto &entry) {
        return entry.second;
    });
}
bool VTRenderer::needsComposition(VTPageAddress page) const {
    const auto found = backend.composed.find(page.key());
    return found == backend.composed.end() || !found->second;
}
VTPageSources VTRenderer::selectSources(VTPageAddress, bool) const {
    // A shared synthetic source deliberately exercises temporary input retention.
    VTPageSources result;
    result.splat = {2, 0, 0};
    result.normals.fill(result.splat);
    return result;
}
uint32_t VTRenderer::renderPages(LandscapeResidency &residency, const ccstd::vector<VTPageAddress> &pages, uint32_t budget) {
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

int main(int argc, char **argv) {
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
    data.maxLevel = 2;
    data.sectorSize = 4;
    data.heightScale = 1;
    const_cast<ccstd::vector<HeightRange> &>(asset.heightRanges()).assign(NodeIndexLayout(data).nodeCount(), {0, 1});
    Quadtree tree;
    tree.init(asset);
    LandscapeResidency residency;
    VTRenderer pages;
    LandscapeSchedule schedule(asset, residency, pages, tree);
    std::ostringstream trace;
    trace << line << '\n';
    uint32_t frame = 0;
    bool hadCoverage = false;
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
        tree.clearChangedNodes();
        tree.beginUpdate(camera, cc::Vec3::ZERO);
        tree.updateTarget();
        schedule.prepareResources(++frame);
        if (repeat) {
            const auto updates = backend.updates;
            schedule.prepareResources(frame);
            require(updates == backend.updates, "Duplicate frame performed resource work");
        }
        const bool coverage = tree.selectRenderables();
        require(!hadCoverage || coverage, "Pending resources removed committed coverage");
        hadCoverage |= coverage;
        std::vector<std::string> nodes;
        tree.forEachNode([&](const Quadtree::Node &node) {
            if (node.geometry.level <= data.maxLevel) {
                nodes.push_back(addressText(node.geometry.address()) + ":" + std::to_string(node.loaded) + ":" + std::to_string(node.ready) + ":" + std::to_string(node.renderable));
            }
            if (node.ready) {
                require(residency.resolve(node.geometry.address()).layer >= 0, "Published geometry lost its source");
            }
            return true;
        });
        std::sort(nodes.begin(), nodes.end());
        trace << "state " << coverage << ' ' << schedule.isSettled() << " nodes=";
        for (const auto &node : nodes) {
            trace << node << ',';
        }
        trace << " sources=";
        for (const auto address : backend.working) {
            trace << addressText(address) << ':' << backend.loaded[address.key()] << ',';
        }
        trace << " pages=";
        for (const auto page : backend.pages) {
            trace << addressText({page.level, page.x, page.z}) << ':' << backend.composed[page.key()] << ',';
        }
        trace << '\n';
    }
    require(hadCoverage && schedule.isSettled(), "Scenario never reached complete stable coverage");
    if (argc == 2 && std::string(argv[1]) == "--dump-parity") {
        std::cout << trace.str();
        return 0;
    }
    const auto baseline = readParityBlock("PARITY_EXPECTED");
    require(trace.str() == baseline, "Native scheduling differs from the shared TS/C++ baseline; inspect --dump-parity");
    std::cout << "PASS: shared scheduling, delayed resources, cancellation, material refresh and repeated frames\n";
}

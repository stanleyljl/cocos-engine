/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/
#include "landscape/Quadtree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include "landscape/Asset.h"

namespace cc::landscape {
// Main flow: initialize, update the view and target, then publish ready resources and select draws.
void Quadtree::init(const LandscapeAsset &asset, float lodQualityScale) {
    _data = asset.data();
    _heightRanges = asset.heightRanges();
    _root = std::make_unique<Node>();
    _renderNodeKeys.clear();
    _renderSelectionChanged = false;
    _needsRenderSelection = true;
    _changedNodes.clear();
    uint32_t rootLevel = _data.maxTileLevel;
    for (uint32_t side = 1; side < std::max(_data.sectorsX, _data.sectorsZ); side *= 2) {
        ++rootLevel;
    }
    buildRoot(*_root, {rootLevel, 0, 0});
    initLodMorphRanges(lodQualityScale);
}

void Quadtree::initLodMorphRanges(float lodQualityScale) {
    _lodMorphRanges.assign(static_cast<size_t>(_data.maxTileLevel) + 1, MorphRange{});

    float totalWeight = 0.0F;
    float weight = 1.0F;
    for (uint32_t level = 0; level <= _data.maxTileLevel; ++level) {
        totalWeight += weight;
        weight *= config::LOD_DISTANCE_RATIO;
    }
    const float section = std::hypot(_data.width(), _data.depth()) * lodQualityScale / totalWeight;
    float end = 0.0F;
    float start = 0.0F;
    weight = 1.0F;
    for (auto &range : _lodMorphRanges) {
        end += section * weight;
        start += (end - start) * config::MORPH_START_RATIO;
        range = {start, end};
        weight *= config::LOD_DISTANCE_RATIO;
    }
}

void Quadtree::updateTarget(const Vec3 &worldCameraPosition, const Vec3 &worldOrigin) {
    const Vec3 localCameraPosition = worldCameraPosition - worldOrigin;
    _viewChanged = localCameraPosition != _localCameraPosition || worldOrigin != _worldOrigin;
    _localCameraPosition = localCameraPosition;
    _worldOrigin = worldOrigin;
    if (_viewChanged || _needsRenderSelection) {
        updateTarget(*_root);
        _needsRenderSelection = true;
    }
}

void Quadtree::updateTarget(Node &node) {
    const auto address = node.geometry.address();
    if (!contains(address)) {
        return;
    }
    bool refine = address.level > _data.maxTileLevel;
    // Distance alone determines refinement; rotating a view must not discard
    // ready detail or change the target used to balance neighboring draws.
    if (!refine && address.level > 0) {
        const float range = _lodMorphRanges[address.level - 1].end;
        for (uint32_t q = 0; q < 4; ++q) {
            if (distanceSquared(child(address, q)) <= range * range) {
                refine = true;
                break;
            }
        }
    }
    node.refined = refine;
    if (!refine) {
        return;
    }
    if (!node._children) {
        node._children = std::make_unique<Node[]>(4);
        for (uint32_t q = 0; q < 4; ++q) {
            node._children[q].geometry = geometry(child(address, q));
        }
    }
    for (uint32_t q = 0; q < 4; ++q) {
        updateTarget(node._children[q]);
    }
}

ccstd::vector<NodeAddress> Quadtree::requests() const {
    ccstd::vector<NodeAddress> missingNodes;
    ccstd::vector<NodeAddress> refreshNodes;
    forEachNode([&](const Node &node) {
        const auto address = node.geometry.address();
        if (address.level > _data.maxTileLevel) {
            return true;
        }
        // Request the first missing node on each branch before its descendants.
        if (!node.ready) {
            missingNodes.push_back(address);
            return false;
        }
        // Retained ancestors keep their mapping; displayed and target leaves refresh.
        if (node.renderable || !node.refined) {
            refreshNodes.push_back(address);
        }
        return node.refined;
    });
    const auto priority = [](NodeAddress a, NodeAddress b) {
        if (a.level != b.level) {
            return a.level < b.level;
        }
        return a.key() < b.key();
    };
    // Complete missing coverage before refreshing existing mappings.
    std::sort(missingNodes.begin(), missingNodes.end(), priority);
    std::sort(refreshNodes.begin(), refreshNodes.end(), priority);
    missingNodes.insert(missingNodes.end(), refreshNodes.begin(), refreshNodes.end());
    return missingNodes;
}

bool Quadtree::publish(NodeAddress address, VTGridMapping material) {
    if (!isTarget(address)) {
        return false;
    }
    auto *node = findNode(address);
    if (address.level > _data.maxTileLevel) {
        return false;
    }
    if (material.pages.empty()) {
        return false;
    }
    if (address.level < _data.maxTileLevel) {
        const auto *parent = find(address.ancestor(address.level + 1));
        if (!parent->ready) {
            return false;
        }
    }
    _needsRenderSelection |= !node->ready;
    node->material = std::move(material);
    node->ready = true;
    if (node->renderable) {
        _changedNodes.insert(address.key());
    }
    return true;
}

bool Quadtree::selectRenderables() {
    _renderSelectionChanged = false;
    if (!_needsRenderSelection) {
        return true;
    }
    if (!sectorRootsReady()) {
        return false;
    }
    // 1. Start with complete, balanced sector-root coverage. Keep the target intact.
    ccstd::vector<Node *> currentLevelNodes;
    ccstd::vector<Node *> nextLevelNodes;
    visitNodes(*_root, [&](Node &node) {
        node.renderable = node.geometry.level == _data.maxTileLevel;
        if (node.renderable) {
            currentLevelNodes.push_back(&node);
        }
        return true;
    });

    // 2. Select finer render nodes one level at a time. Coarser neighbors have
    // already been decided, so a blocked node needs no retry or balance repair.
    while (!currentLevelNodes.empty()) {
        nextLevelNodes.clear();
        for (auto *node : currentLevelNodes) {
            if (!node->refined) {
                continue;
            }
            const bool childrenReady = std::all_of(node->_children.get(), node->_children.get() + 4, [](const Node &child) {
                return child.ready;
            });
            if (!childrenReady) {
                continue;
            }
            // The low bit of each edge pair marks a coarser neighbor.
            if ((computeEdgeMask(node->geometry.address()) & 0x55U) != 0) {
                continue;
            }
            node->renderable = false;
            for (uint32_t q = 0; q < 4; ++q) {
                auto &child = node->_children[q];
                child.renderable = true;
                nextLevelNodes.push_back(&child);
            }
        }
        currentLevelNodes.swap(nextLevelNodes);
    }

    // 3. Update node-owned morph edges and report changes against the previous render set.
    auto previousRenderKeys = std::move(_renderNodeKeys);
    _renderNodeKeys.clear();
    visitNodes(*_root, [&](Node &node) {
        if (!node.renderable) {
            return true;
        }
        const auto address = node.geometry.address();
        const auto edges = computeEdgeMask(address);
        _renderNodeKeys.insert(address.key());
        const bool wasRenderable = previousRenderKeys.erase(address.key()) != 0;
        if (!wasRenderable || node.edgeMask != edges) {
            _changedNodes.insert(address.key());
            _renderSelectionChanged = true;
        }
        node.edgeMask = edges;
        return false;
    });
    for (const auto removed : previousRenderKeys) {
        _changedNodes.insert(removed);
        _renderSelectionChanged = true;
    }
    visitNodes(*_root, [](Node &node) {
        if (!node.refined) {
            node._children.reset();
        }
        return node.refined;
    });
    _needsRenderSelection = false;
    return true;
}

void Quadtree::forEachNode(const std::function<bool(const Node &)> &visit) const {
    visitNodes(static_cast<const Node &>(*_root), visit);
}

void Quadtree::cull(const std::function<bool(const Node &)> &visit) const {
    if (_renderNodeKeys.empty()) {
        return;
    }
    visitNodes(static_cast<const Node &>(*_root), [&](const Node &node) {
        if (!visit(node)) {
            return false;
        }
        return !node.renderable;
    });
}

void Quadtree::forEachRenderable(const std::function<void(const Node &)> &visit) const {
    if (_renderNodeKeys.empty()) {
        return;
    }
    forEachNode([&](const Node &node) {
        if (node.renderable) {
            visit(node);
        }
        return !node.renderable;
    });
}

void Quadtree::visitNodes(const Node &node, const std::function<bool(const Node &)> &visit) const {
    if (!contains(node.geometry.address())) {
        return;
    }
    if (!visit(node)) {
        return;
    }
    if (!node._children) {
        return;
    }
    for (uint32_t q = 0; q < 4; ++q) {
        visitNodes(static_cast<const Node &>(node._children[q]), visit);
    }
}

void Quadtree::visitNodes(Node &node, const std::function<bool(Node &)> &visit) const {
    if (!contains(node.geometry.address())) {
        return;
    }
    if (!visit(node)) {
        return;
    }
    if (!node._children) {
        return;
    }
    for (uint32_t q = 0; q < 4; ++q) {
        visitNodes(node._children[q], visit);
    }
}

// Helpers: final morph edges.
uint8_t Quadtree::computeEdgeMask(NodeAddress address) const {
    const uint32_t width = _data.sectorsX << _data.maxTileLevel;
    const uint32_t depth = _data.sectorsZ << _data.maxTileLevel;
    // In the balanced cut, a whole edge meets one same/coarser node or two
    // children one level finer. One sample per edge determines its morph mask.
    const uint32_t gridX = address.x << address.level;
    const uint32_t gridZ = address.z << address.level;
    const uint32_t size = 1U << address.level;
    uint8_t edges{0};
    const auto addEdge = [&](NodeAddress neighbor, uint32_t edge) {
        if (neighbor.level > address.level) {
            edges |= static_cast<uint8_t>(1U << (edge * 2));
        } else if (neighbor.level < address.level) {
            edges |= static_cast<uint8_t>(2U << (edge * 2));
        }
    };
    // Left, right, bottom, top; outer terrain boundaries have no neighbor.
    if (gridX > 0) {
        addEdge(renderableAt(gridX - 1, gridZ), 0);
    }
    if (gridX + size < width) {
        addEdge(renderableAt(gridX + size, gridZ), 1);
    }
    if (gridZ > 0) {
        addEdge(renderableAt(gridX, gridZ - 1), 2);
    }
    if (gridZ + size < depth) {
        addEdge(renderableAt(gridX, gridZ + size), 3);
    }
    return edges;
}

// Helpers: node ownership and lookup.
void Quadtree::buildRoot(Node &node, NodeAddress address) {
    node.geometry = {address.level, address.x, address.z, 0.0F, 0.0F};
    if (!contains(address)) {
        return;
    }
    if (address.level == _data.maxTileLevel) {
        node.geometry = geometry(address);
        return;
    }
    // Management nodes aggregate bounds but own no height or material resources.
    node.refined = true;
    node._children = std::make_unique<Node[]>(4);
    HeightRange bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    for (uint32_t q = 0; q < 4; ++q) {
        const auto addressOfChild = child(address, q);
        buildRoot(node._children[q], addressOfChild);
        if (contains(addressOfChild)) {
            bounds.minY = std::min(bounds.minY, node._children[q].geometry.minY);
            bounds.maxY = std::max(bounds.maxY, node._children[q].geometry.maxY);
        }
    }
    node.geometry.minY = bounds.minY;
    node.geometry.maxY = bounds.maxY;
}

NodeAddress Quadtree::child(NodeAddress address, uint32_t quadrant) {
    return {address.level - 1, address.x * 2 + (quadrant & 1U), address.z * 2 + (quadrant >> 1U)};
}

// Helpers: node lookup, bounds and render-neighbor queries.
bool Quadtree::sectorRootsReady() const {
    for (uint32_t z = 0; z < _data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < _data.sectorsX; ++x) {
            if (!find({_data.maxTileLevel, x, z})->ready) {
                return false;
            }
        }
    }
    return true;
}

bool Quadtree::contains(NodeAddress address) const {
    if (address.level <= _data.maxTileLevel) {
        const auto sector = address.ancestor(_data.maxTileLevel);
        return sector.x < _data.sectorsX && sector.z < _data.sectorsZ;
    }
    const auto shift = address.level - _data.maxTileLevel;
    return (static_cast<uint64_t>(address.x) << shift) < _data.sectorsX &&
           (static_cast<uint64_t>(address.z) << shift) < _data.sectorsZ;
}

const Quadtree::Node *Quadtree::find(NodeAddress address) const {
    if (!contains(address)) {
        return nullptr;
    }
    if (address.level > _root->geometry.level) {
        return nullptr;
    }
    const Node *node = _root.get();
    while (node->geometry.level > address.level) {
        if (!node->_children) {
            return nullptr;
        }
        const auto shift = node->geometry.level - 1 - address.level;
        const auto quadrant = ((address.x >> shift) & 1U) | (((address.z >> shift) & 1U) << 1U);
        node = &node->_children[quadrant];
    }
    return node;
}

Quadtree::Node *Quadtree::findNode(NodeAddress address) {
    return const_cast<Node *>(static_cast<const Quadtree &>(*this).find(address));
}

bool Quadtree::isTarget(NodeAddress address) const {
    if (!find(address)) {
        return false;
    }
    while (address.level < _root->geometry.level) {
        address = address.ancestor(address.level + 1);
        if (!find(address)->refined) {
            return false;
        }
    }
    return true;
}

const Quadtree::Node *Quadtree::findRenderable(NodeAddress address) const {
    const auto *node = find(address);
    if (!node) {
        return nullptr;
    }
    if (!node->renderable) {
        return nullptr;
    }
    return node;
}

LandscapeRegion Quadtree::region(NodeAddress address) const {
    const float size = std::ldexp(_data.sectorSize, static_cast<int>(address.level) - static_cast<int>(_data.maxTileLevel));
    return {address.x * size, address.z * size, size};
}

HeightRange Quadtree::heightRange(NodeAddress address) const {
    if (address.level > _data.maxTileLevel) {
        const auto &bounds = find(address)->geometry;
        return {bounds.minY, bounds.maxY};
    }
    return _heightRanges[address.level][static_cast<size_t>(address.z) * _data.nodesX(address.level) + address.x];
}

QuadNode Quadtree::geometry(NodeAddress address) const {
    auto bounds = heightRange(address);
    if (address.level < _data.minTileLevel) {
        const auto source = heightRange(address.ancestor(_data.minTileLevel));
        bounds.minY = std::min(bounds.minY, source.minY);
        bounds.maxY = std::max(bounds.maxY, source.maxY);
    }
    return {address.level, address.x, address.z, bounds.minY, bounds.maxY};
}

NodeAddress Quadtree::renderableAt(uint32_t x, uint32_t z) const {
    const Node *node = _root.get();
    while (!node->renderable) {
        const auto shift = node->geometry.level - 1;
        const auto quadrant = ((x >> shift) & 1U) | (((z >> shift) & 1U) << 1U);
        node = &node->_children[quadrant];
    }
    return node->geometry.address();
}

// Helper: distance to a node's height bounds.
float Quadtree::distanceSquared(NodeAddress address) const {
    const auto bounds = heightRange(address);
    const auto area = region(address);
    const float x = std::max({area.x - _localCameraPosition.x, _localCameraPosition.x - area.x - area.size, 0.0F});
    const float y = std::max({bounds.minY - _localCameraPosition.y, _localCameraPosition.y - bounds.maxY, 0.0F});
    const float z = std::max({area.z - _localCameraPosition.z, _localCameraPosition.z - area.z - area.size, 0.0F});
    return x * x + y * y + z * z;
}

} // namespace cc::landscape

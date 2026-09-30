/****************************************************************************
 Copyright (c) 2024 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

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

#pragma once
#include <array>
#include "base/Ptr.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/unordered_set.h"
#include "base/std/container/vector.h"
#include "landscape/VTPaging.h"
namespace cc {
namespace gfx {
class Device;
class Texture;
class Sampler;
} // namespace gfx
namespace landscape {
class LandscapeAsset;
// Physical slots are reserved BEFORE starting IO. In-flight work is bounded;
// only the coordinator's exact working set may be uploaded. No miss resolution.
class TilePagePool {
public:
    // CPU slot ownership, shared by admission and uploads. A reservation either
    // succeeds in full or leaves the previous bindings untouched.
    struct Reservations {
        struct Slot {
            uint32_t layer;
            bool ready{false};
        };
        LandscapeData data;
        uint32_t capacity{0};
        ccstd::unordered_map<uint64_t, Slot> slots;
        ccstd::vector<NodeAddress> working;
        bool canReserve(const ccstd::vector<NodeAddress> &sources) const {
            ccstd::unordered_set<uint64_t> unique;
            for (const auto n : sources) {
                if (n.level < data.minTileLevel || n.level > data.maxLevel || n.x >= data.nodesX(n.level) || n.z >= data.nodesZ(n.level)) {
                    return false;
                }
                unique.insert(n.key());
            }
            return unique.size() <= capacity;
        }
        bool reserve(const ccstd::vector<NodeAddress> &sources) {
            if (!canReserve(sources)) {
                return false;
            }
            ccstd::unordered_set<uint64_t> wanted;
            working.clear();
            for (const auto n : sources) {
                if (wanted.insert(n.key()).second) {
                    working.push_back(n);
                }
            }
            for (auto it = slots.begin(); it != slots.end();) {
                if (!wanted.count(it->first)) {
                    it = slots.erase(it);
                } else {
                    ++it;
                }
            }
            ccstd::vector<bool> used(capacity, false);
            for (const auto &entry : slots) {
                used[entry.second.layer] = true;
            }
            uint32_t layer = 0;
            for (const auto n : working) {
                if (!slots.count(n.key())) {
                    while (used[layer]) {
                        ++layer;
                    }
                    slots.emplace(n.key(), Slot{layer, false});
                    used[layer] = true;
                }
            }
            return true;
        }
        int resident(NodeAddress source) const {
            const auto it = slots.find(source.key());
            return it != slots.end() && it->second.ready ? static_cast<int>(it->second.layer) : -1;
        }
    };

    TilePagePool();
    ~TilePagePool();
    bool init(gfx::Device *device, LandscapeAsset *asset, uint32_t layerCount);
    static bool supportsHeightUnorm(gfx::Device *device);
    bool heightIsUnorm() const { return _heightUnorm; }
    void destroy();
    bool valid() const { return _heightArray && _splatArray && _normalArray; }
    bool canReserve(const ccstd::vector<NodeAddress> &sources) const;
    bool reserve(const ccstd::vector<NodeAddress> &sources);
    bool loadReserved(bool synchronous = false);
    void update(uint32_t maxUploads);
    int resident(NodeAddress source) const;
    bool ready() const;
    bool failed() const;
    uint64_t updateRevision() const { return _updateRevision; }
    gfx::Texture *heightArray() const { return _heightArray; }
    gfx::Texture *splatArray() const { return _splatArray; }
    gfx::Texture *normalArray() const { return _normalArray; }
    gfx::Sampler *heightSampler() const { return _heightSampler; }
    gfx::Sampler *splatSampler() const { return _splatSampler; }
    uint32_t layerCount() const { return _layerCount; }

private:
    bool initTextures();
    void initSamplers();
    void uploadHeight(uint32_t layer, const uint8_t *data);
    void uploadLayer(gfx::Texture *array, uint32_t layer, const uint8_t *data) const;
    gfx::Device *_device{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    IntrusivePtr<gfx::Texture> _heightArray, _splatArray, _normalArray;
    gfx::Sampler *_heightSampler{nullptr}, *_splatSampler{nullptr};
    uint32_t _tileRes{129}, _layerCount{0};
    bool _heightUnorm{false};
    ccstd::vector<uint16_t> _heightUpload;
    Reservations _reservations;
    ccstd::unordered_set<uint64_t> _inFlight, _failed;
    uint64_t _updateRevision{0};
};

// Pure address planning and exact binding. No tree mutation, loading or search.
class TilePageResolver {
public:
    struct Tile {
        NodeAddress address;
        int layer{-1};
    };
    TilePageResolver(TilePagePool &pool, const LandscapeData &data);
    Tile resolve(NodeAddress node) const;
    Vec4 shaderParams(const Tile &tile) const;
    void pageSources(VTPageAddress page, bool bakeNormals, ccstd::vector<NodeAddress> &output) const;
    bool resolvePage(VTPageAddress page, bool bakeNormals, VTPageInputs &output) const;

private:
    uint32_t sourceLevelForWorldSize(float size) const;
    NodeAddress splatSource(VTPageAddress page) const;
    std::array<NodeAddress, config::VT_NORMAL_SOURCE_COUNT> normalSources(VTPageAddress page, bool bakeNormals) const;
    TilePagePool &_pool;
    LandscapeData _data;
    VTPageLayout _vtLayout;
};
} // namespace landscape
} // namespace cc

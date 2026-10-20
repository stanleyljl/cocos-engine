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
#include "landscape/VirtualTexture.h"
namespace cc {
namespace gfx {
class Device;
class Texture;
class Sampler;
} // namespace gfx
namespace landscape {
class LandscapeAsset;
// Physical slots are reserved BEFORE starting IO. In-flight work is bounded;
// only the coordinator's exact working set may be uploaded. Source mapping and
// shader bindings use exact resident tiles, with no missing-source fallback.
class TilePagePool {
public:
    struct Tile {
        NodeAddress address;
        int layer{-1};
    };
    TilePagePool();
    ~TilePagePool();
    bool init(gfx::Device *device, LandscapeAsset *asset, uint32_t layerCount, bool heightUnorm);
    bool heightIsUnorm() const { return _heightUnorm; }
    void destroy();
    bool valid() const { return _heightArray && _splatArray && _normalArray; }
    // Reservation inputs are unique addresses, collected by LandscapeSchedule.
    bool canReserve(const ccstd::vector<NodeAddress> &sources) const;
    // Reserve the whole working set or leave all current bindings untouched.
    bool reserve(const ccstd::vector<NodeAddress> &sources);
    bool loadReserved(bool synchronous = false);
    void update(uint32_t maxUploads);
    int resident(NodeAddress source) const;
    Tile resolve(NodeAddress node) const;
    Vec4 shaderParams(const Tile &tile) const;
    // Append unique source addresses in sampling order.
    void pageSources(VTPageAddress page, bool bakeNormals, ccstd::vector<NodeAddress> &output) const;
    bool resolvePage(VTPageAddress page, bool bakeNormals, VTPageInputs &output) const;
    bool ready() const;
    gfx::Texture *heightArray() const { return _heightArray; }
    gfx::Texture *splatArray() const { return _splatArray; }
    gfx::Texture *normalArray() const { return _normalArray; }
    gfx::Sampler *heightSampler() const { return _heightSampler; }
    gfx::Sampler *splatSampler() const { return _splatSampler; }
    uint32_t layerCount() const { return _capacity; }

private:
    friend struct LandscapePagingTestAccess;
    struct Slot {
        uint32_t layer;
        bool ready{false};
    };
    bool validSource(NodeAddress source) const;
    uint32_t sourceLevelForSize(float size) const;
    NodeAddress splatSource(VTPageAddress page) const;
    std::array<NodeAddress, config::VT_NORMAL_SOURCE_COUNT> normalSources(VTPageAddress page, bool bakeNormals) const;
    bool initTextures();
    void initSamplers();
    void uploadTile(uint64_t key, const uint8_t *height, const uint8_t *splat, const uint8_t *normal);
    void uploadLayer(gfx::Texture *array, uint32_t layer, const uint8_t *data) const;
    gfx::Device *_device{nullptr};
    IntrusivePtr<LandscapeAsset> _asset;
    IntrusivePtr<gfx::Texture> _heightArray;
    IntrusivePtr<gfx::Texture> _splatArray;
    IntrusivePtr<gfx::Texture> _normalArray;
    gfx::Sampler *_heightSampler{nullptr};
    gfx::Sampler *_splatSampler{nullptr};
    bool _heightUnorm{false};
    // Slot ownership and the exact working set share the same lifetime as the pool.
    LandscapeData _data;
    VTPageLayout _vtLayout;
    uint32_t _capacity{0};
    ccstd::unordered_map<uint64_t, Slot> _slots;
    ccstd::vector<NodeAddress> _working;
    // Includes decoded tiles until update() consumes them, even if no longer reserved.
    ccstd::unordered_set<uint64_t> _inFlight;
};

} // namespace landscape
} // namespace cc

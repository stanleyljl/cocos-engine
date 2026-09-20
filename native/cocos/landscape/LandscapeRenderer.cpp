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

#include "landscape/LandscapeRenderer.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

#include "base/Log.h"
#include "base/Macros.h"
#include "base/std/container/unordered_set.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/assets/RenderingSubMesh.h"
#include "core/scene-graph/Node.h"
#include "core/TypedArray.h"
#include "landscape/GridMesh.h"
#include "landscape/MaterialLibrary.h"
#include "landscape/TilePagePool.h"
#include "landscape/VTRenderer.h"
#include "math/Vec4.h"
#include "renderer/gfx-base/GFXBuffer.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/gfx-base/GFXFramebuffer.h"
#include "renderer/gfx-base/GFXTexture.h"
#include "renderer/gfx-base/states/GFXSampler.h"
#include "scene/RenderScene.h"
#include "scene/Octree.h"
#include "scene/Pass.h"

namespace cc {
namespace landscape {

namespace {
Material *createLandscapeMaterial(bool wireframe, bool decal = false) {
    auto *material = ccnew Material();
    IMaterialInfo info;
    info.effectName = ccstd::string{"builtin-landscape"};
    MacroRecord defines;
    defines["USE_INSTANCING"] = true;
    defines["LANDSCAPE_DEBUG_UNLIT"] = false;
    defines["LANDSCAPE_DECAL_MESH"] = decal;
    info.defines = IMaterialInfo::DefinesType{defines};

    if (wireframe) {
        RasterizerStateInfo rasterizer;
        rasterizer.polygonMode = gfx::PolygonMode::LINE;
        rasterizer.cullMode = gfx::CullMode::NONE;
        PassOverrides overrides;
        overrides.rasterizerState = rasterizer;
        info.states = IMaterialInfo::PassOverridesType{overrides};
    }

    material->initialize(info);
    return material;
}

} // namespace

LandscapeRenderer::LandscapeRenderer() = default;

LandscapeRenderer::~LandscapeRenderer() {
    destroy();
}

bool LandscapeRenderer::init(Node *node, scene::RenderScene *scene) {
    CC_ASSERT(_node == nullptr && _scene == nullptr);
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (node == nullptr || scene == nullptr || device == nullptr) {
        return false;
    }

    _node = node;
    _scene = scene;
    for (uint32_t i = 0; i < _meshes.size(); ++i) _meshes[i] = GridMesh::create(device, 1U << i);
    _materialSolid = createLandscapeMaterial(false);
    _materialWire = createLandscapeMaterial(true);
    _decalSolid = createLandscapeMaterial(false, true);
    _decalWire = createLandscapeMaterial(true, true);
    _decalMesh = GridMesh::create(device, 16);
    if (std::any_of(_meshes.begin(), _meshes.end(), [](const auto &mesh) { return mesh == nullptr; }) ||
        _decalMesh == nullptr ||
        _materialSolid == nullptr || _materialWire == nullptr || _decalSolid == nullptr || _decalWire == nullptr) {
        destroy();
        return false;
    }

    const auto passes = _materialSolid->getPasses();
    if (passes == nullptr || passes->empty()) {
        CC_LOG_WARNING("[Landscape] builtin-landscape material has no passes");
        destroy();
        return false;
    }
    _instanceAttributeScratch = Float32Array(4);
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::setLodRanges(const ccstd::vector<float> &morphStart,
                                     const ccstd::vector<float> &morphEnd) {
    _syncCache.invalidate();
    const size_t count = std::min({morphStart.size(), morphEnd.size(),
                                   static_cast<size_t>(config::MAX_LOD_LEVELS)});
    _morphStart.assign(config::MAX_LOD_LEVELS, 0.0F);
    _morphEnd.assign(config::MAX_LOD_LEVELS, 0.0F);
    for (size_t i = 0; i < count; ++i) {
        _morphStart[i] = morphStart[i];
        _morphEnd[i] = morphEnd[i];
    }
    updateMaterialProperties();
}

bool LandscapeRenderer::setAsset(LandscapeAsset *asset) {
    CC_ASSERT(_node != nullptr && _scene != nullptr && _asset == nullptr);
    if (asset == nullptr || !asset->valid() || Root::getInstance() == nullptr) {
        return false;
    }
    auto *root = Root::getInstance();
    auto *device = root != nullptr ? root->getDevice() : nullptr;
    if (device == nullptr) {
        return false;
    }
    _asset = asset;
    _data = asset->data();
    _decalCenters.clear();
    const float finestNodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, 0);
    for (const auto &d : asset->decals()) {
        const float x = d.x + d.size * 0.5F;
        const float z = d.z + d.size * 0.5F;
        float minY = _data.minHeight();
        float maxY = _data.maxHeight();
        asset->getHeightRange(0, static_cast<uint32_t>((x + _data.worldWidth() * 0.5F) / finestNodeSize),
                             static_cast<uint32_t>((z + _data.worldDepth() * 0.5F) / finestNodeSize), minY, maxY);
        // A stable height reference keeps the WHOLE decal's fade independent of
        // selected LOD, streaming height pages and individual vertex positions.
        _decalCenters.emplace_back(x, (minY + maxY) * 0.5F, z);
    }
    _vtRootLevel = vtRootLevel(_data.sectorSize);
    _heightSampleSpacing = computeNodeSize(_data.sectorSize, _data.maxLevel, _data.minTileLevel) /
                           static_cast<float>(_data.tileResolution - 1U);
    _tilePages = std::make_unique<TilePagePool>();
    if (!_tilePages->init(device, asset, config::PAGE_POOL_LAYERS)) {
        _tilePages.reset();
        return false;
    }
    _materialLibrary = std::make_unique<MaterialLibrary>();
    if (!_materialLibrary->init(device, *asset)) {
        return false;
    }
    _vtRenderer = std::make_unique<VTRenderer>();
    if (!_vtRenderer->init(*asset, *_tilePages, *_materialLibrary)) {
        CC_LOG_ERROR("[Landscape] VT initialization failed; terrain requires VT material shading");
        _vtRenderer.reset();
        return false;
    }
    _vtRenderer->setFrozen(_freezeLod);
    // Resource initialization succeeded. Bind the textures once; the mip
    // comparison toggle only replaces their Base Pass samplers afterwards.
    auto &vt = _vtRenderer->texture();
    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
        const auto setRuntimeTexture = [material](const char *name, gfx::Texture *texture, gfx::Sampler *sampler) {
            CC_ASSERT(texture != nullptr);
            material->setPropertyGFXTexture(name, texture);
            for (const auto &pass : *material->getPasses()) {
                const uint32_t handle = pass->getHandle(name);
                if (handle != 0U) {
                    const uint32_t binding = scene::Pass::getBindingFromHandle(handle);
                    pass->bindSampler(binding, sampler);
                }
            }
        };
        setRuntimeTexture("heightmap", _tilePages->heightArray(), _tilePages->heightSampler());
        setRuntimeTexture("terrainNormalMap", _tilePages->normalArray(), _tilePages->heightSampler());
        setRuntimeTexture("vtAlbedo", vt.albedo(), vt.sampler());
        if (material == _decalSolid.get() || material == _decalWire.get()) {
            setRuntimeTexture("decalHeightMap", _materialLibrary->decalHeight(), _tilePages->heightSampler());
        }
        setRuntimeTexture("vtNormalRoughnessAO", vt.normalRoughnessAO(), vt.sampler());
    }
    updateMaterialProperties();
    return true;
}

void LandscapeRenderer::setGlobalColorStrength(float strength) {
    if (_vtRenderer) _vtRenderer->setGlobalColorStrength(strength);
}

void LandscapeRenderer::setDecal3DEnabled(bool enabled) {
    _decal3DEnabled = enabled;
    updateMaterialProperties();
    // Decal visibility affects geometry only and remains immediate while residency is frozen.
    for (size_t i = 0; i < _decalDraws.size(); ++i) _decalDraws[i].model->setEnabled(enabled && i < _decalActive);
}

void LandscapeRenderer::setRVTNormalEnabled(bool enabled) {
    _rvtNormalEnabled = enabled;
    if (_freezeLod || !_vtRenderer) return;
    _vtRenderer->setRVTNormalEnabled(enabled);
    updateMaterialProperties();
}

void LandscapeRenderer::setHeightBlendEnabled(bool enabled) {
    if (_vtRenderer) _vtRenderer->setHeightBlendEnabled(enabled);
}

void LandscapeRenderer::setCliffEnabled(bool enabled) {
    _cliffEnabled = enabled;
    if (_freezeLod || !_vtRenderer) return;
    _vtRenderer->setCliffEnabled(enabled);
    _syncCache.invalidate();
}

void LandscapeRenderer::setVTMipEnabled(bool enabled) {
    if (!_vtRenderer || !_vtRenderer->valid()) return;
    auto &vt = _vtRenderer->texture();
    auto info = vt.sampler()->getInfo();
    info.mipFilter = enabled ? gfx::Filter::LINEAR : gfx::Filter::NONE;
    auto *sampler = Root::getInstance()->getDevice()->getSampler(info);
    // Keep all mip levels resident and current, including while filtering is
    // disabled. Switching is immediate and never invalidates frozen VT pages.
    // Vulkan's NONE mip filter maps to nearest-mip selection. Restrict the
    // sampled view to mip0 as well, using the existing attachment views.
    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
        material->setPropertyGFXTexture("vtAlbedo", enabled ? vt.albedo() : vt.framebuffer()->getColorTextures()[0]);
        material->setPropertyGFXTexture("vtNormalRoughnessAO", enabled ? vt.normalRoughnessAO() : vt.framebuffer()->getColorTextures()[1]);
        for (const auto &pass : *material->getPasses()) {
            for (const char *name : {"vtAlbedo", "vtNormalRoughnessAO"}) {
                const uint32_t handle = pass->getHandle(name);
                if (handle != 0U) {
                    pass->bindSampler(scene::Pass::getBindingFromHandle(handle), sampler);
                }
            }
            pass->update();
        }
    }
}

void LandscapeRenderer::setDebugFlags(bool lodColor, bool showRanges) {
    _lodColor = lodColor;
    _showRanges = showRanges;
    updateMaterialProperties();
}

IntrusivePtr<scene::Model> LandscapeRenderer::createModel(uint32_t meshIndex) {
    if (Root::getInstance() == nullptr || _node == nullptr || _scene == nullptr) {
        return nullptr;
    }

    auto *model = Root::getInstance()->createModel<scene::Model>();
    if (model == nullptr) {
        return nullptr;
    }
    model->setNode(_node);
    model->setTransform(_node);
    model->initSubModel(0, _meshes[meshIndex], _wireframe ? _materialWire.get() : _materialSolid.get());
    model->setEnabled(false);
    _scene->addModel(model);
    return model;
}

void LandscapeRenderer::updateMaterialProperties() {
    if (_materialSolid == nullptr || _materialWire == nullptr || _decalSolid == nullptr || _decalWire == nullptr) {
        return;
    }

    const Vec4 terrainParams{
        _data.heightScale,
        _data.heightBias,
        _lodColor ? 1.0F : 0.0F,
        _showRanges ? 1.0F : 0.0F,
    };
    ccstd::vector<Vec4> lodMorph(config::MAX_LOD_LEVELS);
    for (uint32_t i = 0; i < config::MAX_LOD_LEVELS; ++i) {
        const float start = i < _morphStart.size() ? _morphStart[i] : 0.0F;
        const float end = i < _morphEnd.size() ? _morphEnd[i] : 0.0F;
        lodMorph[i] = Vec4{start, end, 0.0F, 0.0F};
    }

    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
        material->setPropertyVec4("terrainParams", terrainParams);
        material->setPropertyVec4("decalControl", Vec4{_decal3DEnabled ? 1.0F : 0.0F, 0, 0, 0});
        material->setPropertyVec4("rvtNormalParams", Vec4{_vtRenderer && _vtRenderer->rvtNormalEnabled() ? 1.0F : 0.0F, 0, 0, 0});
        material->setPropertyVec4("heightParams", Vec4{_heightSampleSpacing,
                                                        static_cast<float>(_data.tileResolution),
                                                        0.0F, 0.0F});
        material->setPropertyVec4Array("lodMorph", lodMorph);
        material->setPropertyVec4("vtLayout", Vec4{static_cast<float>(config::VT_ATLAS_SIZE),
            static_cast<float>(config::VT_PAGE_RES), static_cast<float>(config::VT_PAGE_BORDER), 0.0F});
    }
    updateMorphCameraProperty();
}

void LandscapeRenderer::updateMorphCameraProperty() {
    if (_materialSolid == nullptr || _materialWire == nullptr || _decalSolid == nullptr || _decalWire == nullptr) {
        return;
    }

    const Vec4 cameraPosition{
        _viewPosition.x,
        _viewPosition.y,
        _viewPosition.z,
        0.0F,
    };
    for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
        material->setPropertyVec4("morphCameraPos", cameraPosition);
    }
}

void LandscapeRenderer::setInstanceAttribute(scene::Model *model, const char *name, const Vec4 &value) {
    // setInstancedAttribute copies into each submodel's own attribute block
    // synchronously, so this buffer can be reused across attributes and models.
    // Creating transient ArrayBuffers here also grows the external pointer
    // table in the bundled V8 build even after their JS objects are collected.
    auto &instance = ccstd::get<Float32Array>(_instanceAttributeScratch);
    instance[0] = value.x;
    instance[1] = value.y;
    instance[2] = value.z;
    instance[3] = value.w;
    model->setInstancedAttribute(name, _instanceAttributeScratch);
}

void LandscapeRenderer::updateInstanceData(scene::Model *model, const Patch &patch) {
    if (model == nullptr) {
        return;
    }

    const auto &node = patch.node;
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, std::min(node.level, _data.maxLevel));
    const float x = static_cast<float>(node.ix) * nodeSize - _data.worldWidth() * 0.5F;
    const float z = static_cast<float>(node.iz) * nodeSize - _data.worldDepth() * 0.5F;
    setInstanceAttribute(model, "a_gridInst", Vec4{x, z, nodeSize, static_cast<float>(node.level)});
    setInstanceAttribute(model, "a_quadrantInst", Vec4{patch.x / 16.0F, patch.z / 16.0F,
        static_cast<float>(1U << patch.meshIndex) / 16.0F, 0.0F});

    const TilePage tile = resolveTilePage(node);
    CC_ASSERTF(tile.layer >= 0,
               "[Landscape] missing resident root: node L%u (%u,%u), source L%u (%u,%u), layer=%d",
               node.level, node.ix, node.iz, tile.level, tile.x, tile.z, tile.layer);
    const float sourceNodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, tile.level);
    const Vec4 tileParams{
        static_cast<float>(tile.x) * sourceNodeSize - _data.worldWidth() * 0.5F,
        static_cast<float>(tile.z) * sourceNodeSize - _data.worldDepth() * 0.5F,
        sourceNodeSize,
        static_cast<float>(tile.layer),
    };
    // a_tileInst.xy = source tile origin in landscape-local meters,
    // z = source tile size in meters, w = shared height/splat array layer. For LODs finer than the
    // generated tile level, keep the complete source-tile range here: the
    // shader's local position selects the corresponding subregion of that tile.
    setInstanceAttribute(model, "a_tileInst", tileParams);
    setInstanceAttribute(model, "a_normalParentInst", this->tileParams(resolveNormalParent(node, tile)));
    const int vtSlot = resolveVTPage(patch.page);
    CC_ASSERTF(vtSlot >= 0, "[Landscape] missing permanent root VT page");
    const auto &vtRegion = _vtRenderer->texture().page(static_cast<uint32_t>(vtSlot)).region;
    // A fallback covers an ancestor's region, not the fine node's region.
    setInstanceAttribute(model, "a_vtInst", Vec4{vtRegion.x, vtRegion.y, vtRegion.z, static_cast<float>(vtSlot)});
}

int LandscapeRenderer::resolveVTPage(VTPageAddress page) const {
    auto &vt = _vtRenderer->texture();
    for (; page.level < _vtRootLevel; ++page.level, page.x >>= 1U, page.z >>= 1U) {
        const int slot = vt.findReadyPage(makeNodeKey(page.level, page.x, page.z));
        if (slot >= 0 && vt.requested(makeNodeKey(page.level, page.x, page.z))) return slot;
    }
    // Roots are pinned. On startup/invalidation their writes are guaranteed by
    // VTRenderer::render before the Base Pass; no unrendered fine page is sampled.
    return vt.findPage(makeNodeKey(_vtRootLevel, page.x, page.z));
}

Vec4 LandscapeRenderer::tileParams(const TilePage &tile) const {
    const float size = computeNodeSize(_data.sectorSize, _data.maxLevel, tile.level);
    return Vec4{tile.x * size - _data.worldWidth() * 0.5F,
                tile.z * size - _data.worldDepth() * 0.5F, size, static_cast<float>(tile.layer)};
}

LandscapeRenderer::TilePage LandscapeRenderer::resolveVTSource(const VTPageAddress &page) {
    // A material page can be finer than every height/splat tile. Choose the
    // containing source independently of the selected geometry node's LOD.
    const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxLevel && computeNodeSize(_data.sectorSize, _data.maxLevel, level) < size) ++level;
    const float sourceSize = computeNodeSize(_data.sectorSize, _data.maxLevel, level);
    return resolveTilePage(QuadNode{level, static_cast<uint32_t>((page.x + 0.5F) * size / sourceSize),
                                   static_cast<uint32_t>((page.z + 0.5F) * size / sourceSize)});
}

std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> LandscapeRenderer::resolveVTNormalSources(const VTPageAddress &page) {
    std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> sources;
    if (!_rvtNormalEnabled) {
        sources.fill(tileParams(resolveVTSource(page)));
        return sources;
    }
    // Match normal spacing to VT texels, independently of geometry LOD. A
    // 129-sample tile covers half a 256-texel page. The outer ring supplies
    // real neighboring normals for gutters instead of clamping at page edges.
    const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
    const float target = size * std::max(0.5F,
        static_cast<float>(_data.tileResolution - 1U) / config::VT_PAGE_INTERIOR);
    uint32_t level = _data.minTileLevel;
    while (level < _data.maxLevel && computeNodeSize(_data.sectorSize, _data.maxLevel, level) < target) ++level;
    for (;;) {
        const float sourceSize = computeNodeSize(_data.sectorSize, _data.maxLevel, level);
        const uint32_t countX = _data.sectorsX << (_data.maxLevel - level);
        const uint32_t countZ = _data.sectorsZ << (_data.maxLevel - level);
        uint32_t residentLevel = level;
        for (uint32_t q = 0; q < sources.size(); ++q) {
            const float x = (page.x - 0.25F + (q % 4U) * 0.5F) * size;
            const float z = (page.z - 0.25F + (q / 4U) * 0.5F) * size;
            const auto tile = resolveTilePage(QuadNode{level,
                static_cast<uint32_t>(std::clamp(std::floor(x / sourceSize), 0.0F, static_cast<float>(countX - 1U))),
                static_cast<uint32_t>(std::clamp(std::floor(z / sourceSize), 0.0F, static_cast<float>(countZ - 1U)))});
            residentLevel = std::max(residentLevel, tile.level);
            sources[q] = tileParams(tile);
        }
        // Publish one resolution for the entire page. Fine sources remain
        // requested; when all arrive, acquirePage invalidates this cached page.
        if (residentLevel == level) return sources;
        level = residentLevel;
    }
}

float LandscapeRenderer::patchDistance(const QuadNode &node, float x, float z, float size, bool farthest) const {
    // Match the translation-only terrain placement used by quadtree selection.
    const Vec3 camera = (farthest ? _viewPosition : _vtViewPosition) - _node->getWorldPosition();
    const auto axisDistance = [farthest](float p, float lo, float hi) {
        return farthest ? std::max(std::abs(p - lo), std::abs(p - hi)) : std::max({lo - p, p - hi, 0.0F});
    };
    const float dx = axisDistance(camera.x, x, x + size);
    const float dy = axisDistance(camera.y, node.minY, node.maxY);
    const float dz = axisDistance(camera.z, z, z + size);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void LandscapeRenderer::selectPatches(const QuadNode &node, uint32_t x, uint32_t z, uint32_t meshIndex,
                                      ccstd::vector<Patch> &patches) {
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
    const float cellSize = nodeSize / 16.0F;
    const uint32_t cells = 1U << meshIndex;
    const float size = cells * cellSize;
    const float minX = node.ix * nodeSize + x * cellSize;
    const float minZ = node.iz * nodeSize + z * cellSize;
    const float localX = minX - _data.worldWidth() * 0.5F;
    const float localZ = minZ - _data.worldDepth() * 0.5F;
    const float distance = patchDistance(node, localX, localZ, size, false);
    float density = 1.0F;
    if (_cliffEnabled) {
        float minY = node.minY, maxY = node.maxY;
        const uint32_t rangeLevel = node.level + meshIndex >= 4U ? node.level + meshIndex - 4U : 0U;
        const float rangeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, rangeLevel);
        const uint32_t rangeX = static_cast<uint32_t>(minX / rangeSize);
        const uint32_t rangeZ = static_cast<uint32_t>(minZ / rangeSize);
        _asset->getHeightRange(rangeLevel, rangeX, rangeZ, minY, maxY);
        density = cliffDensityScale(maxY - minY, size, _asset->getSurfaceStretch(rangeLevel, rangeX, rangeZ));
    }
    const uint32_t desired = vtDesiredLevel(_data.sectorSize, _vtRootLevel, distance / density);
    if (meshIndex > 0 && vtPageWorldSize(_data.sectorSize, _vtRootLevel, desired) < size) {
        const uint32_t half = cells / 2U;
        for (uint32_t q = 0; q < 4; ++q) {
            selectPatches(node, x + (q & 1U) * half, z + (q >> 1U) * half, meshIndex - 1U, patches);
        }
        return;
    }
    // An odd boundary vertex slides toward the previous even vertex. A page
    // must contain that entire trajectory, not just the unmorphed grid cell.
    const bool canMorph = node.level < _morphStart.size() &&
        patchDistance(node, localX, localZ, size, true) >= _morphStart[node.level];
    const float safeMinX = minX - (canMorph ? (x & 1U) * cellSize : 0.0F);
    const float safeMinZ = minZ - (canMorph ? (z & 1U) * cellSize : 0.0F);
    const auto page = vtCoveringPage(_data.sectorSize, _vtRootLevel, desired,
                                     safeMinX, safeMinZ, minX + size, minZ + size);
    patches.push_back(Patch{node, x, z, meshIndex, page, size * density / std::max(distance, 1.0F)});
}

LandscapeRenderer::TilePage LandscapeRenderer::resolveNormalParent(const QuadNode &node, const TilePage &tile) {
    // Finer geometry already shares the finest source normal map. A streaming
    // fallback likewise must not morph towards an extra-coarse level again.
    if (node.level != tile.level || node.level >= _data.maxLevel) return tile;
    return resolveTilePage(QuadNode{node.level + 1U, node.ix >> 1U, node.iz >> 1U});
}

LandscapeRenderer::TilePage LandscapeRenderer::resolveTilePage(const QuadNode &node) {
    if (_tilePages == nullptr || _asset == nullptr) {
        return {};
    }
    TilePage tile;
    tile.level = std::max(node.level, _data.minTileLevel);
    const uint32_t shift = tile.level - node.level;
    tile.x = node.ix >> shift;
    tile.z = node.iz >> shift;
    const uint64_t key = makeNodeKey(tile.level, tile.x, tile.z);
    const auto cached = _tileResolveCache.find(key);
    if (cached != _tileResolveCache.end()) return cached->second;
    // Only the desired tile starts an asynchronous request. Ancestors are
    // queried without loading and remain protected while used as fallbacks.
    tile.layer = _tilePages->query(tile.level, tile.x, tile.z);
    while (tile.layer < 0 && tile.level < _data.maxLevel) {
        ++tile.level;
        tile.x >>= 1U;
        tile.z >>= 1U;
        tile.layer = _tilePages->peekResident(makeNodeKey(tile.level, tile.x, tile.z));
    }
    _tileResolveCache.emplace(key, tile);
    return tile; // initialization guarantees a resident root for every sector
}

void LandscapeRenderer::updateModel(ModelState &state, const Patch &patch) {
    auto *model = state.model.get();
    updateInstanceData(model, patch);
    state.patch = patch;
    model->setEnabled(true);
    updateModelBounds(model, patch);
}

void LandscapeRenderer::updateModelBounds(scene::Model *model, const Patch &patch) {
    if (_freezeLod) {
        // The frozen selection already passed the terrain frustum test. Models
        // without world bounds bypass both forward and custom pipeline culling.
        // Remove any octree entry before clearing bounds to avoid stale queries.
        if (_scene != nullptr && _scene->getOctree() != nullptr) {
            _scene->getOctree()->remove(model);
        }
        model->setWorldBounds(nullptr);
        model->updateOctree();
        return;
    }

    const auto &node = patch.node;
    const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
    const float cellSize = nodeSize / 16.0F;
    const float size = (1U << patch.meshIndex) * cellSize;
    const float x = node.ix * nodeSize - _data.worldWidth() * 0.5F + patch.x * cellSize;
    const float z = node.iz * nodeSize - _data.worldDepth() * 0.5F + patch.z * cellSize;
    model->createBoundingShape(Vec3{x - (patch.x & 1U) * cellSize, node.minY, z - (patch.z & 1U) * cellSize},
                                Vec3{x + size, node.maxY, z + size});
    model->updateWorldBound();
    model->updateOctree();
}

void LandscapeRenderer::sync(const ccstd::vector<QuadNode> &selected) {
    if (_freezeLod || !valid()) {
        return;
    }

    auto &vt = _vtRenderer->texture();
    const auto &origin = _node->getWorldPosition();
    const LandscapeSyncCache::Positions positions{
        _viewPosition.x, _viewPosition.y, _viewPosition.z,
        _vtViewPosition.x, _vtViewPosition.y, _vtViewPosition.z, origin.x, origin.y, origin.z};
    bool uploadsPolled = false;
    if (_syncCache.matches(positions, _tilePages->updateRevision(), vt.contentRevision(), selected)) {
        // Keep last frame's source protection while polling async completions.
        // A stationary camera must still advance loading and fine-page publication.
        _tilePages->update(config::PAGE_UPLOAD_BUDGET);
        uploadsPolled = true;
        if (_syncCache.matches(positions, _tilePages->updateRevision(), vt.contentRevision(), selected)) return;
    }

    ccstd::vector<Patch> patches;
    patches.reserve(selected.size() * 4U);
    for (const auto &node : selected) {
        for (uint32_t q = 0; q < 4; ++q) {
            if ((node.quadrantMask & (1U << q)) != 0) selectPatches(node, (q & 1U) * 8U, (q >> 1U) * 8U, 3U, patches);
        }
    }

    struct Request { VTPageAddress page; float priority; uint64_t key; };
    ccstd::unordered_map<uint64_t, Request> unique;
    for (const auto &patch : patches) {
        auto page = patch.page;
        for (; page.level < _vtRootLevel; ++page.level, page.x >>= 1U, page.z >>= 1U) {
            const float priority = vtAncestorPriority(patch.vtPriority, page.level - patch.page.level);
            const uint64_t key = makeNodeKey(page.level, page.x, page.z);
            auto inserted = unique.emplace(key, Request{page, priority, key});
            inserted.first->second.priority = std::max(inserted.first->second.priority, priority);
        }
    }
    ccstd::vector<Request> requests;
    requests.reserve(unique.size());
    for (const auto &entry : unique) requests.push_back(entry.second);
    std::sort(requests.begin(), requests.end(), [](const Request &a, const Request &b) {
        return a.priority != b.priority ? a.priority > b.priority : a.key < b.key;
    });
    // Bound the working set BEFORE protecting pages. Keeping all desired keys
    // pinned would prevent near pages from replacing distant pages after moving.
    const size_t capacity = config::VT_PAGE_COUNT - static_cast<size_t>(_data.sectorsX) * _data.sectorsZ;
    if (requests.size() > capacity) requests.resize(capacity);
    ccstd::vector<uint64_t> keys;
    keys.reserve(requests.size());
    for (const auto &request : requests) keys.push_back(request.key);
    vt.beginFrame(keys);
    _tilePages->beginFrame();
    _vtRenderer->syncCliffSources(*_tilePages);
    _tileResolveCache.clear();
    // Protect both current and parent normals before recycling any tile layer.
    for (const auto &node : selected) resolveNormalParent(node, resolveTilePage(node));
    // Roots also need refreshed normal sources: their finer children are not
    // permanently resident, unlike the root height/splat source itself.
    for (uint32_t z = 0; z < _data.sectorsZ; ++z) {
        for (uint32_t x = 0; x < _data.sectorsX; ++x) {
            requests.push_back(Request{VTPageAddress{_vtRootLevel, x, z}, 0.0F, makeNodeKey(_vtRootLevel, x, z)});
        }
    }
    struct Sources { Vec4 splat; std::array<Vec4, config::VT_NORMAL_SOURCE_COUNT> normals; };
    ccstd::vector<Sources> sources;
    sources.reserve(requests.size());
    for (const auto &request : requests) {
        sources.push_back({tileParams(resolveVTSource(request.page)), resolveVTNormalSources(request.page)});
    }
    const auto beforeUpload = _tilePages->updateRevision();
    if (!uploadsPolled) _tilePages->update(config::PAGE_UPLOAD_BUDGET);
    const bool sourcesChanged = _tilePages->updateRevision() != beforeUpload;
    if (sourcesChanged) _vtRenderer->syncCliffSources(*_tilePages);
    if (sourcesChanged) _tileResolveCache.clear();
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto &request = requests[i];
        const auto &page = request.page;
        const float size = vtPageWorldSize(_data.sectorSize, _vtRootLevel, page.level);
        const Vec4 region{page.x * size - _data.worldWidth() * 0.5F,
                          page.z * size - _data.worldDepth() * 0.5F, size, 0.0F};
        // Resolve again only if uploads changed the available source layers.
        if (sourcesChanged) sources[i] = {tileParams(resolveVTSource(page)), resolveVTNormalSources(page)};
        vt.acquirePage(request.key, region, sources[i].splat, sources[i].normals,
                       page.level == _vtRootLevel, request.priority);
    }

    ccstd::unordered_set<uint64_t> visible;
    visible.reserve(patches.size());
    for (const auto &patch : patches) {
        const uint64_t key = makeNodeKey(patch.node.level * 4U + patch.meshIndex,
                                        patch.node.ix * 16U + patch.x, patch.node.iz * 16U + patch.z);
        visible.insert(key);
    }
    // Retire old patches before creating replacements so zooming does not
    // unnecessarily grow the model pools.
    for (auto iter = _active.begin(); iter != _active.end();) {
        if (visible.count(iter->first) == 0) {
            iter->second.model->setEnabled(false);
            _pool[iter->second.patch.meshIndex].emplace_back(iter->second.model);
            iter = _active.erase(iter);
        } else {
            ++iter;
        }
    }
    for (const auto &patch : patches) {
        const uint64_t key = makeNodeKey(patch.node.level * 4U + patch.meshIndex,
                                        patch.node.ix * 16U + patch.x, patch.node.iz * 16U + patch.z);
        auto iter = _active.find(key);
        if (iter == _active.end()) {
            auto &pool = _pool[patch.meshIndex];
            IntrusivePtr<scene::Model> model;
            if (!pool.empty()) {
                model = pool.back();
                pool.pop_back();
            } else {
                model = createModel(patch.meshIndex);
            }
            if (model == nullptr) continue;
            iter = _active.emplace(key, ModelState{model, patch}).first;
            updateModel(iter->second, patch);
        } else {
            auto &state = iter->second;
            const bool boundsChanged = state.patch.node.minY != patch.node.minY || state.patch.node.maxY != patch.node.maxY;
            state.patch = patch;
            updateInstanceData(state.model, patch);
            if (boundsChanged) updateModelBounds(state.model, patch);
        }
    }
    syncDecals(patches);
    // Snapshot before BeforeRender: publishing new pages changes the revision
    // and forces bindings to advance from ancestor pages on the next frame.
    _syncCache.store(positions, _tilePages->updateRevision(), vt.contentRevision(), selected);
}

void LandscapeRenderer::updateDecalInstance(const DecalDraw &draw) {
    updateInstanceData(draw.model, draw.patch);
    const auto &d = _asset->decals()[draw.decal];
    const auto &layer = _asset->decalLayers()[d.layer];
    setInstanceAttribute(draw.model, "a_decalRegion", Vec4{d.x, d.z, d.size, static_cast<float>(d.layer)});
    setInstanceAttribute(draw.model, "a_decalGrid", draw.grid);
    const float distance = (_viewPosition - (_node->getWorldPosition() + _decalCenters[draw.decal])).length();
    const float t = std::clamp((distance - d.nearDistance) / (d.farDistance - d.nearDistance), 0.0F, 1.0F);
    setInstanceAttribute(draw.model, "a_decalFade", Vec4{layer.heightScale, 1.0F - t * t * (3.0F - 2.0F * t), 0, 0});
    auto bounds = draw.patch;
    bounds.node.maxY += layer.heightScale;
    updateModelBounds(draw.model, bounds);
}

void LandscapeRenderer::syncDecals(const ccstd::vector<Patch> &patches) {
    _decalActive = 0;
    // Every decal uses a fixed lattice at 1/16 of the finest terrain cell.
    // Split on finest-cell boundaries once conceptually: all possible terrain
    // patches align with them, so changing patch ownership cannot retessellate
    // the decal. Only its base height and resident material mapping may change.
    const float step = computeNodeSize(_data.sectorSize, _data.maxLevel, 0) / 16.0F;
    const float originX = -_data.worldWidth() * 0.5F;
    const float originZ = -_data.worldDepth() * 0.5F;
    for (const auto &patch : patches) {
        const auto &node = patch.node;
        const float nodeSize = computeNodeSize(_data.sectorSize, _data.maxLevel, node.level);
        const float cell = nodeSize / 16.0F;
        const float size = (1U << patch.meshIndex) * cell;
        const float x = node.ix * nodeSize - _data.worldWidth() * .5F + patch.x * cell;
        const float z = node.iz * nodeSize - _data.worldDepth() * .5F + patch.z * cell;
        for (uint32_t i = 0; i < _asset->decals().size(); ++i) {
            const auto &d = _asset->decals()[i];
            // Layers without displacement live entirely in RVT. Do not instantiate
            // grids that would sample heights only to discard every fragment.
            if (_asset->decalLayers()[d.layer].heightScale == 0.0F) continue;
            const float distance = (_viewPosition - (_node->getWorldPosition() + _decalCenters[i])).length();
            if (distance >= d.farDistance || x + size <= d.x || z + size <= d.z ||
                x >= d.x + d.size || z >= d.z + d.size) continue;
            const int firstX = static_cast<int>(std::floor((std::max(x, d.x) - originX) / step));
            const int firstZ = static_cast<int>(std::floor((std::max(z, d.z) - originZ) / step));
            const int endX = static_cast<int>(std::ceil((std::min(x + size, d.x + d.size) - originX) / step));
            const int endZ = static_cast<int>(std::ceil((std::min(z + size, d.z + d.size) - originZ) / step));
            for (int iz = firstZ; iz < endZ; ++iz) for (int ix = firstX; ix < endX; ++ix) {
                const float left = std::max(originX + ix * step, d.x);
                const float bottom = std::max(originZ + iz * step, d.z);
                const float right = std::min(originX + (ix + 1) * step, d.x + d.size);
                const float top = std::min(originZ + (iz + 1) * step, d.z + d.size);
                const size_t at = _decalActive++;
                if (at == _decalDraws.size()) {
                    IntrusivePtr<scene::Model> model = Root::getInstance()->createModel<scene::Model>();
                    model->setNode(_node);
                    model->setTransform(_node);
                    model->initSubModel(0, _decalMesh, _wireframe ? _decalWire.get() : _decalSolid.get());
                    _scene->addModel(model);
                    _decalDraws.push_back(DecalDraw{model, patch, i, {}});
                }
                auto &draw = _decalDraws[at];
                draw.patch = patch;
                draw.decal = i;
                draw.grid = Vec4{left, bottom, right - left, top - bottom};
                updateDecalInstance(draw);
                draw.model->setEnabled(_decal3DEnabled);
            }
        }
    }
    for (size_t i = _decalActive; i < _decalDraws.size(); ++i) _decalDraws[i].model->setEnabled(false);
}

void LandscapeRenderer::setFreezeLod(bool frozen) {
    if (_freezeLod == frozen) {
        return;
    }
    _freezeLod = frozen;
    if (_vtRenderer) _vtRenderer->setFrozen(frozen);
    for (size_t i = 0; i < _decalActive; ++i) updateDecalInstance(_decalDraws[i]);
    if (!frozen) setRVTNormalEnabled(_rvtNormalEnabled);
    if (!frozen) setCliffEnabled(_cliffEnabled);
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        updateModelBounds(state.model, state.patch);
    }
}

void LandscapeRenderer::setWireframe(bool wireframe) {
    _wireframe = wireframe;
    for (size_t i = 0; i < _decalDraws.size(); ++i) {
        const auto &draw = _decalDraws[i];
        draw.model->setSubModelMaterial(0, wireframe ? _decalWire.get() : _decalSolid.get());
        if (i < _decalActive) updateDecalInstance(draw);
    }
    for (const auto &entry : _active) {
        const auto &state = entry.second;
        state.model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        updateInstanceData(state.model, state.patch);
    }
    for (const auto &pool : _pool) {
        for (const auto &model : pool) model->setSubModelMaterial(0, _wireframe ? _materialWire.get() : _materialSolid.get());
        // Inactive models get fresh instance data when reused. Do not request
        // height/VT pages here: doing so would pin invisible nodes in the caches.
    }
}

void LandscapeRenderer::setViewPos(const Vec3 &position) {
    if (_freezeLod) return;
    _vtViewPosition = position;
    if (_viewPosition.x == position.x &&
        _viewPosition.y == position.y &&
        _viewPosition.z == position.z) {
        return;
    }
    _viewPosition = position;
    updateMorphCameraProperty();
}

RenderTexture *LandscapeRenderer::debugAtlas() const {
    return _vtRenderer ? _vtRenderer->texture().atlas() : nullptr;
}

void LandscapeRenderer::setUnlit(bool enabled) {
    if (_unlit == enabled || !valid()) return;
    // These passes belong exclusively to this renderer. Update their shader
    // variants in place, preserving texture bindings and instanced batching.
    const auto compile = [this](bool unlit) {
        bool success = true;
        for (auto *material : {_materialSolid.get(), _materialWire.get(), _decalSolid.get(), _decalWire.get()}) {
            for (const auto &pass : *material->getPasses()) {
                pass->getDefines()["LANDSCAPE_DEBUG_UNLIT"] = unlit;
                success = pass->tryCompile() && success;
            }
        }
        return success;
    };
    if (compile(enabled)) {
        _unlit = enabled;
    } else {
        compile(_unlit);
        CC_LOG_ERROR("[Landscape] failed to switch unlit shader variant");
    }
    // Refresh cached submodel shaders and instance attributes, including pooled
    // models. This is also required while the LOD selection is frozen.
    setWireframe(_wireframe);
}

bool LandscapeRenderer::valid() const {
    return _scene != nullptr && _node != nullptr && _meshes[0] != nullptr &&
           _materialSolid != nullptr && _materialWire != nullptr &&
           _tilePages != nullptr && _tilePages->valid() && _asset != nullptr &&
           _materialLibrary != nullptr && _materialLibrary->valid() &&
           _vtRenderer != nullptr && _vtRenderer->valid();
}

void LandscapeRenderer::destroy() {
    _syncCache.invalidate();
    _tileResolveCache.clear();
    auto removeModel = [this](const IntrusivePtr<scene::Model> &model) {
        if (model == nullptr) {
            return;
        }
        if (_scene != nullptr && model->getScene() == _scene) {
            _scene->removeModel(model);
        }
        model->destroy();
    };

    for (const auto &draw : _decalDraws) removeModel(draw.model);
    _decalDraws.clear();
    _decalActive = 0;
    _decalCenters.clear();
    for (const auto &entry : _active) {
        removeModel(entry.second.model);
    }
    for (auto &pool : _pool) {
        for (const auto &model : pool) removeModel(model);
        pool.clear();
    }
    _active.clear();
    _instanceAttributeScratch = ccstd::monostate{};
    _vtRenderer.reset();

    _tilePages.reset();
    _asset = nullptr;
    for (auto &mesh : _meshes) mesh = nullptr;
    _materialSolid = nullptr;
    _materialWire = nullptr;
    _decalSolid = nullptr;
    _decalWire = nullptr;
    _decalMesh = nullptr;
    _materialLibrary.reset();
    _node = nullptr;
    _scene = nullptr;
    _unlit = false;
}

} // namespace landscape
} // namespace cc

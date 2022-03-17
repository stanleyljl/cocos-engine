/****************************************************************************
 Copyright (c) 2021 Xiamen Yaji Software Co., Ltd.
 
 http://www.cocos.com
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.
 
 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "DebugRenderer.h"
#include "Profiler.h"
#include "application/ApplicationManager.h"
#include "base/Log.h"
#include "base/UTF8.h"
#include "base/memory/Memory.h"
#include "core/assets/BitmapFont.h"
#include "core/assets/FreeTypeFont.h"
#include "math/Vec2.h"
#include "platform/interfaces/modules/ISystemWindow.h"
#include "platform/interfaces/modules/Device.h"
#include "renderer/gfx-base/GFXDescriptorSet.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "renderer/pipeline/Define.h"
#include "renderer/pipeline/PipelineStateManager.h"
#include "renderer/pipeline/RenderPipeline.h"
#include "renderer/pipeline/GlobalDescriptorSetManager.h"
#include "renderer/pipeline/PipelineSceneData.h"

namespace cc {

constexpr uint32_t DEBUG_FONT_SIZE         = 10U;
constexpr uint32_t DEBUG_MAX_CHARACTERS    = 10000U;
constexpr uint32_t DEBUG_VERTICES_PER_CHAR = 6U;

inline uint32_t getFontIndex(bool bold, bool italic) {
    /**
     * Regular
     * Bold
     * Italic
     * BoldItalic
     */
    uint32_t index = 0;
    index |= bold ? 1 : 0;
    index |= italic ? 2 : 0;

    return index;
}

inline std::string getFontPath(uint32_t index) {
    // stanley todo: use readable names later.
    static const std::string uuids[DEBUG_FONT_COUNT] = {
        "0835f102-5471-47a3-9a76-01c07ac9cdb2", //"OpenSans-Regular",
        "b5475517-23b9-4873-bc1a-968d96616081", //"OpenSans-Bold",
        "0ed97c56-390e-4dd1-96b7-e7f2d93a98ed", //"OpenSans-Italic",
        "b23391b6-52eb-46a6-8da1-6244d9d315fb", //"OpenSans-BoldItalic"
    };

    auto *asset = BuiltinResMgr::getInstance()->getAsset(uuids[index]);

    return asset->getNativeUrl();
}

struct DebugVertex {
    DebugVertex() = default;
    DebugVertex(const Vec2 &pos, const Vec2 &tuv, gfx::Color clr)
    : position(pos), uv(tuv), color(clr) {}

    Vec2       position;
    Vec2       uv;
    gfx::Color color;
};

struct DebugBatch {
    DebugBatch(gfx::Device *device, pipeline::RenderPipeline *pipeline, bool bd, bool it, gfx::Texture *tex)
    : bold(bd), italic(it), texture(tex) {
        gfx::DescriptorSetLayoutInfo info;
        info.bindings.push_back({0, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::FRAGMENT});

        descriptorSetLayout = device->createDescriptorSetLayout(info);
        descriptorSet       = device->createDescriptorSet({descriptorSetLayout});

        auto *sampler = pipeline->getGlobalDSManager()->getLinearSampler();
        descriptorSet->bindSampler(0, sampler);
        descriptorSet->bindTexture(0, texture);
        descriptorSet->update();
    }

    ~DebugBatch() {
        CC_SAFE_DESTROY(descriptorSet);
        CC_SAFE_DESTROY(descriptorSetLayout);
    }

    inline bool match(bool b, bool i, gfx::Texture *tex) {
        return bold == b && italic == i && texture == tex;
    }

    std::vector<DebugVertex>  vertices;
    bool                      bold{false};
    bool                      italic{false};
    gfx::Texture *            texture{nullptr};
    gfx::DescriptorSet *      descriptorSet{nullptr};
    gfx::DescriptorSetLayout *descriptorSetLayout{nullptr};
};

class DebugVertexBuffer {
public:
    inline void init(gfx::Device *device, uint32_t maxVertices, const gfx::AttributeList &attributes) {
        _maxVertices = maxVertices;
        _buffer      = device->createBuffer({gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
                                        gfx::MemoryUsageBit::DEVICE,
                                        static_cast<uint32_t>(_maxVertices * sizeof(DebugVertex)),
                                        static_cast<uint32_t>(sizeof(DebugVertex))});

        gfx::InputAssemblerInfo info;
        info.attributes = attributes;
        info.vertexBuffers.push_back(_buffer);
        _inputAssembler = device->createInputAssembler(info);
        CC_PROFILE_MEMORY_INC(DebugVertexBuffer, static_cast<uint32_t>(_maxVertices * sizeof(DebugVertex)));
    }

    inline void update() {
        if (empty()) {
            return;
        }

        std::vector<DebugVertex> vertices;
        for (auto *batch : _batches) {
            vertices.insert(vertices.end(), batch->vertices.begin(), batch->vertices.end());
        }

        const auto count = std::min(static_cast<uint32_t>(vertices.size()), _maxVertices);
        const auto size  = static_cast<uint32_t>(count * sizeof(DebugVertex));
        _buffer->update(&vertices[0], size);
    }

    inline void destroy() {
        for (auto *batch : _batches) {
            CC_SAFE_DELETE(batch);
        }

        CC_SAFE_DESTROY(_buffer);
        CC_SAFE_DESTROY(_inputAssembler);
        CC_PROFILE_MEMORY_DEC(DebugVertexBuffer, static_cast<uint32_t>(_maxVertices * sizeof(DebugVertex)));
    }

    DebugBatch &getOrCreateBatch(gfx::Device *device, pipeline::RenderPipeline *pipeline, bool bold, bool italic, gfx::Texture *texture) {
        for (auto *batch : _batches) {
            if (batch->match(bold, italic, texture)) {
                return *batch;
            }
        }

        DebugBatch *batch = new DebugBatch(device, pipeline, bold, italic, texture);
        _batches.push_back(batch);

        return *batch;
    }

    inline bool empty() const {
        for (auto *batch : _batches) {
            if (!batch->vertices.empty()) {
                return false;
            }
        }

        return true;
    }
    inline void reset() {
        for (auto *batch : _batches) {
            batch->vertices.clear();
        }
    }

private:
    uint32_t                  _maxVertices{0U};
    std::vector<DebugBatch *> _batches;
    gfx::Buffer *             _buffer{nullptr};
    gfx::InputAssembler *     _inputAssembler{nullptr};

    friend class DebugRenderer;
};

DebugRendererInfo::DebugRendererInfo()
: fontSize(DEBUG_FONT_SIZE), maxCharacters(DEBUG_MAX_CHARACTERS) {
}

DebugRenderer *DebugRenderer::_instance = nullptr;
DebugRenderer *DebugRenderer::getInstance() {
    if (!_instance) {
        _instance = new DebugRenderer();
    }

    return _instance;
}

void DebugRenderer::destroyInstance() {
    if (_instance) {
        delete _instance;
        _instance = nullptr;
    }
}

void DebugRenderer::activate(gfx::Device *device, pipeline::RenderPipeline *pipeline, const DebugRendererInfo &info) {
    _device   = device;
    _pipeline = pipeline;

    static const gfx::AttributeList attributes = {
        {"a_position", gfx::Format::RG32F},
        {"a_texCoord", gfx::Format::RG32F},
        {"a_color", gfx::Format::RGBA32F}};

    _buffer = new DebugVertexBuffer();
    _buffer->init(_device, info.maxCharacters * DEBUG_VERTICES_PER_CHAR, attributes);

    const auto *window   = CC_CURRENT_ENGINE()->getInterface<ISystemWindow>();
    const auto  width    = window->getViewSize().x * Device::getDevicePixelRatio();
    const auto  fontSize = std::clamp(static_cast<uint32_t>(width / 800.0F * info.fontSize), 10U, 20U);

    for (auto i = 0U; i < _fonts.size(); i++) {
        _fonts[i].font           = new FreeTypeFont(getFontPath(i));
        _fonts[i].face           = _fonts[i].font->createFace(FontFaceInfo(fontSize));
        _fonts[i].invTextureSize = {1.0F / _fonts[i].face->getTextureWidth(), 1.0F / _fonts[i].face->getTextureHeight()};
    }
}

void DebugRenderer::render(gfx::RenderPass *renderPass, gfx::CommandBuffer *cmdBuff) {
    CC_PROFILE(DebugRendererRender);
    if (!_buffer || _buffer->empty()) {
        return;
    }

    _buffer->update();

    const auto *sceneData = _pipeline->getPipelineSceneData();
    const auto &pass      = sceneData->getDebugRendererPass();
    const auto &shader    = sceneData->getDebugRendererShader();

    auto *pso = pipeline::PipelineStateManager::getOrCreatePipelineState(pass, shader, _buffer->_inputAssembler, renderPass);
    cmdBuff->bindPipelineState(pso);
    cmdBuff->bindInputAssembler(_buffer->_inputAssembler);

    uint32_t offset = 0U;
    for (auto *batch : _buffer->_batches) {
        auto count = std::min(static_cast<uint32_t>(batch->vertices.size()), _buffer->_maxVertices - offset);
        if (count == 0U) {
            break;
        }

        gfx::DrawInfo drawInfo;
        drawInfo.firstVertex = offset;
        drawInfo.vertexCount = count;

        cmdBuff->bindDescriptorSet(pipeline::materialSet, batch->descriptorSet);
        cmdBuff->draw(drawInfo);

        offset += count;
    }

    // reset all debug data for next frame
    _buffer->reset();
}

void DebugRenderer::destroy() {
    CC_SAFE_DESTROY(_buffer);

    for (auto &iter : _fonts) {
        CC_SAFE_DELETE(iter.font);
    }
}

void DebugRenderer::addText(const std::string &text, const Vec2 &screenPos, const DebugTextInfo &info) {
    uint32_t index    = getFontIndex(info.bold, info.italic);
    auto &   fontInfo = _fonts[index];
    auto *   face     = fontInfo.face;

    if (!_buffer || !face || text.empty()) {
        return;
    }

    std::u32string unicodeText;
    bool           success = StringUtils::UTF8ToUTF32(text, unicodeText);
    if (!success) {
        return;
    }

    auto        offsetX        = screenPos.x;
    auto        offsetY        = screenPos.y;
    const auto  scale          = info.scale;
    const auto  lineHeight     = face->getLineHeight() * scale;
    const auto &invTextureSize = fontInfo.invTextureSize;

    for (auto i = 0U; i < unicodeText.size(); i++) {
        const auto code = unicodeText[i];

        if (code == '\r') {
            continue;
        }

        if (code == '\n') {
            offsetX = screenPos.x;
            offsetY += lineHeight;
            continue;
        }

        const auto *glyph = face->getGlyph(code);
        if (!glyph) {
            continue;
        }

        if (glyph->width > 0U && glyph->height > 0U) {
            auto &batch = _buffer->getOrCreateBatch(_device, _pipeline, info.bold, info.italic, face->getTexture(glyph->page));

            Vec4 rect{offsetX + glyph->bearingX * scale, offsetY - glyph->bearingY * scale, glyph->width * scale, glyph->height * scale};
            Vec4 uv{glyph->x * invTextureSize.x, glyph->y * invTextureSize.y, glyph->width * invTextureSize.x, glyph->height * invTextureSize.y};

            if (info.shadow) {
                for (auto x = 1U; x <= info.shadowThickness; x++) {
                    for (auto y = 1U; y <= info.shadowThickness; y++) {
                        Vec4 shadowRect(rect.x + x, rect.y + y, rect.z, rect.w);
                        addQuad(batch, shadowRect, uv, info.shadowColor);
                    }
                }
            }

            addQuad(batch, rect, uv, info.color);
        }

        offsetX += glyph->advance * scale;

#ifdef USE_KERNING
        if (i < unicodeText.size() - 1) {
            offsetX += face->getKerning(code, unicodeText[i + 1]) * scale;
        }
#endif
    }
}

uint32_t DebugRenderer::getLineHeight(bool bold, bool italic) {
    uint32_t index    = getFontIndex(bold, italic);
    auto &   fontInfo = _fonts[index];

    if (fontInfo.face) {
        return fontInfo.face->getLineHeight();
    }

    return 0U;
}

void DebugRenderer::addQuad(DebugBatch &batch, const Vec4 &rect, const Vec4 &uv, gfx::Color color) {
    DebugVertex quad[4] = {
        {Vec2(rect.x, rect.y), Vec2(uv.x, uv.y), color},
        {Vec2(rect.x + rect.z, rect.y), Vec2(uv.x + uv.z, uv.y), color},
        {Vec2(rect.x, rect.y + rect.w), Vec2(uv.x, uv.y + uv.w), color},
        {Vec2(rect.x + rect.z, rect.y + rect.w), Vec2(uv.x + uv.z, uv.y + uv.w), color}};

    // first triangle
    batch.vertices.emplace_back(quad[0]);
    batch.vertices.emplace_back(quad[1]);
    batch.vertices.emplace_back(quad[2]);

    // second triangle
    batch.vertices.emplace_back(quad[1]);
    batch.vertices.emplace_back(quad[3]);
    batch.vertices.emplace_back(quad[2]);
}

} // namespace cc

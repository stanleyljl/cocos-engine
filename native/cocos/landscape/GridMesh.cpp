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

#include "landscape/GridMesh.h"

#include "base/std/container/vector.h"
#include "core/assets/RenderingSubMesh.h"
#include "landscape/LandscapeConfig.h"
#include "renderer/gfx-base/GFXBuffer.h"
#include "renderer/gfx-base/GFXDef-common.h"
#include "renderer/gfx-base/GFXDevice.h"

namespace cc {
namespace landscape {

RenderingSubMesh *GridMesh::create(gfx::Device *device) {
    const int n = config::VERTS_PER_GRID_SIDE;
    const float step = 1.0F / static_cast<float>(n - 1);

    // Vertices normalized to [0,1] x [0,1] on the XZ plane (y = 0).
    ccstd::vector<float> verts;
    verts.reserve(static_cast<size_t>(n) * n * 3);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            verts.push_back(static_cast<float>(x) * step);
            verts.push_back(0.0F);
            verts.push_back(static_cast<float>(z) * step);
        }
    }
    const auto vbSize = static_cast<uint32_t>(verts.size() * sizeof(float));
    auto *vertexBuffer = device->createBuffer({
        gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
        gfx::MemoryUsageBit::DEVICE,
        vbSize,
        static_cast<uint32_t>(3 * sizeof(float)),
    });
    vertexBuffer->update(verts.data(), vbSize);

    // Triangle-list indices for the (n-1) x (n-1) quads.
    ccstd::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(n - 1) * (n - 1) * 6);
    for (int z = 0; z < n - 1; ++z) {
        for (int x = 0; x < n - 1; ++x) {
            const auto a = static_cast<uint16_t>(z * n + x);
            const auto b = static_cast<uint16_t>(z * n + x + 1);
            const auto c = static_cast<uint16_t>((z + 1) * n + x);
            const auto d = static_cast<uint16_t>((z + 1) * n + x + 1);
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }
    const auto ibSize = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
    auto *indexBuffer = device->createBuffer({
        gfx::BufferUsageBit::INDEX | gfx::BufferUsageBit::TRANSFER_DST,
        gfx::MemoryUsageBit::DEVICE,
        ibSize,
        static_cast<uint32_t>(sizeof(uint16_t)),
    });
    indexBuffer->update(indices.data(), ibSize);

    gfx::BufferList vertexBuffers;
    vertexBuffers.emplace_back(vertexBuffer);
    ccstd::vector<gfx::Attribute> attributes{
        gfx::Attribute{gfx::ATTR_NAME_POSITION, gfx::Format::RGB32F},
    };
    auto *mesh = ccnew RenderingSubMesh(vertexBuffers, attributes, gfx::PrimitiveMode::TRIANGLE_LIST, indexBuffer);
    mesh->setSubMeshIdx(0);
    return mesh;
}

} // namespace landscape
} // namespace cc

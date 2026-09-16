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

#include <cstdint>

namespace cc {

class RenderingSubMesh;
namespace gfx {
class Device;
} // namespace gfx

namespace landscape {

/**
 * Shared grids with 1, 2, 4 or 8 cells per side. Material-page patches retain
 * the original CDLOD cell spacing and triangles while batching by grid size.
 * The shader restores parent-node coordinates before applying morph.
 */
class GridMesh {
public:
    // Creates the shared grid RenderingSubMesh (RG32F XZ position, triangle list).
    // Returns a new object; the caller takes ownership (e.g. via IntrusivePtr).
    static RenderingSubMesh *create(gfx::Device *device, uint32_t cells = 8);
    // An RG32F [0,1] XZ quad (4 vertices, 6 indices) for VT page rendering.
    static RenderingSubMesh *createVTQuad(gfx::Device *device);
};

} // namespace landscape
} // namespace cc

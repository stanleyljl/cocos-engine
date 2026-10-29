// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import { RenderingSubMesh } from '../asset/assets/rendering-sub-mesh';
import { Attribute, AttributeName, BufferInfo, BufferUsageBit, Device, Format, MemoryUsageBit, PrimitiveMode } from '../gfx';
import * as config from './Config';

export class GridMesh {
    public static create (device: Device, cells = config.VERTS_PER_NODE_SIDE - 1): RenderingSubMesh {
        const n = cells + 1;
        const vertices = new Float32Array(n * n * 2);
        for (let z = 0; z < n; ++z) {
            for (let x = 0; x < n; ++x) {
                const i = (z * n + x) * 2;
                vertices[i] = x / cells;
                vertices[i + 1] = z / cells;
            }
        }
        const indices = new Uint16Array(cells * cells * 6);
        let offset = 0;
        for (let z = 0; z < cells; ++z) {
            for (let x = 0; x < cells; ++x) {
                const a = z * n + x;
                indices.set([a, a + n, a + 1, a + 1, a + n, a + n + 1], offset);
                offset += 6;
            }
        }
        const vb = device.createBuffer(new BufferInfo(BufferUsageBit.VERTEX | BufferUsageBit.TRANSFER_DST, MemoryUsageBit.DEVICE, vertices.byteLength, 8));
        const ib = device.createBuffer(new BufferInfo(BufferUsageBit.INDEX | BufferUsageBit.TRANSFER_DST, MemoryUsageBit.DEVICE, indices.byteLength, 2));
        vb.update(vertices);
        ib.update(indices);
        return new RenderingSubMesh([vb], [new Attribute(AttributeName.ATTR_POSITION, Format.RG32F)], PrimitiveMode.TRIANGLE_LIST, ib);
    }

    public static createVTQuad (device: Device): RenderingSubMesh {
        return GridMesh.create(device, 1);
    }
}

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import downloader from '../asset/asset-manager/downloader';
import zlib from '../../external/compression/zlib.min';

/** CPU-only description of the finest source grid. */
export interface LandscapeHeightLayout {
    resolution: number;
    tilesX: number;
    tilesZ: number;
    tileSize: number;
    heightScale: number;
    heightBias: number;
    level: number;
    files: Record<string, string>;
}

export function parseLandscapeHeightLayout (manifest: any): LandscapeHeightLayout {
    const { sectorCount, maxLevel, minTileLevel, nodeTileResolution, sectorSizeMeters, heightScale, heightBias, files } = manifest;
    if (!Array.isArray(sectorCount) || sectorCount.length !== 2
        || !sectorCount.every((v: number) => Number.isInteger(v) && v > 0)
        || !Number.isInteger(maxLevel) || maxLevel < 0 || maxLevel > 8
        || !Number.isInteger(minTileLevel) || minTileLevel < 0 || minTileLevel > maxLevel
        || !Number.isInteger(nodeTileResolution) || nodeTileResolution < 2 || nodeTileResolution > 1025
        || !Number.isFinite(sectorSizeMeters) || sectorSizeMeters <= 0
        || !Number.isFinite(heightScale) || heightScale <= 0 || !Number.isFinite(heightBias)
        || !files || typeof files !== 'object') throw new Error('Invalid Landscape height manifest');
    const side = 2 ** (maxLevel - minTileLevel);
    const tilesX = sectorCount[0] * side; const tilesZ = sectorCount[1] * side;
    if (tilesX * tilesZ > 1000000) throw new Error('Landscape tile count exceeds limit');
    return { resolution: nodeTileResolution, tilesX, tilesZ, tileSize: sectorSizeMeters / side,
        heightScale, heightBias, level: minTileLevel, files };
}

/** Decode grayscale16 directly; browser image/canvas decoding would lose precision. */
export function decodeLandscapeHeight (buffer: ArrayBuffer, resolution: number): Uint16Array {
    const bytes = new Uint8Array(buffer); const view = new DataView(buffer);
    if (bytes.length < 45 || view.getUint32(0) !== 0x89504e47 || view.getUint32(4) !== 0x0d0a1a0a
        || view.getUint32(8) !== 13 || view.getUint32(12) !== 0x49484452
        || view.getUint32(16) !== resolution || view.getUint32(20) !== resolution
        || bytes[24] !== 16 || bytes[25] !== 0 || bytes[26] !== 0 || bytes[27] !== 0 || bytes[28] !== 0) {
        throw new Error('Landscape height requires non-interlaced grayscale16 PNG');
    }
    const chunks: Uint8Array[] = []; let size = 0; let ended = false;
    for (let offset = 8; offset + 12 <= bytes.length;) {
        const length = view.getUint32(offset); const type = view.getUint32(offset + 4);
        if (length > bytes.length - offset - 12) throw new Error('Truncated Landscape PNG');
        if (type === 0x49444154) { chunks.push(bytes.subarray(offset + 8, offset + 8 + length)); size += length; }
        offset += length + 12;
        if (type === 0x49454e44) { ended = true; break; }
    }
    if (!ended || !size) throw new Error('Incomplete Landscape PNG');
    const packed = new Uint8Array(size); let offset = 0;
    for (const chunk of chunks) { packed.set(chunk, offset); offset += chunk.length; }
    const decoded = new zlib.Inflate(packed, { verify: true }).decompress() as Uint8Array;
    const stride = resolution * 2;
    if (decoded.length !== (stride + 1) * resolution) throw new Error('Invalid Landscape PNG scanline size');
    const pixels = new Uint8Array(stride * resolution);
    let input = 0;
    for (let row = 0; row < resolution; ++row) {
        const filter = decoded[input++];
        if (filter > 4) throw new Error('Invalid Landscape PNG filter');
        for (let col = 0; col < stride; ++col) {
            const at = row * stride + col;
            const a = col >= 2 ? pixels[at - 2] : 0;
            const b = row ? pixels[at - stride] : 0;
            const c = row && col >= 2 ? pixels[at - stride - 2] : 0;
            const p = a + b - c;
            const pa = Math.abs(p - a); const pb = Math.abs(p - b); const pc = Math.abs(p - c);
            const prediction = filter === 1 ? a : filter === 2 ? b : filter === 3 ? (a + b) >>> 1
                : filter === 4 ? (pa <= pb && pa <= pc ? a : pb <= pc ? b : c) : 0;
            pixels[at] = decoded[input++] + prediction;
        }
    }
    const heights = new Uint16Array(resolution * resolution);
    for (let i = 0; i < heights.length; ++i) heights[i] = pixels[i * 2] * 256 + pixels[i * 2 + 1];
    return heights;
}

/** Raw downloader deliberately bypasses AssetManager's permanent file cache. */
export class LandscapeHeightLoader {
    public async loadLayout (url: string): Promise<LandscapeHeightLayout> {
        const manifest = await new Promise<any>((resolve, reject) => {
            downloader._downloadJson(url, {}, (err, data) => err ? reject(err) : resolve(data));
        });
        return parseLandscapeHeightLayout(manifest);
    }

    public async loadTile (url: string, layout: LandscapeHeightLayout, x: number, z: number): Promise<Uint16Array> {
        const suffix = layout.files[`nodes/L${layout.level}/h_${x}_${z}.png`];
        if (!/^\.lsraw\d+$/.test(suffix)) throw new Error(`Missing Landscape height tile ${x},${z}`);
        const [path, query] = url.split('?');
        if (!path.endsWith('.lsmanifest')) throw new Error('Landscape physics requires an imported manifest');
        const tileURL = path.slice(0, -11) + suffix + (query ? `?${query}` : '');
        const bytes = await new Promise<ArrayBuffer>((resolve, reject) => {
            downloader._downloadArrayBuffer(tileURL, {}, (err, data) => err ? reject(err) : resolve(data));
        });
        return decodeLandscapeHeight(bytes, layout.resolution);
    }
}

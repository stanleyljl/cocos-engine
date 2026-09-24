import { deflateSync } from 'zlib';
import { decodeLandscapeHeight, parseLandscapeHeightLayout, LandscapeHeightLoader } from '../../cocos/landscape/landscape-height-data';
import downloader from '../../cocos/asset/asset-manager/downloader';

function png (filter: number) {
    const resolution = 3;
    const expected = [0, 1, 255, 256, 32767, 32768, 40000, 65534, 65535];
    const pixels = Buffer.alloc(18); expected.forEach((v, i) => pixels.writeUInt16BE(v, i * 2));
    const scan = Buffer.alloc(21);
    for (let row = 0; row < 3; ++row) {
        scan[row * 7] = filter;
        for (let col = 0; col < 6; ++col) {
            const at = row * 6 + col;
            const a = col >= 2 ? pixels[at - 2] : 0;
            const b = row ? pixels[at - 6] : 0;
            const c = row && col >= 2 ? pixels[at - 8] : 0;
            const p = a + b - c; const distances = [Math.abs(p - a), Math.abs(p - b), Math.abs(p - c)];
            const pred = filter === 1 ? a : filter === 2 ? b : filter === 3 ? Math.floor((a + b) / 2)
                : filter === 4 ? [a, b, c][distances.indexOf(Math.min(...distances))] : 0;
            scan[row * 7 + col + 1] = (pixels[at] - pred) & 255;
        }
    }
    function chunk (name: string, data: Buffer) {
        const result = Buffer.alloc(data.length + 12); result.writeUInt32BE(data.length);
        result.write(name, 4); data.copy(result, 8); return result;
    }
    const header = Buffer.alloc(13); header.writeUInt32BE(resolution); header.writeUInt32BE(resolution, 4); header[8] = 16;
    const packed = deflateSync(scan); const split = Math.floor(packed.length / 2);
    const file = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header),
        chunk('IDAT', packed.subarray(0, split)), chunk('IDAT', packed.subarray(split)), chunk('IEND', Buffer.alloc(0))]);
    return { buffer: file.buffer.slice(file.byteOffset, file.byteOffset + file.length), expected };
}

test.each([0, 1, 2, 3, 4])('16-bit PNG filter %i preserves low bits and signed-boundary samples', filter => {
    const fixture = png(filter);
    expect([...decodeLandscapeHeight(fixture.buffer, 3)]).toEqual(fixture.expected);
    expect(() => decodeLandscapeHeight(fixture.buffer, 129)).toThrow();
    expect(() => decodeLandscapeHeight(fixture.buffer.slice(0, -12), 3)).toThrow();
});

test('finest tile layout and imported URLs retain content hash and query', async () => {
    const manifest = { sectorCount: [2, 2], maxLevel: 7, minTileLevel: 3, nodeTileResolution: 129,
        sectorSizeMeters: 2048, heightScale: 3000, heightBias: 0, files: { 'nodes/L3/h_0_0.png': '.lsraw000042' } };
    const layout = parseLandscapeHeightLayout(manifest);
    expect([layout.tilesX, layout.tilesZ, layout.tileSize, layout.resolution]).toEqual([32, 32, 128, 129]);
    const fixture = png(4); layout.resolution = 3;
    const load = jest.spyOn(downloader, '_downloadArrayBuffer').mockImplementation((url, options, done) => {
        done(null, fixture.buffer); return null as any;
    });
    try {
        const samples = await new LandscapeHeightLoader().loadTile('/assets/aa/uuid.hash.lsmanifest?v=1', layout, 0, 0);
        expect([...samples]).toEqual(fixture.expected);
        expect(load.mock.calls[0][0]).toBe('/assets/aa/uuid.hash.lsraw000042?v=1');
    } finally { load.mockRestore(); }
    expect(() => parseLandscapeHeightLayout({ ...manifest, heightScale: 0 })).toThrow();
});

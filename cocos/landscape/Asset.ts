// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
import type { LandscapeQueryTile, LandscapeQueryCompletion } from './Data';
import downloader from '../asset/asset-manager/downloader';
import { error, log } from '../core/platform/debug';
import zlib from '../../external/compression/zlib.min';
import { decodeLandscapeHeight } from './landscape-height-data';
import { HeightRange, LandscapeData, NodeIndexLayout, NodeKey, makeNodeKey } from './Data';
import * as config from './Config';

export interface MaterialLayer {
    id: number;
    name: string;
    albedoHeight: string;
    normalRoughnessAO: string;
    pixelsPerMeter: number;
    globalColorInfluence: number;
}

export interface DecalLayer {
    albedoAlpha: string;
    normalRoughnessAO: string;
    height: string;
    heightScale: number;
    modulateColor: boolean;
    detailMaterial0: number;
    detailMaterial1: number;
}

export interface Decal {
    x: number;
    z: number;
    size: number;
    layer: number;
}

export interface TileData {
    key: NodeKey;
    height: Uint8Array;
    splat: Uint16Array;
    normal: Uint8Array;
    minimumNormalY: number;
}


function requireValue (condition: unknown, message: string): asserts condition {
    if (!condition) {
        throw new Error(`[Landscape] ${message}`);
    }
}

function unsigned (value: unknown): value is number {
    return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 0xFFFFFFFF;
}

function number (value: unknown): value is number {
    return typeof value === 'number';
}

// Decode data PNGs without browser color conversion, alpha premultiplication or
// canvas round-trips. The importer writes non-interlaced RGB8/RGBA8 PNGs.
function decodeColor (buffer: ArrayBuffer, resolution: number, channels: 3 | 4): Uint8Array {
    const bytes = new Uint8Array(buffer);
    const view = new DataView(buffer);
    requireValue(bytes.length >= 45 && view.getUint32(0) === 0x89504E47 && view.getUint32(4) === 0x0D0A1A0A
        && view.getUint32(8) === 13 && view.getUint32(12) === 0x49484452
        && view.getUint32(16) === resolution && view.getUint32(20) === resolution
        && bytes[24] === 8 && bytes[25] === (channels === 3 ? 2 : 6)
        && bytes[26] === 0 && bytes[27] === 0 && bytes[28] === 0, 'Invalid color PNG');
    const chunks: Uint8Array[] = [];
    let total = 0;
    let ended = false;
    for (let offset = 8; offset + 12 <= bytes.length;) {
        const size = view.getUint32(offset);
        const type = view.getUint32(offset + 4);
        requireValue(size <= bytes.length - offset - 12, 'Truncated color PNG');
        if (type === 0x49444154) {
            chunks.push(bytes.subarray(offset + 8, offset + 8 + size));
            total += size;
        }
        offset += size + 12;
        if (type === 0x49454E44) {
            ended = true;
            break;
        }
    }
    requireValue(ended && total > 0, 'Incomplete color PNG');
    const packed = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
        packed.set(chunk, offset);
        offset += chunk.length;
    }
    const decoded = new zlib.Inflate(packed, { verify: true }).decompress() as Uint8Array;
    const stride = resolution * channels;
    requireValue(decoded.length === (stride + 1) * resolution, 'Invalid color PNG scanlines');
    const result = new Uint8Array(stride * resolution);
    for (let y = 0; y < resolution; ++y) {
        const row = y * stride;
        const input = y * (stride + 1);
        const filter = decoded[input];
        requireValue(filter <= 4, 'Invalid PNG filter');
        for (let x = 0; x < stride; ++x) {
            const a = x >= channels ? result[row + x - channels] : 0;
            const b = y > 0 ? result[row + x - stride] : 0;
            const c = y > 0 && x >= channels ? result[row + x - stride - channels] : 0;
            let prediction = 0;
            if (filter === 1) {
                prediction = a;
            } else if (filter === 2) {
                prediction = b;
            } else if (filter === 3) {
                prediction = Math.floor((a + b) / 2);
            } else if (filter === 4) {
                const p = a + b - c;
                const pa = Math.abs(p - a);
                const pb = Math.abs(p - b);
                const pc = Math.abs(p - c);
                prediction = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            result[row + x] = decoded[input + 1 + x] + prediction;
        }
    }
    return result;
}

// Temporary parse state, matching native LandscapeAsset::Manifest. Publish only
// after every section validates; URL resolution remains local to the Web runtime.
class Manifest {
    public readonly parsed = new LandscapeData();
    public nodeIndexLayout = new NodeIndexLayout();
    public ranges: HeightRange[] = [];
    public assetDir = '';
    public files = new Map<string, string>();
    public materialLayers: MaterialLayer[] = [];
    public materialResolution = 0;
    public decalLayers: DecalLayer[] = [];
    public decals: Decal[] = [];
    public decalResolution = 1;
    public decalNearDistance = 12;
    public decalFarDistance = 40;
    private readonly document: any;
    private readonly manifestPath: string;

    constructor (document: any, manifestPath: string) {
        this.document = document;
        this.manifestPath = manifestPath;
    }

    public read (): void {
        this.readDimensions();
        this.readHeightRanges();
        this.readFileIndex();
        this.readMaterials();
        this.readDecalLayers();
        this.readDecals();
    }

    public resolve (logical: string): string {
        const result = this.files.size ? this.files.get(logical) : this.assetDir + logical;
        requireValue(result, `Missing resource ${logical}`);
        return result;
    }

    private readDimensions (): void {
        const manifest = this.document;
        requireValue(manifest && Array.isArray(manifest.sectorCount) && manifest.sectorCount.length === 2, 'Missing sector dimensions');
        const data = this.parsed;
        data.sectorsX = manifest.sectorCount[0];
        data.sectorsZ = manifest.sectorCount[1];
        data.maxLevel = manifest.maxLevel;
        data.minTileLevel = manifest.minTileLevel;
        data.sectorSize = manifest.sectorSizeMeters;
        data.heightScale = manifest.heightScale;
        data.heightBias = manifest.heightBias;
        requireValue(data.valid() && number(data.sectorSize) && number(data.heightScale) && number(data.heightBias), 'Invalid dimensions');
    }

    private readHeightRanges (): void {
        const manifest = this.document;
        const data = this.parsed;
        requireValue(Array.isArray(manifest.levels) && manifest.levels.length === data.maxLevel + 1, 'Invalid height range levels');
        this.nodeIndexLayout = new NodeIndexLayout(data);
        this.ranges = new Array(this.nodeIndexLayout.nodeCount());
        for (let level = 0; level <= data.maxLevel; ++level) {
            this.readHeightRangeLevel(level, manifest.levels[level]);
        }
    }

    private readFileIndex (): void {
        const manifest = this.document;
        const url = this.manifestPath;
        const queryStart = url.search(/[?#]/);
        const path = queryStart < 0 ? url : url.slice(0, queryStart);
        const query = queryStart < 0 ? '' : url.slice(queryStart);
        this.assetDir = path.slice(0, path.lastIndexOf('/') + 1);
        this.files = new Map();
        if (path.endsWith('.lsmanifest')) {
            requireValue(manifest.files && typeof manifest.files === 'object' && Object.keys(manifest.files).length > 0, 'Missing raw file index');
            for (const logical of Object.keys(manifest.files)) {
                const suffix = manifest.files[logical];
                requireValue(typeof suffix === 'string' && /^\.lsraw\d+$/.test(suffix), 'Invalid raw file extension');
                this.files.set(logical, path.slice(0, -11) + suffix + query);
            }
        }
    }

    private readMaterials (): void {
        const manifest = this.document;
        const library = manifest.materialLibrary;
        requireValue(library && unsigned(library.resolution) && library.resolution > 0 && typeof library.dir === 'string'
            && Array.isArray(library.layers) && library.layers.length > 0 && library.layers.length <= config.MATERIAL_LIBRARY_MAX, 'Invalid material library');
        this.materialLayers = library.layers.map((layer: any, id: number) => {
            requireValue(layer && typeof layer.name === 'string' && typeof layer.albedoHeight === 'string'
                && typeof layer.normalRoughnessAO === 'string' && number(layer.pixelsPerMeter) && layer.pixelsPerMeter > 0
                && number(layer.globalColorInfluence) && layer.globalColorInfluence >= 0 && layer.globalColorInfluence <= 1, 'Invalid material layer');
            return {
                id,
                name: layer.name,
                pixelsPerMeter: layer.pixelsPerMeter,
                globalColorInfluence: layer.globalColorInfluence,
                albedoHeight: this.resolve(`${library.dir}/${layer.albedoHeight}`),
                normalRoughnessAO: this.resolve(`${library.dir}/${layer.normalRoughnessAO}`),
            };
        });
        this.materialResolution = library.resolution;
    }

    private readDecalLayers (): void {
        const manifest = this.document;
        if (manifest.decalLibrary !== undefined) {
            const dl = manifest.decalLibrary;
            requireValue(dl && number(dl.nearDistance) && number(dl.farDistance) && dl.nearDistance >= 0 && dl.farDistance > dl.nearDistance
                && unsigned(dl.resolution) && dl.resolution >= 2 && dl.resolution <= 2048 && (dl.resolution & (dl.resolution - 1)) === 0
                && Array.isArray(dl.layers) && dl.layers.length > 0 && dl.layers.length <= config.DECAL_LIBRARY_MAX, 'Invalid decal library');
            this.decalResolution = dl.resolution;
            this.decalNearDistance = dl.nearDistance;
            this.decalFarDistance = dl.farDistance;
            for (const layer of dl.layers) {
                requireValue(layer && typeof layer.albedoAlpha === 'string' && typeof layer.normalRoughnessAO === 'string'
                    && typeof layer.height === 'string' && number(layer.heightScale) && layer.heightScale >= 0 && layer.heightScale <= 10
                    && (layer.modulateColor === undefined || typeof layer.modulateColor === 'boolean'), 'Invalid decal layer');
                const ids = layer.detailMaterials;
                requireValue(ids === undefined || (Array.isArray(ids) && ids.length === 2 && layer.heightScale === 0 && !layer.modulateColor
                    && ids.every((id) => unsigned(id) && id < this.materialLayers.length)), 'Invalid decal detail materials');
                this.decalLayers.push({
                    albedoAlpha: this.resolve(layer.albedoAlpha),
                    normalRoughnessAO: this.resolve(layer.normalRoughnessAO),
                    height: this.resolve(layer.height),
                    heightScale: layer.heightScale,
                    modulateColor: layer.modulateColor === true,
                    detailMaterial0: ids === undefined ? -1 : ids[0],
                    detailMaterial1: ids === undefined ? -1 : ids[1],
                });
            }
        }
    }

    private readDecals (): void {
        const manifest = this.document;
        const data = this.parsed;
        if (manifest.decals !== undefined) {
            requireValue(Array.isArray(manifest.decals) && manifest.decals.length <= config.DECAL_INSTANCE_MAX, 'Invalid decal instances');
            for (const d of manifest.decals) {
                requireValue(d && number(d.x) && number(d.z) && number(d.size) && unsigned(d.layer) && d.layer < this.decalLayers.length
                    && d.size > 0 && d.size <= 64 && d.x >= 0 && d.z >= 0 && d.x + d.size <= data.width() && d.z + d.size <= data.depth(), 'Invalid decal region');
                this.decals.push({ x: d.x, z: d.z, size: d.size, layer: d.layer });
            }
        }
    }

    private readHeightRangeLevel (level: number, levelValue: any): void {
        const data = this.parsed;
        const range = levelValue.heightRange;
        const width = data.nodesX(level);
        const depth = data.nodesZ(level);
        requireValue(range && Array.isArray(range.min) && Array.isArray(range.max)
            && range.min.length === width * depth && range.max.length === width * depth, 'Invalid height range size');
        for (let z = 0; z < depth; ++z) {
            for (let x = 0; x < width; ++x) {
                const index = z * width + x;
                requireValue(number(range.min[index]) && number(range.max[index]), 'Invalid height range');
                this.ranges[this.nodeIndexLayout.globalNodeIndex(level, x, z)] = {
                    minY: data.heightBias + range.min[index] * data.heightScale / 65535,
                    maxY: data.heightBias + range.max[index] * data.heightScale / 65535,
                };
            }
        }
    }
}

export class LandscapeAsset {
    private _data = new LandscapeData();
    private _heightRanges: HeightRange[] = [];
    private _nodeIndexLayout = new NodeIndexLayout();
    private _files = new Map<string, string>();
    private _dataDir = '';
    private _materialLayers: MaterialLayer[] = [];
    private _materialResolution = 0;
    private _decalLayers: DecalLayer[] = [];
    private _decals: Decal[] = [];
    private _decalResolution = 1;
    private _decalNearDistance = 12;
    private _decalFarDistance = 40;
    private _ready: TileData[] = [];
    private _loadFailed = false;
    private _generation = 0;

    public data (): LandscapeData {
        return this._data;
    }
    public heightRanges (): readonly HeightRange[] {
        return this._heightRanges;
    }
    public materialLayers (): readonly MaterialLayer[] {
        return this._materialLayers;
    }
    public materialResolution (): number {
        return this._materialResolution;
    }
    public decalLayers (): readonly DecalLayer[] {
        return this._decalLayers;
    }
    public decals (): readonly Decal[] {
        return this._decals;
    }
    public decalResolution (): number {
        return this._decalResolution;
    }
    public decalNearDistance (): number {
        return this._decalNearDistance;
    }
    public decalFarDistance (): number {
        return this._decalFarDistance;
    }
    public loadingFailed (): boolean {
        return this._loadFailed;
    }

    public destroy (): void {
        ++this._generation;
        this._ready.length = 0;
    }

    public async load (url: string): Promise<boolean> {
        this.destroy();
        this._loadFailed = false;
        const generation = this._generation;
        try {
            const manifest = await new Promise<any>((resolve, reject) => {
                downloader._downloadJson(url, {}, (err, value) => err ? reject(err) : resolve(value));
            });
            if (generation !== this._generation) {
                return false;
            }
            // The runtime owner publishes this asset only after all sections validate.
            const parsed = new Manifest(manifest, url);
            parsed.read();
            this._data = parsed.parsed;
            this._dataDir = parsed.assetDir;
            this._files = parsed.files;
            this._materialLayers = parsed.materialLayers;
            this._materialResolution = parsed.materialResolution;
            this._decalLayers = parsed.decalLayers;
            this._decals = parsed.decals;
            this._decalResolution = parsed.decalResolution;
            this._decalNearDistance = parsed.decalNearDistance;
            this._decalFarDistance = parsed.decalFarDistance;
            this._nodeIndexLayout = parsed.nodeIndexLayout;
            this._heightRanges = parsed.ranges;
            if (config.LANDSCAPE_DEBUG) {
                log(`[Landscape] Asset: sectors=${this._data.sectorsX}x${this._data.sectorsZ} levels=${this._data.maxLevel + 1} minTileLevel=${this._data.minTileLevel} materials=${this._materialLayers.length} decals=${this._decals.length}`);
            }
            return true;
        } catch (cause) {
            if (generation === this._generation) {
                this._loadFailed = true;
                error(`[Landscape] Cannot load ${url}: ${String(cause)}`);
            }
            return false;
        }
    }

    public resolveFile (logical: string): string {
        const result = this._files.size ? this._files.get(logical) : this._dataDir + logical;
        requireValue(result, `Missing resource ${logical}`);
        return result;
    }

    public static async loadBytes (url: string): Promise<ArrayBuffer> {
        return new Promise<ArrayBuffer>((resolve, reject) => {
            downloader._downloadArrayBuffer(url, {}, (err, value) => err ? reject(err) : resolve(value));
        });
    }

    public static async loadColor (url: string, resolution: number, channels: 3 | 4): Promise<Uint8Array> {
        return decodeColor(await LandscapeAsset.loadBytes(url), resolution, channels);
    }

    private async loadTileSet (level: number, x: number, z: number): Promise<LandscapeQueryTile> {
        const prefix = `nodes/L${level}/`;
        const suffix = `${x}_${z}.png`;
        const [heightBytes, splatBytes, normal] = await Promise.all([
            LandscapeAsset.loadBytes(this.resolveFile(`${prefix}h_${suffix}`)),
            LandscapeAsset.loadBytes(this.resolveFile(`${prefix}s_${suffix}`)),
            LandscapeAsset.loadColor(this.resolveFile(`${prefix}n_${suffix}`), config.SOURCE_NORMAL_RESOLUTION, 3),
        ]);
        const samples = decodeLandscapeHeight(heightBytes, config.SOURCE_TILE_RESOLUTION);
        const height = new Uint8Array(samples.length * 2);
        for (let i = 0; i < samples.length; ++i) {
            height[i * 2] = samples[i] >> 8;
            height[i * 2 + 1] = samples[i] & 255;
        }
        return { height, splat: decodeLandscapeHeight(splatBytes, config.SOURCE_TILE_RESOLUTION), normal };
    }

    public requestTile (level: number, x: number, z: number): boolean {
        if (this._loadFailed || level < this._data.minTileLevel || level > this._data.maxLevel) {
            return false;
        }
        const generation = this._generation;
        void this.loadTileSet(level, x, z).then((tile) => {
            if (generation !== this._generation || this._loadFailed) {
                return;
            }
            const count = config.SOURCE_NORMAL_RESOLUTION ** 2;
            let minimumNormalY = 1;
            for (let i = 0; i < count; ++i) {
                minimumNormalY = Math.min(minimumNormalY, Math.max(0, tile.normal[i * 3 + 1] / 127.5 - 1));
                tile.normal[i * 2] = tile.normal[i * 3];
                tile.normal[i * 2 + 1] = tile.normal[i * 3 + 2];
            }
            this._ready.push({ ...tile, normal: tile.normal.subarray(0, count * 2), key: makeNodeKey(level, x, z), minimumNormalY });
        }).catch((cause) => {
            if (generation === this._generation) {
                this._loadFailed = true;
                this._ready.length = 0;
                error(`[Landscape] Tile L${level}/${x}/${z}: ${String(cause)}`);
            }
        });
        return true;
    }

    public takeReadyTile (): TileData | undefined {
        return this._loadFailed ? undefined : this._ready.shift();
    }

    public requestQueryTile (x: number, z: number, completion: LandscapeQueryCompletion): void {
        if (x < 0 || z < 0 || x >= this._data.nodesX(this._data.minTileLevel) || z >= this._data.nodesZ(this._data.minTileLevel)) {
            completion(null);
            return;
        }
        const generation = this._generation;
        void this.loadTileSet(this._data.minTileLevel, x, z).then((tile) => {
            if (generation === this._generation) {
                completion(tile);
            }
        }).catch(() => {
            if (generation === this._generation) {
                completion(null);
            }
        });
    }

    public getHeightRange (level: number, x: number, z: number): HeightRange | undefined {
        return this._heightRanges[this._nodeIndexLayout.globalNodeIndex(level, x, z)];
    }
}

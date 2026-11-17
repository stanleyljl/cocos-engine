import * as config from '../../cocos/landscape/Config';
import { Vec3 } from '../../cocos/core/math';
import type { LandscapeAsset } from '../../cocos/landscape/Asset';
import { HeightRange, LandscapeData, NodeAddress, VTGridMapping, VTPageAddress, VTPageLayout, VTPageSources } from '../../cocos/landscape/Data';
import { LandscapeResidency } from '../../cocos/landscape/Residency';
import { Quadtree } from '../../cocos/landscape/Quadtree';
import { PageState, VTPage, VTCapacityQuery, VTRenderer } from '../../cocos/landscape/VTRenderer';

function requireCondition (condition: boolean, message: string): void {
    if (!condition) {
        throw new Error(message);
    }
}

function cacheData (level: number, sectorsX: number, sectorsZ: number): LandscapeData {
    return Object.assign(new LandscapeData(), { maxTileLevel: level, sectorSize: 2 ** level, sectorsX, sectorsZ, heightScale: 1 });
}

function mapping (address: NodeAddress): VTGridMapping {
    const result = new VTGridMapping();
    result.pages = [new VTPageAddress(address.level, address.x, address.z)];
    return result;
}

// Match the native CPU fixture. Supply only CPU state; accidental GPU calls fail.
// Demand reconciliation, eviction, lookup and invalidation run production methods.
function pageFixture (data: LandscapeData, capacity: number): { renderer: VTRenderer; reserve: (addresses: VTPageAddress[]) => boolean; slots: () => number[]; pages: () => VTPage[] } {
    const state = {
        _data: data,
        _heightRanges: Array.from({ length: data.maxTileLevel + 1 }, (_, level) => Array.from({ length: data.nodesX(level) * data.nodesZ(level) }, () => ({ minY: 0, maxY: data.sectorSize * 2 }))),
        _layout: new VTPageLayout(data.sectorSize),
        _pages: Array.from({ length: capacity }, () => new VTPage()),
        _reservedSlots: [] as number[],
        _lookup: new Map<string, number>(),
        _reservation: 0,
        _requests: new Map(),
        _requestsDirty: false,
    };
    const renderer: VTRenderer = Object.assign(Object.create(VTRenderer.prototype), state);
    const cpu = renderer as unknown as typeof state;
    let demands: VTPageAddress[] = [];
    const reserve = (addresses: VTPageAddress[]): boolean => {
        if (!renderer.canFit(addresses)) {
            return false;
        }
        demands.forEach((address) => renderer.release(address));
        addresses.forEach((address) => renderer.request(address));
        demands = addresses;
        return renderer['applyRequests']();
    };
    return { renderer, reserve, slots: () => cpu._reservedSlots, pages: () => cpu._pages };
}

test('reused page sources still check capacity against the current retained set', () => {
    const first = new NodeAddress(0, 0, 0);
    const second = new NodeAddress(0, 1, 0);
    const input = new NodeAddress(0, 2, 0);
    const page = new VTPageAddress(0, 0, 0);
    const selectSources = jest.fn(() => {
        const result = new VTPageSources();
        result.splat = input;
        result.normals.fill(input);
        return result;
    });
    const residency = {
        canFit: (sources: NodeAddress[]): boolean => sources.length <= 2,
        layerCount: (): number => 2,
    } as LandscapeResidency;
    const pages = {
        canFit: (): boolean => true,
        selectSources,
    } as unknown as VTRenderer;
    const capacity = new VTCapacityQuery(pages, residency);
    expect(capacity.canPrepare([page], [first])).toBe(true);
    // The old page must now be rejected, although its source list is reusable.
    expect(capacity.canPrepare([page], [first, second])).toBe(false);
    // A retained source can also satisfy the page's composition demand.
    expect(capacity.canPrepare([page], [first, input])).toBe(true);
    expect(selectSources).toHaveBeenCalledTimes(1);
    expect(selectSources).toHaveBeenCalledWith(page, true, true);
    expect(new VTCapacityQuery(pages, residency).canPrepare([page], [first])).toBe(true);
    expect(selectSources).toHaveBeenCalledTimes(2);
});

test('VT admission checks per-page headroom without reserving resources', () => {
    const data = cacheData(3, 1, 1);
    const residency = new LandscapeResidency();
    residency['_data'] = data;
    residency['_capacity'] = 2;
    const { renderer, slots } = pageFixture(data, 2);
    renderer['_residency'] = residency;
    const capacity = renderer.capacityQuery();
    const first = new NodeAddress(0, 1, 0);
    const second = new NodeAddress(0, 2, 0);
    const input = new NodeAddress(0, 0, 0);
    const page = new VTPageAddress(0, 0, 0);
    const other = new VTPageAddress(0, 3, 0);
    expect(capacity.canPrepare([page], [first])).toBe(true);
    expect(capacity.canPrepare([page], [first, second])).toBe(false);
    expect(capacity.canPrepare([page], [first, input])).toBe(true);
    // Pages compose in batches: their combined input footprint need not fit at once.
    expect(capacity.canPrepare([page, other], [first])).toBe(true);
    expect(capacity.canPrepare([page, other, new VTPageAddress(0, 4, 0)], [first])).toBe(false);
    expect(capacity.canPrepare([page], [first, second, input])).toBe(false);
    expect(capacity.canPrepare([new VTPageAddress(4, 0, 0)], [first])).toBe(false);
    expect(capacity.canPrepare([page], [new NodeAddress(4, 0, 0)])).toBe(false);
    expect(slots()).toHaveLength(0);
    expect(residency['_requests'].size).toBe(0);
});

test('shared source demands retain uploaded data until the last release', () => {
    const data = cacheData(3, 1, 1);
    const residency = new LandscapeResidency();
    residency['_data'] = data;
    residency['_capacity'] = 1;
    const source = new NodeAddress(3, 0, 0);
    residency.request(source);
    residency.request(source);
    expect(residency.canRequest([source, source])).toBe(true);
    expect(residency['applyRequests']()).toBe(true);
    residency['_slots'].get(source.key())!.ready = true;
    const layer = residency.resident(source);
    residency.release(source);
    expect(residency['applyRequests']()).toBe(true);
    expect(residency.ready()).toBe(true);
    expect(residency.resident(source)).toBe(layer);
    residency.release(source);
    residency.request(source);
    expect(residency['applyRequests']()).toBe(true);
    expect(residency.resident(source)).toBe(layer);
    residency.release(source);
    expect(residency['applyRequests']()).toBe(true);
    expect(residency.resident(source)).toBe(-1);
});

test('shared page demands deduplicate work and survive partial release and same-frame changes', () => {
    const { renderer, slots, pages } = pageFixture(cacheData(3, 1, 1), 1);
    const page = new VTPageAddress(3, 0, 0);
    renderer.request(page);
    renderer.request(page);
    expect(renderer['applyRequests']()).toBe(true);
    expect(slots()).toHaveLength(1);
    pages()[slots()[0]].state = PageState.READY;
    const slot = renderer.resolve(page);
    renderer.release(page);
    expect(renderer['applyRequests']()).toBe(true);
    expect(renderer.ready()).toBe(true);
    expect(renderer.resolve(page)).toBe(slot);
    renderer.release(page);
    renderer.request(page);
    expect(renderer['applyRequests']()).toBe(true);
    expect(renderer.ready()).toBe(true);
    expect(renderer.resolve(page)).toBe(slot);
    renderer.release(page);
    expect(renderer['applyRequests']()).toBe(true);
    expect(renderer.resolve(page)).toBe(-1);
});

test('sector dimensions must be powers of two', () => {
    const data = cacheData(8, 1, 1);
    for (const size of [0.125, 1, 1024, 4096]) {
        data.sectorSize = size;
        expect(data.valid()).toBe(true);
    }
    for (const size of [0, -1, 1.5, 1000]) {
        data.sectorSize = size;
        expect(data.valid()).toBe(false);
    }
});

test('triplanar height sources cover page interiors and gutters across sectors', () => {
    // Same boundary samples as native testCompositionSources; no GPU resources.
    const data = Object.assign(cacheData(7, 2, 2), { sectorSize: 2048 });
    const { renderer } = pageFixture(data, 256);
    const layout = new VTPageLayout(data.sectorSize);
    expect(renderer.selectSources(new VTPageAddress(8, 2, 2), true, false).triplanar).toBe(false);
    const samples = [-8 / 256, 0, 0.25, 0.5, 0.75, 1, 1 + 8 / 256];
    for (let level = 0; level <= layout.rootLevel(); ++level) {
        const size = layout.pageSize(level);
        const last = data.width() / size - 1;
        for (const x of [0, Math.floor(last / 2), last]) {
            for (const z of [0, Math.floor(last / 2), last]) {
                for (const detailed of [false, true]) {
                    const inputs = renderer.selectSources(new VTPageAddress(level, x, z), detailed, true);
                    requireCondition(inputs.triplanar, 'Cliff pages must use triplanar');
                    const region = data.nodeRegion(inputs.splat);
                    const padding = region.size * config.SOURCE_HEIGHT_BORDER / (config.SOURCE_TILE_RESOLUTION - 1);
                    for (const u of samples) {
                        for (const v of samples) {
                            const worldX = (x + u) * size;
                            const worldZ = (z + v) * size;
                            requireCondition(worldX >= region.x - padding && worldX <= region.x + region.size + padding && worldZ >= region.z - padding && worldZ <= region.z + region.size + padding,
                                'Padded height must cover page interiors and all gutter edges and corners');
                        }
                    }
                    const sources: NodeAddress[] = [];
                    inputs.appendTo(sources);
                    expect(new Set(sources.map((source) => source.key())).size).toBe(sources.length);
                    expect(sources.length).toBeLessThanOrEqual(5);
                    const withoutTriplanar: NodeAddress[] = [];
                    renderer.selectSources(new VTPageAddress(level, x, z), detailed, false).appendTo(withoutTriplanar);
                    expect(sources).toEqual(withoutTriplanar);
                }
            }
        }
    }
});

test('CPU cliff estimates use height span and region size including gutters', () => {
    const data = Object.assign(cacheData(3, 2, 1), { sectorSize: 128 });
    const { renderer } = pageFixture(data, 256);
    const ranges = (renderer as unknown as { _heightRanges: HeightRange[][] })._heightRanges;
    for (const levelRanges of ranges) {
        levelRanges.fill({ minY: 100, maxY: 100 });
    }
    // The same height span becomes gentle when averaged over a coarse region.
    for (let level = 0; level <= data.maxTileLevel; ++level) {
        ranges[level][(2 >> level) * data.nodesX(level) + (8 >> level)] = { minY: 100, maxY: 164 };
    }
    const ordinary = new VTPageAddress(3, 0, 0);
    expect(renderer.selectSources(ordinary, true, true).triplanar).toBe(false);
    expect(renderer.selectSources(ordinary, true, true)).toEqual(renderer.selectSources(ordinary, true, false));
    expect(renderer.selectSources(new VTPageAddress(0, 128, 32), true, true).triplanar).toBe(true);
    // Its gutter reaches a cliff across the sector boundary.
    expect(renderer.selectSources(new VTPageAddress(4, 7, 2), true, true).triplanar).toBe(true);
    expect(renderer.selectSources(new VTPageAddress(7, 1, 0), true, true).triplanar).toBe(false);
    expect(renderer.selectSources(new VTPageAddress(7, 1, 0), true, false).triplanar).toBe(false);
});

// Independent cell rasterization checks coverage and every neighboring LOD pair.
// Expected topology and edge masks additionally come from the common native trace.
function checkCut (tree: Quadtree, data: LandscapeData): void {
    const width = data.sectorsX * 2 ** data.maxTileLevel;
    const depth = data.sectorsZ * 2 ** data.maxTileLevel;
    const cells = new Int32Array(width * depth).fill(-1);
    tree.forEachRenderable((node) => {
        const address = node.geometry.address();
        requireCondition(tree.findRenderable(address) === node, 'Draw lookup differs from traversal');
        const side = 2 ** address.level;
        for (let z = address.z * side; z < (address.z + 1) * side; ++z) {
            for (let x = address.x * side; x < (address.x + 1) * side; ++x) {
                requireCondition(cells[z * width + x] === -1, 'Overlapping draws');
                cells[z * width + x] = address.level;
            }
        }
    });
    for (let z = 0; z < depth; ++z) {
        for (let x = 0; x < width; ++x) {
            const level = cells[z * width + x];
            requireCondition(level >= 0, 'Hole in committed coverage');
            requireCondition(x === 0 || Math.abs(level - cells[z * width + x - 1]) <= 1, 'Horizontal 2:1 violation');
            requireCondition(z === 0 || Math.abs(level - cells[(z - 1) * width + x]) <= 1, 'Vertical 2:1 violation');
        }
    }
    tree.forEachRenderable((node) => {
        if (!node.children || !node.children.every((child) => child.ready())) {
            return;
        }
        const address = node.geometry.address();
        const side = 2 ** address.level;
        const x = address.x * side;
        const z = address.z * side;
        let blocked = false;
        for (let offset = 0; offset < side; ++offset) {
            blocked = blocked || (x > 0 && cells[(z + offset) * width + x - 1] > address.level)
                || (x + side < width && cells[(z + offset) * width + x + side] > address.level)
                || (z > 0 && cells[(z - 1) * width + x + offset] > address.level)
                || (z + side < depth && cells[(z + side) * width + x + offset] > address.level);
        }
        requireCondition(blocked, 'Ready detail omitted without a coarser neighbor');
    });
}

function list (entries: string[]): string {
    return entries.sort().map((entry) => `${entry},`).join('');
}

function drawKeys (tree: Quadtree): string[] {
    const keys: string[] = [];
    tree.forEachRenderable((node) => keys.push(node.geometry.address().key()));
    return keys;
}

describe('Landscape shared native/Web CPU paging scenarios', () => {
    test('matches every committed frame, page lifecycle and morph coverage', () => {
        const lines = PARITY_CASES.trimStart().split(/\r?\n/);
        const trace: string[] = [];
        let data = new LandscapeData();
        const tree = new Quadtree();
        let pages: ReturnType<typeof pageFixture>;
        for (const line of lines) {
            if (!line || line.startsWith('#')) {
                continue;
            }
            const [command, ...parameters] = line.split(/\s+/);
            const values = parameters.map(Number);
            trace.push(line);
            if (command === 'tree' || command === 'pages') {
                const [level, sectorsX, sectorsZ, capacity] = values;
                data = cacheData(level, sectorsX, sectorsZ);
                if (command === 'tree') {
                    const ranges = Array.from({ length: data.maxTileLevel + 1 }, (_, level) => Array.from({ length: data.nodesX(level) * data.nodesZ(level) }, () => ({ minY: 0, maxY: 1 })));
                    tree.init({ data: () => data, heightRanges: () => ranges } as unknown as LandscapeAsset);
                    for (let z = 0; z < sectorsZ; ++z) {
                        for (let x = 0; x < sectorsX; ++x) {
                            const address = new NodeAddress(level, x, z);
                            requireCondition(tree.publish(address, mapping(address)), 'Initial sector publication');
                        }
                    }
                    requireCondition(tree.selectRenderables(), 'Initial sector coverage');
                } else {
                    pages = pageFixture(data, capacity);
                }
            } else if (command === 'view') {
                const [x, y, z, frames, budget] = values;
                for (let frame = 0; frame < frames; ++frame) {
                    const previous = tree.requests();
                    tree.clearChangedNodes();
                    tree.updateTarget(new Vec3(x, y, z), Vec3.ZERO);
                    let cancelled = 0;
                    for (const address of previous) {
                        if (!tree.isInTargetTree(address)) {
                            requireCondition(!tree.publish(address, mapping(address)), 'Retired completion resurrected a node');
                            ++cancelled;
                        }
                    }
                    let completed = 0;
                    for (const address of tree.requests()) {
                        if (completed < budget && !tree.find(address)!.ready()) {
                            requireCondition(tree.publish(address, mapping(address)), 'Resource publication');
                            ++completed;
                        }
                    }
                    requireCondition(tree.selectRenderables(), 'Lost sector coverage');
                    checkCut(tree, data);
                    const target: string[] = [];
                    const draw: string[] = [];
                    tree.forEachNode((node) => {
                        if (!node.children) {
                            target.push(node.geometry.address().key());
                        }
                        return true;
                    });
                    tree.forEachRenderable((node) => draw.push(`${node.geometry.address().key()}:${node.edgeMask}`));
                    const requests = tree.requests().map((address) => `${address.key()}:${Number(tree.find(address)!.ready())},`).join('');
                    trace.push(`frame ${cancelled} ${completed} ${Number(tree.renderSelectionChanged())} target=${list(target)} draw=${list(draw)} changed=${list([...tree.changedNodes()])} requests=${requests}`);
                }
            } else if (command === 'replace') {
                const before = drawKeys(tree);
                const address = NodeAddress.fromKey(before[0]);
                const parent = address.ancestor(data.maxTileLevel);
                tree.clearChangedNodes();
                requireCondition(tree.publish(address, mapping(parent)) && tree.selectRenderables(), 'Material replacement');
                expect(drawKeys(tree)).toEqual(before);
                expect(tree.renderSelectionChanged()).toBe(false);
                expect([...tree.changedNodes()]).toEqual([address.key()]);
                trace.push(`material ${address.key()} ${parent.key()}`);
            } else if (command === 'reserve' || command === 'compose' || command === 'invalidate') {
                const renderer = pages!.renderer;
                if (command === 'reserve') {
                    const addresses = Array.from({ length: values[0] }, (_, i) => new VTPageAddress(values[i * 3 + 1], values[i * 3 + 2], values[i * 3 + 3]));
                    trace.push(`accepted ${Number(pages!.reserve(addresses))}`);
                } else if (command === 'compose') {
                    // Simulate completion after GPU submission, as native publishPages does.
                    const updates = pages!.slots().filter((slot) => pages!.pages()[slot].state !== PageState.READY).slice(0, values[0]);
                    for (const slot of updates) {
                        pages!.pages()[slot].state = PageState.READY;
                    }
                } else {
                    renderer.invalidate(values.length ? { x: values[0], z: values[1], size: values[2] } : undefined);
                }
                const slots = pages!.pages().map((page) => `${page.address.key()}:${page.state}:${renderer.resolve(page.address)},`).join('');
                trace.push(`ready ${Number(renderer.ready())} reserved=${pages!.slots().map((slot) => `${slot},`).join('')} slots=${slots}`);
            } else if (command === 'map') {
                const [sectorSize, nodeX, nodeZ, x, z, height] = values;
                const data = Object.assign(cacheData(8, 2, 3), { sectorSize });
                const address = new NodeAddress(0, nodeX, nodeZ);
                const region = data.nodeRegion(address);
                const cell = region.size / 16;
                const mapping = new VTPageLayout(sectorSize).buildGridMapping(data, address, 0, 0, new Vec3(region.x + x * cell, height, region.z + z * cell));
                const page = mapping.pages[mapping.cells[z * 16 + x]];
                trace.push(`page ${page.key()}`);
            } else if (command === 'morph') {
                const [sectorSize, maxLod] = values;
                const layout = new VTPageLayout(sectorSize);
                const data = Object.assign(cacheData(8, 1, 1), { sectorSize });
                let checked = 0;
                for (let lod = 0; lod <= maxLod; ++lod) {
                    const cell = data.nodeSize(lod) / 16;
                    for (let desired = 0; desired <= layout.rootLevel(); ++desired) {
                        const mapping = layout.buildGridMapping(data, new NodeAddress(lod, 0, 0), 0, 0, new Vec3(0, layout.pageSize(desired) * 2, 0));
                        for (let z = 0; z < 16; ++z) {
                            for (let x = 0; x < 16; ++x) {
                                const page = mapping.pages[mapping.cells[z * 16 + x]];
                                const size = layout.pageSize(page.level);
                                for (let vz = z; vz <= z + 1; ++vz) {
                                    for (let vx = x; vx <= x + 1; ++vx) {
                                        for (const amount of [0, 0.5, 1]) {
                                            const px = (vx - (vx & 1) * amount) * cell;
                                            const pz = (vz - (vz & 1) * amount) * cell;
                                            requireCondition(px >= page.x * size && px <= (page.x + 1) * size && pz >= page.z * size && pz <= (page.z + 1) * size, 'Morph vertex escaped its mapped page');
                                            ++checked;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                trace.push(`vertices ${checked}`);
            } else {
                throw new Error(`Unknown shared parity command: ${command}`);
            }
        }
        const expected = PARITY_EXPECTED.trimStart();
        expect(`${trace.join('\n')}\n`).toBe(expected);
    });
});

// Shared with the native CPU test. Keep these raw blocks literal: the native
// reader extracts their lines without evaluating TypeScript or escape sequences.
const PARITY_CASES = String.raw`
# Shared CPU scenarios. view: camera XYZ, frame count, completions per frame.
# A completion supplies synthetic resources; GPU submission is outside this suite.
tree 3 3 2
view 100 100 100 1 0
view 0.5 0.5 0.5 4 1
view 100 100 100 1 0
view 7.5 0.5 7.5 9 8
replace
view 23.5 0.5 15.5 8 8
view 12 10 8 3 16
view 12 100 8 2 0
tree 2 1 3
view 0.5 0.5 0.5 6 4
view 3.5 0.5 11.5 6 4
# pages: maximum level, sector counts, physical capacity.
# reserve: count followed by (level, x, z). compose: completion budget.
pages 3 1 1 5
reserve 1 3 0 0
compose 1
reserve 5 3 0 0 2 0 0 2 1 0 2 0 1 2 1 1
compose 0
compose 1
compose 1
compose 1
reserve 6 3 0 0 2 0 0 2 1 0 2 0 1 2 1 1 0 0 0
compose 1
reserve 4 2 0 0 2 1 0 2 0 1 2 1 1
invalidate
compose 2
compose 2
reserve 2 0 0 0 0 1 0
compose 1
invalidate
compose 2
reserve 1 0 0 0
invalidate 0.96 0.25 0.02
reserve 2 0 0 0 0 1 0
compose 2
reserve 1 4 0 0
reserve 1 0 8 0
# map: sector size, L0 node XZ, cell XZ, camera height above that cell.
map 4096 1 1 0 0 0
map 4096 1 1 1 13 0
map 4096 1 1 1 13 32
map 4096 511 255 15 15 8192
map 1024 384 128 0 0 0
# morph: sector size, maximum geometry LOD tested; checks all vertex trajectories.
morph 4096 8
morph 1024 5
`;

const PARITY_EXPECTED = String.raw`
tree 3 3 2
view 100 100 100 1 0
frame 0 0 0 target=3:0:0,3:0:1,3:1:0,3:1:1,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
view 0.5 0.5 0.5 4 1
frame 0 1 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,1:2:0,1:2:1,1:2:2,1:2:3,1:3:0,1:3:1,1:3:2,1:3:3,2:0:2,2:0:3,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=1:0:0:0,1:0:1:0,1:1:0:0,1:1:1:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 1 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,1:2:0,1:2:1,1:2:2,1:2:3,1:3:0,1:3:1,1:3:2,1:3:3,2:0:2,2:0:3,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=0:0:0:0,0:0:1:0,0:1:0:0,0:1:1:0,1:0:1:0,1:1:0:0,1:1:1:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 1 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,1:2:0,1:2:1,1:2:2,1:2:3,1:3:0,1:3:1,1:3:2,1:3:3,2:0:2,2:0:3,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=0:0:1:0,0:1:0:0,0:1:1:0,1:0:1:0,1:1:0:0,1:1:1:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,0:0:0:1,3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 1 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,1:2:0,1:2:1,1:2:2,1:2:3,1:3:0,1:3:1,1:3:2,1:3:3,2:0:2,2:0:3,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=0:1:0:0,0:1:1:0,1:0:1:0,1:1:0:0,1:1:1:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,0:0:0:1,0:0:1:1,3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
view 100 100 100 1 0
frame 22 0 0 target=3:0:0,3:0:1,3:1:0,3:1:1,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
view 7.5 0.5 7.5 9 8
frame 0 8 1 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=2:0:0:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:4,2:1:1:4,2:1:2:4,2:1:3:4,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed=2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3,3:0:0,3:0:1,3:1:0,3:1:1, requests=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:0,1:1:1:0,1:1:2:0,1:1:3:0,1:1:4:0,1:1:5:0,1:1:6:0,1:1:7:0,1:2:0:0,1:2:1:0,1:2:2:0,1:2:3:0,1:2:4:0,1:2:5:0,1:2:6:0,1:2:7:0,1:3:0:0,1:3:1:0,1:3:2:0,1:3:3:0,1:3:4:0,1:3:5:0,1:3:6:0,1:3:7:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,2:0:0:1,2:0:1:1,2:0:2:1,2:0:3:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=2:0:0:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:4,2:1:1:4,2:1:2:4,2:1:3:4,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=1:1:0:0,1:1:1:0,1:1:2:0,1:1:3:0,1:1:4:0,1:1:5:0,1:1:6:0,1:1:7:0,1:2:0:0,1:2:1:0,1:2:2:0,1:2:3:0,1:2:4:0,1:2:5:0,1:2:6:0,1:2:7:0,1:3:0:0,1:3:1:0,1:3:2:0,1:3:3:0,1:3:4:0,1:3:5:0,1:3:6:0,1:3:7:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,2:0:0:1,2:0:1:1,2:0:2:1,2:0:3:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 1 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed=1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3, requests=1:2:0:0,1:2:1:0,1:2:2:0,1:2:3:0,1:2:4:0,1:2:5:0,1:2:6:0,1:2:7:0,1:3:0:0,1:3:1:0,1:3:2:0,1:3:3:0,1:3:4:0,1:3:5:0,1:3:6:0,1:3:7:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=0:4:6:0,0:4:7:0,0:4:8:0,0:4:9:0,0:5:6:0,0:5:7:0,0:5:8:0,0:5:9:0,1:3:0:0,1:3:1:0,1:3:2:0,1:3:3:0,1:3:4:0,1:3:5:0,1:3:6:0,1:3:7:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,1:2:0:1,1:2:1:1,1:2:2:1,1:2:5:1,1:2:6:1,1:2:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=1:3:0:0,1:3:1:0,1:3:2:0,1:3:3:0,1:3:4:0,1:3:5:0,1:3:6:0,1:3:7:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,0:4:6:1,0:4:7:1,0:4:8:1,0:4:9:1,0:5:6:1,0:5:7:1,0:5:8:1,0:5:9:1,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,1:2:0:1,1:2:1:1,1:2:2:1,1:2:5:1,1:2:6:1,1:2:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=0:6:4:0,0:6:5:0,0:6:6:0,0:6:7:0,0:6:8:0,0:6:9:0,0:7:4:0,0:7:5:0,0:7:6:0,0:7:7:0,0:7:8:0,0:7:9:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,0:4:6:1,0:4:7:1,0:4:8:1,0:4:9:1,0:5:6:1,0:5:7:1,0:5:8:1,0:5:9:1,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,1:2:0:1,1:2:1:1,1:2:2:1,1:2:5:1,1:2:6:1,1:2:7:1,1:3:0:1,1:3:1:1,1:3:5:1,1:3:6:1,1:3:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=0:7:6:0,0:7:7:0,0:7:8:0,0:7:9:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,0:4:6:1,0:4:7:1,0:4:8:1,0:4:9:1,0:5:6:1,0:5:7:1,0:5:8:1,0:5:9:1,0:6:4:1,0:6:5:1,0:6:6:1,0:6:7:1,0:6:8:1,0:6:9:1,0:7:4:1,0:7:5:1,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,1:2:0:1,1:2:1:1,1:2:2:1,1:2:5:1,1:2:6:1,1:2:7:1,1:3:0:1,1:3:1:1,1:3:5:1,1:3:6:1,1:3:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=1:4:0:0,1:4:1:0,1:4:2:0,1:4:3:0,1:4:4:0,1:4:5:0,1:4:6:0,1:4:7:0,1:5:0:0,1:5:1:0,1:5:2:0,1:5:3:0,1:5:4:0,1:5:5:0,1:5:6:0,1:5:7:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,0:4:6:1,0:4:7:1,0:4:8:1,0:4:9:1,0:5:6:1,0:5:7:1,0:5:8:1,0:5:9:1,0:6:4:1,0:6:5:1,0:6:6:1,0:6:7:1,0:6:8:1,0:6:9:1,0:7:4:1,0:7:5:1,0:7:6:1,0:7:7:1,0:7:8:1,0:7:9:1,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,1:2:0:1,1:2:1:1,1:2:2:1,1:2:5:1,1:2:6:1,1:2:7:1,1:3:0:1,1:3:1:1,1:3:5:1,1:3:6:1,1:3:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:4:6,0:4:7,0:4:8,0:4:9,0:5:6,0:5:7,0:5:8,0:5:9,0:6:4,0:6:5,0:6:6,0:6:7,0:6:8,0:6:9,0:7:4,0:7:5,0:7:6,0:7:7,0:7:8,0:7:9,0:8:4,0:8:5,0:8:6,0:8:7,0:8:8,0:8:9,0:9:4,0:9:5,0:9:6,0:9:7,0:9:8,0:9:9,1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,1:2:0,1:2:1,1:2:2,1:2:5,1:2:6,1:2:7,1:3:0,1:3:1,1:3:5,1:3:6,1:3:7,1:4:0,1:4:1,1:4:5,1:4:6,1:4:7,1:5:0,1:5:1,1:5:2,1:5:3,1:5:4,1:5:5,1:5:6,1:5:7,1:6:0,1:6:1,1:6:2,1:6:3,1:6:4,1:6:5,1:7:0,1:7:1,1:7:2,1:7:3,1:7:4,1:7:5,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=1:0:0:0,1:0:1:0,1:0:2:0,1:0:3:0,1:0:4:0,1:0:5:0,1:0:6:0,1:0:7:0,1:1:0:4,1:1:1:4,1:1:2:4,1:1:3:4,1:1:4:4,1:1:5:4,1:1:6:4,1:1:7:4,2:1:0:6,2:1:1:6,2:1:2:6,2:1:3:6,3:1:0:2,3:1:1:2,3:2:0:0,3:2:1:0, changed= requests=0:8:4:0,0:8:5:0,0:8:6:0,0:8:7:0,0:8:8:0,0:8:9:0,0:9:4:0,0:9:5:0,0:9:6:0,0:9:7:0,0:9:8:0,0:9:9:0,1:5:0:0,1:5:1:0,1:5:2:0,1:5:3:0,1:5:4:0,1:5:5:0,1:5:6:0,1:5:7:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,0:4:6:1,0:4:7:1,0:4:8:1,0:4:9:1,0:5:6:1,0:5:7:1,0:5:8:1,0:5:9:1,0:6:4:1,0:6:5:1,0:6:6:1,0:6:7:1,0:6:8:1,0:6:9:1,0:7:4:1,0:7:5:1,0:7:6:1,0:7:7:1,0:7:8:1,0:7:9:1,1:0:0:1,1:0:1:1,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:0:6:1,1:0:7:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,1:1:6:1,1:1:7:1,1:2:0:1,1:2:1:1,1:2:2:1,1:2:5:1,1:2:6:1,1:2:7:1,1:3:0:1,1:3:1:1,1:3:5:1,1:3:6:1,1:3:7:1,1:4:0:1,1:4:1:1,1:4:5:1,1:4:6:1,1:4:7:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
replace
material 1:0:0 3:0:0
view 23.5 0.5 15.5 8 8
frame 76 8 1 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:4,2:3:1:4,2:3:2:4,2:3:3:4,3:0:0:8,3:0:1:8,3:2:0:2,3:2:1:2, changed=1:0:0,1:0:1,1:0:2,1:0:3,1:0:4,1:0:5,1:0:6,1:0:7,1:1:0,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,1:1:6,1:1:7,2:1:0,2:1:1,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,3:0:0,3:0:1,3:1:0,3:1:1,3:2:0,3:2:1, requests=1:8:4:0,1:8:5:0,1:8:6:0,1:8:7:0,1:9:4:0,1:9:5:0,1:9:6:0,1:9:7:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,3:0:0:1,3:0:1:1,3:2:0:1,3:2:1:1,
frame 0 8 0 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:4,2:3:1:4,2:3:2:4,2:3:3:4,3:0:0:8,3:0:1:8,3:2:0:2,3:2:1:2, changed= requests=2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,3:0:0:1,3:0:1:1,3:2:0:1,3:2:1:1,
frame 0 4 1 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=1:8:4:17,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:20,1:9:5:4,1:9:6:4,1:9:7:4,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:0,2:3:1:0,2:3:2:8,2:3:3:8,2:4:0:0,2:4:1:128,2:5:0:0,2:5:1:0,2:5:2:2,2:5:3:2,3:0:0:8,3:0:1:8, changed=1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,2:5:2,2:5:3,3:2:0,3:2:1, requests=1:10:4:0,1:10:5:0,1:10:6:0,1:10:7:0,1:11:4:0,1:11:5:0,1:11:6:0,1:11:7:0,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:5:0:1,2:5:1:1,2:5:2:1,2:5:3:1,3:0:0:1,3:0:1:1,
frame 0 8 1 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=1:10:4:16,1:10:5:0,1:10:6:0,1:10:7:0,1:11:4:16,1:11:5:0,1:11:6:0,1:11:7:0,1:8:4:17,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:16,1:9:5:0,1:9:6:0,1:9:7:0,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:0,2:3:1:0,2:3:2:8,2:3:3:8,2:4:0:0,2:4:1:128,2:5:0:0,2:5:1:128,3:0:0:8,3:0:1:8, changed=1:10:4,1:10:5,1:10:6,1:10:7,1:11:4,1:11:5,1:11:6,1:11:7,1:9:4,1:9:5,1:9:6,1:9:7,2:5:1,2:5:2,2:5:3, requests=0:20:14:0,0:20:15:0,0:21:14:0,0:21:15:0,0:22:12:0,0:22:13:0,0:22:14:0,0:22:15:0,0:23:12:0,0:23:13:0,0:23:14:0,0:23:15:0,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,1:10:4:1,1:10:5:1,1:10:6:1,1:10:7:1,1:11:4:1,1:11:5:1,1:11:6:1,1:11:7:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:5:0:1,2:5:1:1,3:0:0:1,3:0:1:1,
frame 0 8 1 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=0:20:14:17,0:20:15:1,0:21:14:20,0:21:15:4,1:10:4:16,1:10:5:0,1:10:6:128,1:11:4:16,1:11:5:0,1:11:6:0,1:11:7:2,1:8:4:17,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:16,1:9:5:0,1:9:6:0,1:9:7:8,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:0,2:3:1:0,2:3:2:8,2:3:3:8,2:4:0:0,2:4:1:128,2:5:0:0,2:5:1:128,3:0:0:8,3:0:1:8, changed=0:20:14,0:20:15,0:21:14,0:21:15,1:10:6,1:10:7,1:11:7,1:9:7, requests=0:23:12:0,0:23:13:0,0:23:14:0,0:23:15:0,0:20:14:1,0:20:15:1,0:21:14:1,0:21:15:1,0:22:12:1,0:22:13:1,0:22:14:1,0:22:15:1,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,1:10:4:1,1:10:5:1,1:10:6:1,1:11:4:1,1:11:5:1,1:11:6:1,1:11:7:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:5:0:1,2:5:1:1,3:0:0:1,3:0:1:1,
frame 0 4 1 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=0:20:14:17,0:20:15:1,0:21:14:16,0:21:15:0,0:22:12:17,0:22:13:1,0:22:14:0,0:22:15:0,0:23:12:16,0:23:13:0,0:23:14:0,0:23:15:0,1:10:4:16,1:10:5:0,1:10:6:136,1:11:4:16,1:11:5:128,1:8:4:17,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:16,1:9:5:0,1:9:6:0,1:9:7:8,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:0,2:3:1:0,2:3:2:8,2:3:3:8,2:4:0:0,2:4:1:128,2:5:0:0,2:5:1:128,3:0:0:8,3:0:1:8, changed=0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:6,1:11:5,1:11:6,1:11:7, requests=0:20:14:1,0:20:15:1,0:21:14:1,0:21:15:1,0:22:12:1,0:22:13:1,0:22:14:1,0:22:15:1,0:23:12:1,0:23:13:1,0:23:14:1,0:23:15:1,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,1:10:4:1,1:10:5:1,1:10:6:1,1:11:4:1,1:11:5:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:5:0:1,2:5:1:1,3:0:0:1,3:0:1:1,
frame 0 0 0 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=0:20:14:17,0:20:15:1,0:21:14:16,0:21:15:0,0:22:12:17,0:22:13:1,0:22:14:0,0:22:15:0,0:23:12:16,0:23:13:0,0:23:14:0,0:23:15:0,1:10:4:16,1:10:5:0,1:10:6:136,1:11:4:16,1:11:5:128,1:8:4:17,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:16,1:9:5:0,1:9:6:0,1:9:7:8,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:0,2:3:1:0,2:3:2:8,2:3:3:8,2:4:0:0,2:4:1:128,2:5:0:0,2:5:1:128,3:0:0:8,3:0:1:8, changed= requests=0:20:14:1,0:20:15:1,0:21:14:1,0:21:15:1,0:22:12:1,0:22:13:1,0:22:14:1,0:22:15:1,0:23:12:1,0:23:13:1,0:23:14:1,0:23:15:1,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,1:10:4:1,1:10:5:1,1:10:6:1,1:11:4:1,1:11:5:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:5:0:1,2:5:1:1,3:0:0:1,3:0:1:1,
frame 0 0 0 target=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:5:0,2:5:1,3:0:0,3:0:1, draw=0:20:14:17,0:20:15:1,0:21:14:16,0:21:15:0,0:22:12:17,0:22:13:1,0:22:14:0,0:22:15:0,0:23:12:16,0:23:13:0,0:23:14:0,0:23:15:0,1:10:4:16,1:10:5:0,1:10:6:136,1:11:4:16,1:11:5:128,1:8:4:17,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:16,1:9:5:0,1:9:6:0,1:9:7:8,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:0,2:3:1:0,2:3:2:8,2:3:3:8,2:4:0:0,2:4:1:128,2:5:0:0,2:5:1:128,3:0:0:8,3:0:1:8, changed= requests=0:20:14:1,0:20:15:1,0:21:14:1,0:21:15:1,0:22:12:1,0:22:13:1,0:22:14:1,0:22:15:1,0:23:12:1,0:23:13:1,0:23:14:1,0:23:15:1,1:8:4:1,1:8:5:1,1:8:6:1,1:8:7:1,1:9:4:1,1:9:5:1,1:9:6:1,1:9:7:1,1:10:4:1,1:10:5:1,1:10:6:1,1:11:4:1,1:11:5:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:5:0:1,2:5:1:1,3:0:0:1,3:0:1:1,
view 12 10 8 3 16
frame 25 8 1 target=2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=2:0:0:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0, changed=0:20:14,0:20:15,0:21:14,0:21:15,0:22:12,0:22:13,0:22:14,0:22:15,0:23:12,0:23:13,0:23:14,0:23:15,1:10:4,1:10:5,1:10:6,1:11:4,1:11:5,1:8:4,1:8:5,1:8:6,1:8:7,1:9:4,1:9:5,1:9:6,1:9:7,2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:2,2:3:3,2:4:1,2:4:2,2:4:3,2:5:1,2:5:2,2:5:3,3:0:0,3:0:1, requests=2:0:0:1,2:0:1:1,2:0:2:1,2:0:3:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:4:2:1,2:4:3:1,2:5:0:1,2:5:1:1,2:5:2:1,2:5:3:1,
frame 0 0 0 target=2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=2:0:0:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0, changed= requests=2:0:0:1,2:0:1:1,2:0:2:1,2:0:3:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:4:2:1,2:4:3:1,2:5:0:1,2:5:1:1,2:5:2:1,2:5:3:1,
frame 0 0 0 target=2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3, draw=2:0:0:0,2:0:1:0,2:0:2:0,2:0:3:0,2:1:0:0,2:1:1:0,2:1:2:0,2:1:3:0,2:2:0:0,2:2:1:0,2:2:2:0,2:2:3:0,2:3:0:0,2:3:1:0,2:3:2:0,2:3:3:0,2:4:0:0,2:4:1:0,2:4:2:0,2:4:3:0,2:5:0:0,2:5:1:0,2:5:2:0,2:5:3:0, changed= requests=2:0:0:1,2:0:1:1,2:0:2:1,2:0:3:1,2:1:0:1,2:1:1:1,2:1:2:1,2:1:3:1,2:2:0:1,2:2:1:1,2:2:2:1,2:2:3:1,2:3:0:1,2:3:1:1,2:3:2:1,2:3:3:1,2:4:0:1,2:4:1:1,2:4:2:1,2:4:3:1,2:5:0:1,2:5:1:1,2:5:2:1,2:5:3:1,
view 12 100 8 2 0
frame 24 0 1 target=3:0:0,3:0:1,3:1:0,3:1:1,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed=2:0:0,2:0:1,2:0:2,2:0:3,2:1:0,2:1:1,2:1:2,2:1:3,2:2:0,2:2:1,2:2:2,2:2:3,2:3:0,2:3:1,2:3:2,2:3:3,2:4:0,2:4:1,2:4:2,2:4:3,2:5:0,2:5:1,2:5:2,2:5:3,3:0:0,3:0:1,3:1:0,3:1:1,3:2:0,3:2:1, requests=3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
frame 0 0 0 target=3:0:0,3:0:1,3:1:0,3:1:1,3:2:0,3:2:1, draw=3:0:0:0,3:0:1:0,3:1:0:0,3:1:1:0,3:2:0:0,3:2:1:0, changed= requests=3:0:0:1,3:0:1:1,3:1:0:1,3:1:1:1,3:2:0:1,3:2:1:1,
tree 2 1 3
view 0.5 0.5 0.5 6 4
frame 0 4 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,2:0:2, draw=2:0:0:0,2:0:1:0,2:0:2:0, changed= requests=0:0:0:0,0:0:1:0,0:0:2:0,0:0:3:0,0:1:0:0,0:1:1:0,0:1:2:0,0:1:3:0,1:1:0:0,1:1:1:0,1:1:2:0,1:1:3:0,1:0:2:1,1:0:3:1,2:0:0:1,2:0:1:1,2:0:2:1,
frame 0 4 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,2:0:2, draw=2:0:0:0,2:0:1:0,2:0:2:0, changed= requests=0:1:0:0,0:1:1:0,0:1:2:0,0:1:3:0,1:1:0:0,1:1:1:0,1:1:2:0,1:1:3:0,0:0:0:1,0:0:1:1,0:0:2:1,0:0:3:1,1:0:2:1,1:0:3:1,2:0:0:1,2:0:1:1,2:0:2:1,
frame 0 4 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,2:0:2, draw=2:0:0:0,2:0:1:0,2:0:2:0, changed= requests=1:1:0:0,1:1:1:0,1:1:2:0,1:1:3:0,0:0:0:1,0:0:1:1,0:0:2:1,0:0:3:1,0:1:0:1,0:1:1:1,0:1:2:1,0:1:3:1,1:0:2:1,1:0:3:1,2:0:0:1,2:0:1:1,2:0:2:1,
frame 0 4 1 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,2:0:2, draw=0:0:0:0,0:0:1:0,0:0:2:0,0:0:3:64,0:1:0:4,0:1:1:4,0:1:2:4,0:1:3:68,1:0:2:32,1:0:3:64,1:1:0:2,1:1:1:2,1:1:2:0,1:1:3:64,2:0:2:32, changed=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,1:0:2,1:0:3,1:1:0,1:1:1,1:1:2,1:1:3,2:0:0,2:0:1,2:0:2, requests=0:2:0:0,0:2:1:0,0:3:0:0,0:3:1:0,0:0:0:1,0:0:1:1,0:0:2:1,0:0:3:1,0:1:0:1,0:1:1:1,0:1:2:1,0:1:3:1,1:0:2:1,1:0:3:1,1:1:0:1,1:1:1:1,1:1:2:1,1:1:3:1,2:0:2:1,
frame 0 4 1 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,2:0:2, draw=0:0:0:0,0:0:1:0,0:0:2:0,0:0:3:64,0:1:0:0,0:1:1:0,0:1:2:4,0:1:3:68,0:2:0:0,0:2:1:64,0:3:0:0,0:3:1:64,1:0:2:32,1:0:3:64,1:1:1:34,1:1:2:0,1:1:3:64,2:0:2:32, changed=0:1:0,0:1:1,0:2:0,0:2:1,0:3:0,0:3:1,1:1:0,1:1:1, requests=0:0:0:1,0:0:1:1,0:0:2:1,0:0:3:1,0:1:0:1,0:1:1:1,0:1:2:1,0:1:3:1,0:2:0:1,0:2:1:1,0:3:0:1,0:3:1:1,1:0:2:1,1:0:3:1,1:1:1:1,1:1:2:1,1:1:3:1,2:0:2:1,
frame 0 0 0 target=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,2:0:2, draw=0:0:0:0,0:0:1:0,0:0:2:0,0:0:3:64,0:1:0:0,0:1:1:0,0:1:2:4,0:1:3:68,0:2:0:0,0:2:1:64,0:3:0:0,0:3:1:64,1:0:2:32,1:0:3:64,1:1:1:34,1:1:2:0,1:1:3:64,2:0:2:32, changed= requests=0:0:0:1,0:0:1:1,0:0:2:1,0:0:3:1,0:1:0:1,0:1:1:1,0:1:2:1,0:1:3:1,0:2:0:1,0:2:1:1,0:3:0:1,0:3:1:1,1:0:2:1,1:0:3:1,1:1:1:1,1:1:2:1,1:1:3:1,2:0:2:1,
view 3.5 0.5 11.5 6 4
frame 13 4 1 target=0:0:10,0:0:11,0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:2,1:0:3,1:0:4,1:1:2,1:1:3,2:0:0, draw=1:0:2:16,1:0:3:0,1:0:4:0,1:0:5:0,1:1:2:16,1:1:3:0,1:1:4:0,1:1:5:0,2:0:0:128, changed=0:0:0,0:0:1,0:0:2,0:0:3,0:1:0,0:1:1,0:1:2,0:1:3,0:2:0,0:2:1,0:3:0,0:3:1,1:0:2,1:0:3,1:0:4,1:0:5,1:1:1,1:1:2,1:1:3,1:1:4,1:1:5,2:0:0,2:0:2, requests=0:0:10:0,0:0:11:0,0:1:10:0,0:1:11:0,0:2:8:0,0:2:9:0,0:2:10:0,0:2:11:0,0:3:8:0,0:3:9:0,0:3:10:0,0:3:11:0,1:0:2:1,1:0:3:1,1:0:4:1,1:0:5:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,2:0:0:1,
frame 0 4 1 target=0:0:10,0:0:11,0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:2,1:0:3,1:0:4,1:1:2,1:1:3,2:0:0, draw=0:0:10:16,0:0:11:0,0:1:10:20,0:1:11:4,1:0:2:16,1:0:3:0,1:0:4:128,1:1:2:16,1:1:3:0,1:1:4:0,1:1:5:2,2:0:0:128, changed=0:0:10,0:0:11,0:1:10,0:1:11,1:0:4,1:0:5,1:1:5, requests=0:2:8:0,0:2:9:0,0:2:10:0,0:2:11:0,0:3:8:0,0:3:9:0,0:3:10:0,0:3:11:0,0:0:10:1,0:0:11:1,0:1:10:1,0:1:11:1,1:0:2:1,1:0:3:1,1:0:4:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,2:0:0:1,
frame 0 4 0 target=0:0:10,0:0:11,0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:2,1:0:3,1:0:4,1:1:2,1:1:3,2:0:0, draw=0:0:10:16,0:0:11:0,0:1:10:20,0:1:11:4,1:0:2:16,1:0:3:0,1:0:4:128,1:1:2:16,1:1:3:0,1:1:4:0,1:1:5:2,2:0:0:128, changed= requests=0:3:8:0,0:3:9:0,0:3:10:0,0:3:11:0,0:0:10:1,0:0:11:1,0:1:10:1,0:1:11:1,0:2:8:1,0:2:9:1,0:2:10:1,0:2:11:1,1:0:2:1,1:0:3:1,1:0:4:1,1:1:2:1,1:1:3:1,1:1:4:1,1:1:5:1,2:0:0:1,
frame 0 4 1 target=0:0:10,0:0:11,0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:2,1:0:3,1:0:4,1:1:2,1:1:3,2:0:0, draw=0:0:10:16,0:0:11:0,0:1:10:16,0:1:11:0,0:2:10:0,0:2:11:0,0:2:8:17,0:2:9:1,0:3:10:0,0:3:11:0,0:3:8:16,0:3:9:0,1:0:2:16,1:0:3:0,1:0:4:136,1:1:2:16,1:1:3:128,2:0:0:128, changed=0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:4,1:1:3,1:1:4,1:1:5, requests=0:0:10:1,0:0:11:1,0:1:10:1,0:1:11:1,0:2:8:1,0:2:9:1,0:2:10:1,0:2:11:1,0:3:8:1,0:3:9:1,0:3:10:1,0:3:11:1,1:0:2:1,1:0:3:1,1:0:4:1,1:1:2:1,1:1:3:1,2:0:0:1,
frame 0 0 0 target=0:0:10,0:0:11,0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:2,1:0:3,1:0:4,1:1:2,1:1:3,2:0:0, draw=0:0:10:16,0:0:11:0,0:1:10:16,0:1:11:0,0:2:10:0,0:2:11:0,0:2:8:17,0:2:9:1,0:3:10:0,0:3:11:0,0:3:8:16,0:3:9:0,1:0:2:16,1:0:3:0,1:0:4:136,1:1:2:16,1:1:3:128,2:0:0:128, changed= requests=0:0:10:1,0:0:11:1,0:1:10:1,0:1:11:1,0:2:8:1,0:2:9:1,0:2:10:1,0:2:11:1,0:3:8:1,0:3:9:1,0:3:10:1,0:3:11:1,1:0:2:1,1:0:3:1,1:0:4:1,1:1:2:1,1:1:3:1,2:0:0:1,
frame 0 0 0 target=0:0:10,0:0:11,0:1:10,0:1:11,0:2:10,0:2:11,0:2:8,0:2:9,0:3:10,0:3:11,0:3:8,0:3:9,1:0:2,1:0:3,1:0:4,1:1:2,1:1:3,2:0:0, draw=0:0:10:16,0:0:11:0,0:1:10:16,0:1:11:0,0:2:10:0,0:2:11:0,0:2:8:17,0:2:9:1,0:3:10:0,0:3:11:0,0:3:8:16,0:3:9:0,1:0:2:16,1:0:3:0,1:0:4:136,1:1:2:16,1:1:3:128,2:0:0:128, changed= requests=0:0:10:1,0:0:11:1,0:1:10:1,0:1:11:1,0:2:8:1,0:2:9:1,0:2:10:1,0:2:11:1,0:3:8:1,0:3:9:1,0:3:10:1,0:3:11:1,1:0:2:1,1:0:3:1,1:0:4:1,1:1:2:1,1:1:3:1,2:0:0:1,
pages 3 1 1 5
reserve 1 3 0 0
accepted 1
ready 0 reserved=0, slots=3:0:0:1:-1,0:0:0:0:-1,0:0:0:0:-1,0:0:0:0:-1,0:0:0:0:-1,
compose 1
ready 1 reserved=0, slots=3:0:0:2:0,0:0:0:0:-1,0:0:0:0:-1,0:0:0:0:-1,0:0:0:0:-1,
reserve 5 3 0 0 2 0 0 2 1 0 2 0 1 2 1 1
accepted 1
ready 0 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:1:-1,2:0:1:1:-1,2:1:0:1:-1,2:1:1:1:-1,
compose 0
ready 0 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:1:-1,2:0:1:1:-1,2:1:0:1:-1,2:1:1:1:-1,
compose 1
ready 0 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:2:1,2:0:1:1:-1,2:1:0:1:-1,2:1:1:1:-1,
compose 1
ready 0 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:2:1,2:0:1:2:2,2:1:0:1:-1,2:1:1:1:-1,
compose 1
ready 0 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:2:1,2:0:1:2:2,2:1:0:2:3,2:1:1:1:-1,
reserve 6 3 0 0 2 0 0 2 1 0 2 0 1 2 1 1 0 0 0
accepted 0
ready 0 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:2:1,2:0:1:2:2,2:1:0:2:3,2:1:1:1:-1,
compose 1
ready 1 reserved=1,2,3,4,0, slots=3:0:0:2:0,2:0:0:2:1,2:0:1:2:2,2:1:0:2:3,2:1:1:2:4,
reserve 4 2 0 0 2 1 0 2 0 1 2 1 1
accepted 1
ready 1 reserved=1,2,3,4, slots=3:0:0:2:-1,2:0:0:2:1,2:0:1:2:2,2:1:0:2:3,2:1:1:2:4,
invalidate
ready 0 reserved=1,2,3,4, slots=3:0:0:3:-1,2:0:0:3:1,2:0:1:3:2,2:1:0:3:3,2:1:1:3:4,
compose 2
ready 0 reserved=1,2,3,4, slots=3:0:0:3:-1,2:0:0:2:1,2:0:1:2:2,2:1:0:3:3,2:1:1:3:4,
compose 2
ready 1 reserved=1,2,3,4, slots=3:0:0:3:-1,2:0:0:2:1,2:0:1:2:2,2:1:0:2:3,2:1:1:2:4,
reserve 2 0 0 0 0 1 0
accepted 1
ready 0 reserved=0,1, slots=0:0:0:1:-1,0:1:0:1:-1,2:0:1:2:-1,2:1:0:2:-1,2:1:1:2:-1,
compose 1
ready 0 reserved=0,1, slots=0:0:0:2:0,0:1:0:1:-1,2:0:1:2:-1,2:1:0:2:-1,2:1:1:2:-1,
invalidate
ready 0 reserved=0,1, slots=0:0:0:3:0,0:1:0:1:-1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
compose 2
ready 1 reserved=0,1, slots=0:0:0:2:0,0:1:0:2:1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
reserve 1 0 0 0
accepted 1
ready 1 reserved=0, slots=0:0:0:2:0,0:1:0:2:-1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
invalidate 0.96 0.25 0.02
ready 0 reserved=0, slots=0:0:0:3:0,0:1:0:3:-1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
reserve 2 0 0 0 0 1 0
accepted 1
ready 0 reserved=0,1, slots=0:0:0:3:0,0:1:0:3:1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
compose 2
ready 1 reserved=0,1, slots=0:0:0:2:0,0:1:0:2:1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
reserve 1 4 0 0
accepted 0
ready 1 reserved=0,1, slots=0:0:0:2:0,0:1:0:2:1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
reserve 1 0 8 0
accepted 0
ready 1 reserved=0,1, slots=0:0:0:2:0,0:1:0:2:1,2:0:1:3:-1,2:1:0:3:-1,2:1:1:3:-1,
map 4096 1 1 0 0 0
page 0:16:16
map 4096 1 1 1 13 0
page 1:8:14
map 4096 1 1 1 13 32
page 4:1:1
map 4096 511 255 15 15 8192
page 12:1:0
map 1024 384 128 0 0 0
page 0:1536:512
morph 4096 8
vertices 359424
morph 1024 5
vertices 202752
`;

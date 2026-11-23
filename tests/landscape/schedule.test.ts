import { Frustum } from '../../cocos/core/geometry';
import { Vec3 } from '../../cocos/core/math';
import { LandscapeAsset } from '../../cocos/landscape/Asset';
import { LandscapeData, NodeAddress, VTGridMapping, VTPageAddress, VTPageSources } from '../../cocos/landscape/Data';
import type { LandscapeRenderer, LandscapeRenderNode } from '../../cocos/landscape/Renderer';
import { LandscapeResidency } from '../../cocos/landscape/Residency';
import { LandscapeSchedule } from '../../cocos/landscape/Schedule';
import { VTCapacityQuery, VTRenderer } from '../../cocos/landscape/VTRenderer';

test('target changes and resource publication preserve the committed render snapshot', () => {
    const data = Object.assign(new LandscapeData(), { maxTileLevel: 1, sectorSize: 2, sectorsX: 1, sectorsZ: 1, heightScale: 1 });
    const ranges = Array.from({ length: 2 }, (_, level) => Array.from({ length: data.nodesX(level) * data.nodesZ(level) }, () => ({ minY: 0, maxY: 1 })));
    const asset = { data: () => data, heightRanges: () => ranges } as unknown as LandscapeAsset;
    const renderer = { residency: () => ({}), vtRenderer: () => ({}), setLodMorphRanges: jest.fn() } as unknown as LandscapeRenderer;
    const schedule = new LandscapeSchedule(asset, renderer, 1);
    const target = schedule['_targetTree'];
    const render = schedule['_renderTree'];
    target.updateTarget(new Vec3(0.5, 0.5, 0.5), Vec3.ZERO);
    const root = new NodeAddress(1, 0, 0);
    const material = new VTGridMapping();
    material.pages = [new VTPageAddress(1, 0, 0)];
    schedule['_nodes'].set(root.key(), { address: root, current: material, pending: new VTGridMapping() });
    const changes = render.select(target, (address) => schedule['queryMaterialMapping'](address));
    expect(changes.added).toEqual([root.key()]);
    expect(changes.updated).toEqual([]);
    expect(changes.removed).toEqual([]);
    const prepared = render.findRenderable(root)!;
    const requests = schedule['requests']().map((address) => address.key());
    expect(render.findRenderable(root)).toBe(prepared);
    expect(schedule['requests']().map((address) => address.key())).toEqual(requests);
    expect(target.find(root)!.children).not.toBeNull();
    target.updateTarget(new Vec3(100, 100, 100), Vec3.ZERO);
    expect(target.find(root)!.children).toBeNull();
    expect(render.findRenderable(root)).toBe(prepared);
    material.pages[0] = new VTPageAddress(0, 0, 0);
    expect(prepared.material.pages[0].level).toBe(1);
    const replacement = render.select(target, (address) => schedule['queryMaterialMapping'](address));
    expect(replacement.added).toEqual([]);
    expect(replacement.removed).toEqual([]);
    expect(replacement.updated).toEqual([root.key()]);
    expect(render.findRenderable(root)!.material.pages[0].level).toBe(0);
    expect(prepared.material.pages[0].level).toBe(1);
});

// Same deterministic IO/GPU boundaries as native schedule_test.cpp. The actual
// scheduler, quadtree and mapping layout execute; paging.test covers allocation.
test.each([false, true])('scheduling preserves delayed sources, cancellation, refresh and shared camera passes (triplanar=%s)', (triplanar) => {
    const lines = PARITY_CASES.trimStart().split(/\r?\n/).filter((line) => line && !line.startsWith('#'));
    const [capacityCommand, sourceLimit, pageLimit] = lines.shift()!.split(' ');
    expect(capacityCommand).toBe('capacity');
    const sourceCapacity = Number(sourceLimit);
    const pageCapacity = Number(pageLimit);
    const trace = [`capacity ${sourceCapacity} ${pageCapacity}`];
    const expectedStates = PARITY_EXPECTED.split(/\r?\n/).filter((line) => line.startsWith('state '));
    const targetState = (state: string): string[] => state.split(' nodes=')[1].split(' sources=')[0].split(',').filter(Boolean).map((entry) => entry.split(':').slice(0, 3).join(':'));
    let uploads = 0;
    let compositions = 0;
    let updates = 0;
    let preparing = false;
    let working: NodeAddress[] = [];
    let reserved: VTPageAddress[] = [];
    let loaded = new Map<string, boolean>();
    let composed = new Map<string, boolean>();
    const sourceRequests = new Map<string, { address: NodeAddress; count: number }>();
    const pageRequests = new Map<string, { address: VTPageAddress; count: number }>();
    let inputs: NodeAddress[] = [];
    let pagesDirty = false;
    const data = Object.assign(new LandscapeData(), { sectorsX: 2, sectorsZ: 1, maxTileLevel: 2, sectorSize: 4, heightScale: 1 });
    const ranges = Array.from({ length: data.maxTileLevel + 1 }, (_, level) => Array.from({ length: data.nodesX(level) * data.nodesZ(level) }, () => ({ minY: 0, maxY: 1 })));
    const asset = { data: () => data, heightRanges: () => ranges, loadingFailed: () => false } as unknown as LandscapeAsset;
    const residency = {
        canFit: (sources: NodeAddress[]): boolean => sources.length <= sourceCapacity,
        request: (address: NodeAddress): void => {
            const entry = sourceRequests.get(address.key());
            if (entry) {
                ++entry.count;
            } else {
                sourceRequests.set(address.key(), { address, count: 1 });
            }
        },
        release: (address: NodeAddress): void => {
            const entry = sourceRequests.get(address.key())!;
            expect(entry).toBeDefined();
            if (--entry.count === 0) {
                sourceRequests.delete(address.key());
            }
        },
        canRequest: (sources: NodeAddress[]): boolean => new Set([...sourceRequests.keys(), ...sources.map((source) => source.key())]).size <= sourceCapacity,
        update: (budget: number): boolean => {
            working = [...sourceRequests.values()].map((entry) => entry.address).sort((a, b) => a.level - b.level || a.x - b.x || a.z - b.z);
            expect(working.length).toBeLessThanOrEqual(sourceCapacity);
            loaded = new Map(working.map((address) => [address.key(), loaded.get(address.key()) ?? false]));
            expect(preparing).toBe(true);
            ++updates;
            budget = Math.min(budget, uploads);
            for (const address of working) {
                if (budget && !loaded.get(address.key())) {
                    loaded.set(address.key(), true);
                    --budget;
                }
            }
            return true;
        },
        ready: (): boolean => [...sourceRequests.keys()].every((key) => loaded.get(key)),
        resolve: (node: NodeAddress) => ({ address: node, layer: loaded.get(node.key()) ? 0 : -1 }),
        layerCount: (): number => sourceCapacity,
    } as LandscapeResidency;
    const pages = {
        capacityQuery: (): VTCapacityQuery => new VTCapacityQuery(pages, residency),
        canFit: (addresses: VTPageAddress[]): boolean => addresses.length <= pageCapacity,
        request: (address: VTPageAddress): void => {
            const entry = pageRequests.get(address.key());
            if (entry) {
                ++entry.count;
            } else {
                pageRequests.set(address.key(), { address, count: 1 });
                pagesDirty = true;
            }
        },
        release: (address: VTPageAddress): void => {
            const entry = pageRequests.get(address.key())!;
            expect(entry).toBeDefined();
            if (--entry.count === 0) {
                pageRequests.delete(address.key());
                pagesDirty = true;
            }
        },
        ready: (): boolean => !pagesDirty && reserved.length > 0 && [...composed.values()].every(Boolean),
        needsComposition: (address: VTPageAddress): boolean => !composed.get(address.key()),
        getTriplanarEnabled: (): boolean => triplanar,
        getRefineFarDetail: (): boolean => true,
        selectSources: (): VTPageSources => {
            const result = new VTPageSources();
            result.splat = new NodeAddress(2, 0, 0);
            result.normals.fill(result.splat);
            return result;
        },
        pageTable: () => ({
            texture: () => null,
            canFit: (): boolean => {
                expect(preparing).toBe(false);
                return true;
            },
            ensureCapacity: (): boolean => {
                expect(preparing).toBe(true);
                return true;
            },
        }),
        update: (budget: number): boolean => {
            expect(preparing).toBe(true);
            reserved = [...pageRequests.values()].map((entry) => entry.address).sort((a, b) => a.level - b.level || a.x - b.x || a.z - b.z);
            expect(reserved.length).toBeLessThanOrEqual(pageCapacity);
            composed = new Map(reserved.map((address) => [address.key(), composed.get(address.key()) ?? false]));
            pagesDirty = false;
            inputs.forEach((input) => residency.release(input));
            inputs = [];
            const input = new NodeAddress(2, 0, 0);
            if (pages.ready() || !reserved.length || !residency.canRequest([input])) {
                return true;
            }
            inputs.push(input);
            residency.request(input);
            if (residency.resolve(input).layer < 0) {
                return true;
            }
            budget = Math.min(budget, compositions);
            let count = 0;
            for (const address of reserved) {
                if (count < budget && !composed.get(address.key())) {
                    composed.set(address.key(), true);
                    ++count;
                }
            }
            return true;
        },
    } as unknown as VTRenderer;
    const draws = new Map<string, number>();
    const renderer = {
        residency: () => residency,
        vtRenderer: () => pages,
        setLodMorphRanges: jest.fn(),
        setViewPosition: jest.fn(),
        collectNodeModels: () => true,
        prepareRenderData: (_frame: number, _previous: unknown, added: readonly LandscapeRenderNode[], updated: readonly LandscapeRenderNode[], removed: readonly string[]): void => {
            expect(preparing).toBe(true);
            removed.forEach((key) => draws.delete(key));
            for (const node of added) {
                expect(draws.has(node.geometry.address().key())).toBe(false);
            }
            for (const node of updated) {
                expect(draws.has(node.geometry.address().key())).toBe(true);
            }
            for (const node of [...added, ...updated]) {
                expect(residency.resolve(node.geometry.address()).layer).toBeGreaterThanOrEqual(0);
                expect(node.material.pages.every((page) => !pages.needsComposition(page))).toBe(true);
                draws.set(node.geometry.address().key(), node.edgeMask);
            }
        },
    } as unknown as LandscapeRenderer;
    const schedule = new LandscapeSchedule(asset, renderer, 1);
    const tree = schedule['_targetTree'];
    const render = schedule['_renderTree'];
    let frame = 0;
    let hadCoverage = false;
    for (const line of lines) {
        trace.push(line);
        const [command, ...parameters] = line.split(' ');
        if (command === 'invalidate') {
            composed.forEach((_value, key) => composed.set(key, false));
            continue;
        }
        expect(command).toBe('frame');
        const [x, y, z, uploadBudget, compositionBudget, extraCamera] = parameters.map(Number);
        uploads = uploadBudget;
        compositions = compositionBudget;
        const committed = new Map<string, number>();
        render.forEachRenderableNode((node) => committed.set(node.geometry.address().key(), node.edgeMask));
        schedule.update(new Vec3(x, y, z), Vec3.ZERO);
        const afterTick = new Map<string, number>();
        render.forEachRenderableNode((node) => afterTick.set(node.geometry.address().key(), node.edgeMask));
        expect(afterTick).toEqual(committed);
        preparing = true;
        schedule.prepareRenderData(++frame);
        preparing = false;
        const before = updates;
        for (let view = 0; view < 1 + extraCamera; ++view) {
            const frustum = new Frustum();
            for (const plane of frustum.planes) {
                plane.n.set(1, 0, 0);
                plane.d = 100000 + view;
            }
            schedule.collectPassModels(frustum, false, []);
        }
        expect(updates).toBe(before);
        const coverage = !render.empty();
        const selected = new Map<string, number>();
        render.forEachRenderableNode((node) => selected.set(node.geometry.address().key(), node.edgeMask));
        expect(draws).toEqual(selected);
        preparing = false;
        expect(!hadCoverage || coverage).toBe(true);
        hadCoverage = hadCoverage || coverage;
        const nodes: string[] = [];
        tree.traverse((node) => {
            if (node.geometry.level <= data.maxTileLevel) {
                nodes.push(`${node.geometry.address().key()}:${Number(residency.resolve(node.geometry.address()).layer >= 0)}:${Number(schedule['queryMaterialMapping'](node.geometry.address()) !== null)}:${Number(render.findRenderable(node.geometry.address()) !== null)}`);
            }
            if (schedule['queryMaterialMapping'](node.geometry.address())) {
                expect(residency.resolve(node.geometry.address()).layer).toBeGreaterThanOrEqual(0);
            }
            return true;
        });
        const sources = working.map((address) => `${address.key()}:${Number(loaded.get(address.key()))},`).join('');
        const mappings = reserved.map((address) => `${address.key()}:${Number(composed.get(address.key()))},`).join('');
        trace.push(`state ${Number(coverage)} ${Number(schedule.isRenderStateStable())} nodes=${nodes.sort().map((node) => `${node},`).join('')} sources=${sources} pages=${mappings}`);
        expect(targetState(trace[trace.length - 1])).toEqual(targetState(expectedStates[frame - 1]));
    }
    expect(hadCoverage && schedule.isRenderStateStable()).toBe(true);
    // Triplanar changes composition inputs, not the distance-based page allocation.
    expect(trace[trace.length - 1]).toBe(expectedStates[expectedStates.length - 1]);
    schedule.destroy();
    expect(pageRequests.size).toBe(0);
    expect([...sourceRequests.values()].every((entry) => entry.count === 1 && inputs.some((input) => input.key() === entry.address.key()))).toBe(true);
});

// Shared with the native CPU test. Keep these raw blocks literal: the native
// reader extracts their lines without evaluating TypeScript or escape sequences.
const PARITY_CASES = String.raw`
# source capacity, page capacity; fixed 2x1 sectors, level 2, size 4.
capacity 24 24
# frame: main camera XYZ, source completions, page completions, extra camera pass.
frame 100 100 100 0 0 1
frame 100 100 100 1 0 1
frame 100 100 100 8 1 0
frame 100 100 100 8 8 0
frame 0.5 0.5 0.5 0 0 1
frame 0.5 0.5 0.5 1 0 0
frame 100 100 100 0 0 1
frame 100 100 100 8 8 0
frame 7.5 0.5 3.5 8 1 0
frame 7.5 0.5 3.5 8 2 1
frame 7.5 0.5 3.5 8 8 0
frame 7.5 0.5 3.5 8 8 0
frame 7.5 0.5 3.5 8 8 0
frame 7.5 0.5 3.5 8 8 0
frame 7.5 0.5 3.5 8 8 0
frame 7.5 0.5 3.5 8 8 0
invalidate
frame 7.5 0.5 3.5 0 0 1
frame 7.5 0.5 3.5 8 1 0
frame 7.5 0.5 3.5 8 8 0
frame 7.5 0.5 3.5 8 8 0
frame 100 100 100 8 8 0
frame 100 100 100 8 8 0
frame 100 100 100 8 8 0
`;

const PARITY_EXPECTED = String.raw`
capacity 24 24
frame 100 100 100 0 0 1
state 0 0 nodes=2:0:0:0:0:0,2:1:0:0:0:0, sources=2:0:0:0,2:1:0:0, pages=2:0:0:0,2:1:0:0,
frame 100 100 100 1 0 1
state 0 0 nodes=2:0:0:1:0:0,2:1:0:0:0:0, sources=2:0:0:1,2:1:0:0, pages=2:0:0:0,2:1:0:0,
frame 100 100 100 8 1 0
state 0 0 nodes=2:0:0:1:1:0,2:1:0:1:0:0, sources=2:0:0:1,2:1:0:1, pages=2:0:0:1,2:1:0:0,
frame 100 100 100 8 8 0
state 1 0 nodes=2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1, pages=2:0:0:1,2:1:0:1,
frame 0.5 0.5 0.5 0 0 1
state 1 0 nodes=0:0:0:0:0:0,0:0:1:0:0:0,0:1:0:0:0:0,0:1:1:0:0:0,1:0:0:0:0:0,1:0:1:0:0:0,1:1:0:0:0:0,1:1:1:0:0:0,1:2:0:0:0:0,1:2:1:0:0:0,1:3:0:0:0:0,1:3:1:0:0:0,2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1,1:0:0:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,1:3:1:0, pages=0:0:0:0,0:0:1:0,0:0:2:0,0:1:0:0,0:1:1:0,0:1:2:0,0:2:0:0,0:2:1:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,2:0:0:1,2:1:0:1,
frame 0.5 0.5 0.5 1 0 0
state 1 0 nodes=0:0:0:0:0:0,0:0:1:0:0:0,0:1:0:0:0:0,0:1:1:0:0:0,1:0:0:1:0:0,1:0:1:0:0:0,1:1:0:0:0:0,1:1:1:0:0:0,1:2:0:0:0:0,1:2:1:0:0:0,1:3:0:0:0:0,1:3:1:0:0:0,2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1,1:0:0:1,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,1:3:1:0, pages=0:0:0:0,0:0:1:0,0:0:2:0,0:1:0:0,0:1:1:0,0:1:2:0,0:2:0:0,0:2:1:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,2:0:0:1,2:1:0:1,
frame 100 100 100 0 0 1
state 1 0 nodes=2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1, pages=0:0:0:0,0:0:1:0,0:0:2:0,0:1:0:0,0:1:1:0,0:1:2:0,0:2:0:0,0:2:1:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,2:0:0:1,2:1:0:1,
frame 100 100 100 8 8 0
state 1 0 nodes=2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 1 0
state 1 0 nodes=0:6:2:0:0:0,0:6:3:0:0:0,0:7:2:0:0:0,0:7:3:0:0:0,1:0:0:0:0:0,1:0:1:0:0:0,1:1:0:0:0:0,1:1:1:0:0:0,1:2:0:0:0:0,1:2:1:0:0:0,1:3:0:0:0:0,1:3:1:0:0:0,2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1,1:0:0:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,1:3:1:0, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:0,0:5:3:0,0:6:1:0,0:6:2:0,0:6:3:0,0:7:1:0,0:7:2:0,0:7:3:0,1:0:1:1,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 2 1
state 1 0 nodes=0:6:2:0:0:0,0:6:3:0:0:0,0:7:2:0:0:0,0:7:3:0:0:0,1:0:0:1:1:0,1:0:1:1:1:0,1:1:0:1:0:0,1:1:1:1:0:0,1:2:0:1:0:0,1:2:1:1:0:0,1:3:0:1:0:0,1:3:1:1:0:0,2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1,1:0:0:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,1:3:1:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:0,0:6:2:0,0:6:3:0,0:7:1:0,0:7:2:0,0:7:3:0,1:0:1:1,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 8 0
state 1 0 nodes=0:6:2:0:0:0,0:6:3:0:0:0,0:7:2:0:0:0,0:7:3:0:0:0,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:0:0,1:2:1:1:0:0,1:3:0:1:0:0,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:1, sources=2:0:0:1,1:0:0:1,1:0:1:1,2:1:0:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,1:3:1:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 8 0
state 1 0 nodes=0:6:2:0:0:0,0:6:3:0:0:0,0:7:2:0:0:0,0:7:3:0:0:0,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:1,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:3:1:1,1:2:0:1,1:2:1:1,1:3:0:1,0:6:2:0,0:6:3:0,0:7:2:0,0:7:3:0, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 8 0
state 1 0 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:6:3:1,0:7:2:1,0:7:3:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 8 0
state 1 1 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 8 0
state 1 1 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,2:0:0:1,2:1:0:1,
frame 7.5 0.5 3.5 8 8 0
state 1 1 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,1:3:0:1,2:0:0:1,2:1:0:1,
invalidate
frame 7.5 0.5 3.5 0 0 1
state 1 0 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:0,0:0:1:0,0:0:2:0,0:1:0:0,0:1:1:0,0:1:2:0,0:2:0:0,0:2:1:0,0:5:2:0,0:5:3:0,0:6:1:0,0:6:2:0,0:6:3:0,0:7:1:0,0:7:2:0,0:7:3:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:0,2:1:0:0,
frame 7.5 0.5 3.5 8 1 0
state 1 0 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:1,0:0:1:0,0:0:2:0,0:1:0:0,0:1:1:0,0:1:2:0,0:2:0:0,0:2:1:0,0:5:2:0,0:5:3:0,0:6:1:0,0:6:2:0,0:6:3:0,0:7:1:0,0:7:2:0,0:7:3:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:0,2:1:0:0,
frame 7.5 0.5 3.5 8 8 0
state 1 0 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:0,0:6:1:0,0:6:2:0,0:6:3:0,0:7:1:0,0:7:2:0,0:7:3:0,1:0:1:0,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:0,2:1:0:0,
frame 7.5 0.5 3.5 8 8 0
state 1 0 nodes=0:6:2:1:1:1,0:6:3:1:1:1,0:7:2:1:1:1,0:7:3:1:1:1,1:0:0:1:1:1,1:0:1:1:1:1,1:1:0:1:1:1,1:1:1:1:1:1,1:2:0:1:1:1,1:2:1:1:1:1,1:3:0:1:1:1,1:3:1:1:1:0,2:0:0:1:1:0,2:1:0:1:1:0, sources=2:0:0:1,1:0:0:1,1:1:0:1,1:0:1:1,1:1:1:1,2:1:0:1,1:2:0:1,1:3:0:1,1:2:1:1,1:3:1:1,0:6:2:1,0:7:2:1,0:6:3:1,0:7:3:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,0:5:2:1,0:5:3:1,0:6:1:1,0:6:2:1,0:6:3:1,0:7:1:1,0:7:2:1,0:7:3:1,1:0:1:1,1:1:0:0,1:1:1:0,1:2:0:0,1:2:1:0,1:3:0:0,2:0:0:0,2:1:0:0,
frame 100 100 100 8 8 0
state 1 0 nodes=2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1, pages=0:0:0:1,0:0:1:1,0:0:2:1,0:1:0:1,0:1:1:1,0:1:2:1,0:2:0:1,0:2:1:1,1:0:1:1,1:1:0:1,1:1:1:1,1:2:0:1,1:2:1:1,2:0:0:1,2:1:0:1,
frame 100 100 100 8 8 0
state 1 1 nodes=2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1, pages=2:0:0:1,2:1:0:1,
frame 100 100 100 8 8 0
state 1 1 nodes=2:0:0:1:1:1,2:1:0:1:1:1, sources=2:0:0:1,2:1:0:1, pages=2:0:0:1,2:1:0:1,
`;

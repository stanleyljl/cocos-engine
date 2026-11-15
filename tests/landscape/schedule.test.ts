import { Frustum } from '../../cocos/core/geometry';
import { Vec3 } from '../../cocos/core/math';
import { LandscapeAsset } from '../../cocos/landscape/Asset';
import { LandscapeData, NodeAddress, VTGridMapping, VTPageAddress, VTPageSources } from '../../cocos/landscape/Data';
import type { LandscapeRenderer, LandscapeRenderNode } from '../../cocos/landscape/Renderer';
import { LandscapeResidency } from '../../cocos/landscape/Residency';
import { LandscapeSchedule } from '../../cocos/landscape/Schedule';
import { VTRenderer } from '../../cocos/landscape/VTRenderer';

test('pass diagnostics preserve prepared draws, pending requests and change notifications', () => {
    const data = Object.assign(new LandscapeData(), { maxTileLevel: 1, sectorSize: 2, sectorsX: 1, sectorsZ: 1, heightScale: 1 });
    const ranges = Array.from({ length: 2 }, (_, level) => Array.from({ length: data.nodesX(level) * data.nodesZ(level) }, () => ({ minY: 0, maxY: 1 })));
    const asset = { data: () => data, heightRanges: () => ranges } as unknown as LandscapeAsset;
    const residency = {} as LandscapeResidency;
    const pages = {} as VTRenderer;
    const renderer = { residency: () => residency, vtRenderer: () => pages, setLodMorphRanges: jest.fn() } as unknown as LandscapeRenderer;
    const schedule = new LandscapeSchedule(asset, renderer, 1);
    const tree = schedule['_quadtree'];
    const camera = new Vec3(0.5, 0.5, 0.5);
    tree.updateTarget(camera, Vec3.ZERO);
    const root = new NodeAddress(1, 0, 0);
    const mapping = new VTGridMapping();
    mapping.pages = [new VTPageAddress(root.level, root.x, root.z)];
    expect(tree.publish(root, mapping)).toBe(true);
    expect(tree.selectRenderables()).toBe(true);
    expect(tree.renderSelectionChanged()).toBe(true);
    const prepared = tree.findRenderable(root);
    const requests = tree.requests().map((address) => address.key());
    const changed = new Set(tree.changedNodes());
    const frustum = new Frustum();
    for (const plane of frustum.planes) {
        plane.n.set(1, 0, 0);
        plane.d = 100000;
    }
    schedule.beginPasses();
    schedule.addPass(frustum, true);
    schedule.addPass(frustum, false);
    schedule.endPasses();
    expect(tree.findRenderable(root)).toBe(prepared);
    expect(tree.requests().map((address) => address.key())).toEqual(requests);
    expect(tree.changedNodes()).toEqual(changed);
    expect(tree.renderSelectionChanged()).toBe(true);

    tree.clearChangedNodes();
    tree.updateTarget(camera, Vec3.ZERO);
    expect(tree.selectRenderables()).toBe(true);
    for (const plane of frustum.planes) {
        plane.d = -100000;
    }
    schedule.beginPasses();
    schedule.addPass(frustum, true);
    schedule.endPasses();
    expect(tree.viewChanged()).toBe(false);
    expect(tree.renderSelectionChanged()).toBe(false);
    expect(tree.changedNodes().size).toBe(0);
    expect(tree.findRenderable(root)).toBe(prepared);
    expect(tree.requests().map((address) => address.key())).toEqual(requests);
});

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
        canReserve: (sources: NodeAddress[]): boolean => sources.length <= 2,
        layerCount: (): number => 2,
    } as LandscapeResidency;
    const pages = {
        canReserve: (): boolean => true,
        selectSources,
    } as unknown as VTRenderer;
    const data = Object.assign(new LandscapeData(), { sectorsX: 1, sectorsZ: 1, sectorSize: 4 });
    const asset = { data: () => data, heightRanges: () => [[{ minY: 0, maxY: 1 }]] } as unknown as LandscapeAsset;
    const renderer = { residency: () => residency, vtRenderer: () => pages, setLodMorphRanges: jest.fn() } as unknown as LandscapeRenderer;
    const schedule = new LandscapeSchedule(asset, renderer, 1);
    const pageSources = new Map<string, NodeAddress[]>();
    expect(schedule['canPrepare']({ sources: [first], pages: [page], mappingCount: 0 }, pageSources)).toBe(true);
    // The old page must now be rejected, although its source list is reusable.
    expect(schedule['canPrepare']({ sources: [first, second], pages: [page], mappingCount: 0 }, pageSources)).toBe(false);
    // A retained source can also satisfy the page's composition demand.
    expect(schedule['canPrepare']({ sources: [first, input], pages: [page], mappingCount: 0 }, pageSources)).toBe(true);
    expect(selectSources).toHaveBeenCalledTimes(1);
    expect(schedule['canPrepare']({ sources: [first], pages: [page], mappingCount: 0 }, new Map())).toBe(true);
    expect(selectSources).toHaveBeenCalledTimes(2);
});

// Same deterministic IO/GPU boundaries as native schedule_test.cpp. The actual
// scheduler, quadtree and mapping layout execute; paging.test covers allocation.
test.each([false, true])('scheduling preserves delayed sources, cancellation, refresh and duplicate frames (triplanar=%s)', (triplanar) => {
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
    const data = Object.assign(new LandscapeData(), { sectorsX: 2, sectorsZ: 1, maxTileLevel: 2, sectorSize: 4, heightScale: 1 });
    const ranges = Array.from({ length: data.maxTileLevel + 1 }, (_, level) => Array.from({ length: data.nodesX(level) * data.nodesZ(level) }, () => ({ minY: 0, maxY: 1 })));
    const asset = { data: () => data, heightRanges: () => ranges, loadingFailed: () => false } as unknown as LandscapeAsset;
    const residency = {
        canReserve: (sources: NodeAddress[]): boolean => sources.length <= sourceCapacity,
        reserve: (sources: NodeAddress[]): boolean => {
            expect(preparing).toBe(false);
            expect(sources.length).toBeLessThanOrEqual(sourceCapacity);
            working = sources.slice();
            loaded = new Map(sources.map((address) => [address.key(), loaded.get(address.key()) ?? false]));
            return true;
        },
        loadReserved: (): boolean => {
            expect(preparing).toBe(false);
            return true;
        },
        update: (budget: number): void => {
            expect(preparing).toBe(true);
            ++updates;
            budget = Math.min(budget, uploads);
            for (const address of working) {
                if (budget && !loaded.get(address.key())) {
                    loaded.set(address.key(), true);
                    --budget;
                }
            }
        },
        ready: (): boolean => [...loaded.values()].every(Boolean),
        resolve: (node: NodeAddress) => ({ address: node, layer: loaded.get(node.key()) ? 0 : -1 }),
        layerCount: (): number => sourceCapacity,
    } as LandscapeResidency;
    const pages = {
        canReserve: (addresses: VTPageAddress[]): boolean => addresses.length <= pageCapacity,
        reserve: (addresses: VTPageAddress[]): boolean => {
            expect(addresses.length).toBeLessThanOrEqual(pageCapacity);
            reserved = addresses.slice();
            composed = new Map(addresses.map((address) => [address.key(), composed.get(address.key()) ?? false]));
            return true;
        },
        ready: (): boolean => reserved.length > 0 && [...composed.values()].every(Boolean),
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
        renderPages: (_residency: LandscapeResidency, addresses: VTPageAddress[], budget: number): number => {
            expect(preparing).toBe(true);
            if (residency.resolve(new NodeAddress(2, 0, 0)).layer < 0) {
                return 0;
            }
            budget = Math.min(budget, compositions);
            let count = 0;
            for (const address of addresses) {
                if (count < budget && !composed.get(address.key())) {
                    composed.set(address.key(), true);
                    ++count;
                }
            }
            return count;
        },
    } as unknown as VTRenderer;
    const draws = new Map<string, number>();
    const renderer = {
        residency: () => residency,
        vtRenderer: () => pages,
        setLodMorphRanges: jest.fn(),
        setViewPosition: jest.fn(),
        prepareRenderData: (_frame: number, _previous: unknown, updated: readonly LandscapeRenderNode[], removed: readonly string[]): void => {
            expect(preparing).toBe(true);
            removed.forEach((key) => draws.delete(key));
            for (const node of updated) {
                expect(residency.resolve(node.geometry.address()).layer).toBeGreaterThanOrEqual(0);
                expect(node.material.pages.every((page) => !pages.needsComposition(page))).toBe(true);
                draws.set(node.geometry.address().key(), node.edgeMask);
            }
        },
    } as unknown as LandscapeRenderer;
    const schedule = new LandscapeSchedule(asset, renderer, 1);
    const tree = schedule['_quadtree'];
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
        const [x, y, z, uploadBudget, compositionBudget, repeat] = parameters.map(Number);
        uploads = uploadBudget;
        compositions = compositionBudget;
        const committed = new Map<string, number>();
        tree.forEachRenderable((node) => committed.set(node.geometry.address().key(), node.edgeMask));
        schedule.update(new Vec3(x, y, z), Vec3.ZERO);
        const afterTick = new Map<string, number>();
        tree.forEachRenderable((node) => afterTick.set(node.geometry.address().key(), node.edgeMask));
        expect(afterTick).toEqual(committed);
        preparing = true;
        schedule.prepareRenderData(++frame);
        if (repeat) {
            const before = updates;
            schedule.prepareRenderData(frame);
            expect(updates).toBe(before);
        }
        schedule.beginPasses();
        schedule.endPasses();
        const coverage = tree.sectorRootsReady();
        const selected = new Map<string, number>();
        tree.forEachRenderable((node) => selected.set(node.geometry.address().key(), node.edgeMask));
        expect(draws).toEqual(selected);
        preparing = false;
        expect(!hadCoverage || coverage).toBe(true);
        hadCoverage = hadCoverage || coverage;
        const nodes: string[] = [];
        tree.forEachNode((node) => {
            if (node.geometry.level <= data.maxTileLevel) {
                nodes.push(`${node.geometry.address().key()}:${Number(residency.resolve(node.geometry.address()).layer >= 0)}:${Number(node.ready)}:${Number(node.renderable)}`);
            }
            if (node.ready) {
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
});

// Shared with the native CPU test. Keep these raw blocks literal: the native
// reader extracts their lines without evaluating TypeScript or escape sequences.
const PARITY_CASES = String.raw`
# source capacity, page capacity; fixed 2x1 sectors, level 2, size 4.
capacity 24 24
# frame: camera XYZ, source completions, page completions, repeat same frame.
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

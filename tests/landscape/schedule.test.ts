import { Vec3 } from '../../cocos/core/math';
import { LandscapeAsset } from '../../cocos/landscape/Asset';
import { LandscapeData, NodeAddress, NodeIndexLayout, VTPageAddress, VTPageSources } from '../../cocos/landscape/Data';
import { Quadtree } from '../../cocos/landscape/Quadtree';
import { LandscapeResidency } from '../../cocos/landscape/Residency';
import { LandscapeSchedule } from '../../cocos/landscape/Schedule';
import { VTRenderer } from '../../cocos/landscape/VTRenderer';

// Same deterministic IO/GPU boundaries as native schedule_test.cpp. The actual
// scheduler, quadtree and mapping layout execute; paging.test covers allocation.
test('native/Web scheduling agrees with delayed sources, cancellation, refresh and duplicate frames', () => {
    const lines = PARITY_CASES.trimStart().split(/\r?\n/).filter((line) => line && !line.startsWith('#'));
    const [capacityCommand, sourceLimit, pageLimit] = lines.shift()!.split(' ');
    expect(capacityCommand).toBe('capacity');
    const sourceCapacity = Number(sourceLimit);
    const pageCapacity = Number(pageLimit);
    const trace = [`capacity ${sourceCapacity} ${pageCapacity}`];
    let uploads = 0;
    let compositions = 0;
    let updates = 0;
    let working: NodeAddress[] = [];
    let reserved: VTPageAddress[] = [];
    let loaded = new Map<string, boolean>();
    let composed = new Map<string, boolean>();
    const data = Object.assign(new LandscapeData(), { sectorsX: 2, sectorsZ: 1, maxLevel: 2, sectorSize: 4, heightScale: 1 });
    const ranges = Array.from({ length: new NodeIndexLayout(data).nodeCount() }, () => ({ minY: 0, maxY: 1 }));
    const asset = { data: () => data, heightRanges: () => ranges, loadingFailed: () => false } as LandscapeAsset;
    const residency = {
        canReserve: (sources: NodeAddress[]): boolean => sources.length <= sourceCapacity,
        reserve: (sources: NodeAddress[]): boolean => {
            expect(sources.length).toBeLessThanOrEqual(sourceCapacity);
            working = sources.slice();
            loaded = new Map(sources.map((address) => [address.key(), loaded.get(address.key()) ?? false]));
            return true;
        },
        loadReserved: (): boolean => true,
        update: (budget: number): void => {
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
        getRefineFarDetail: (): boolean => true,
        selectSources: (): VTPageSources => {
            const result = new VTPageSources();
            result.splat = new NodeAddress(2, 0, 0);
            result.normals.fill(result.splat);
            return result;
        },
        pageTable: () => ({ ensureCapacity: (): boolean => true }),
        renderPages: (_residency: LandscapeResidency, addresses: VTPageAddress[], budget: number): number => {
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
    } as VTRenderer;
    const tree = new Quadtree();
    tree.init(asset);
    const schedule = new LandscapeSchedule(asset, residency, pages, tree);
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
        tree.clearChangedNodes();
        tree.beginUpdate(new Vec3(x, y, z), Vec3.ZERO);
        tree.updateTarget();
        schedule.prepareResources(++frame);
        if (repeat) {
            const before = updates;
            schedule.prepareResources(frame);
            expect(updates).toBe(before);
        }
        const coverage = tree.selectRenderables();
        expect(!hadCoverage || coverage).toBe(true);
        hadCoverage = hadCoverage || coverage;
        const nodes: string[] = [];
        tree.forEachNode((node) => {
            if (node.geometry.level <= data.maxLevel) {
                nodes.push(`${node.geometry.address().key()}:${Number(node.loaded)}:${Number(node.ready)}:${Number(node.renderable)}`);
            }
            if (node.ready) {
                expect(residency.resolve(node.geometry.address()).layer).toBeGreaterThanOrEqual(0);
            }
            return true;
        });
        const sources = working.map((address) => `${address.key()}:${Number(loaded.get(address.key()))},`).join('');
        const mappings = reserved.map((address) => `${address.key()}:${Number(composed.get(address.key()))},`).join('');
        trace.push(`state ${Number(coverage)} ${Number(schedule.isSettled())} nodes=${nodes.sort().map((node) => `${node},`).join('')} sources=${sources} pages=${mappings}`);
    }
    expect(hadCoverage && schedule.isSettled()).toBe(true);
    const expected = PARITY_EXPECTED.trimStart();
    expect(`${trace.join('\n')}\n`).toBe(expected);
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

# Landscape runtime

## Responsibilities

| Module | Owns | Does not own |
| --- | --- | --- |
| `Landscape` / `QuadTree` | Scene lifecycle, camera selection, visibility and geometry LOD | Material page residency |
| `LandscapeAsset` | Manifest validation, height ranges, file resolution and asynchronous tile decoding | GPU uploads or scene models |
| `TilePagePool` | Paired height/splat/normal texture arrays, source residency, upload budget and eviction protection | Geometry or VT LOD policy |
| `TilePageResolver` (in `TilePagePool.*`) | Source coordinates, resident ancestor lookup, normal gutter sources and interval-local lookup cache | Physical storage or upload scheduling |
| `VTRequestPlan` (in `VTPaging.h`) | Visible material requests, ancestor priorities, coverage budget and permanent root requests | GPU resources or I/O |
| `VirtualTexture` | Atlas resources, page allocation, dirty/ready state and content revision | Material composition commands |
| `VTRenderer` | Composition resources, fixed cliff references, dirty-page submission and three mip levels | Camera selection or terrain models |
| `LandscapeRenderer` | Frame orchestration, patch/model pools, instance attributes and displaced decal grids | Manifest parsing or source fallback policy |
| `MaterialLibrary` | Asset-defined material/decal texture arrays and fallback texture | Application-specific material presets |

The asset, tile pool and VT have different lifetimes and revisions. A source tile
is an input shared by geometry and composition; a VT page is cached composed
material output. Their LODs and page identities are independent.

## Debug settings

`LandscapeDebugData` holds all debug switches in both TypeScript and native
code. Project controls and the Inspector edit `landscape.debugData`; the
component sends one `setDebugData()` update when its values change, before
selection/drawing. Scenes/prefabs serialize only the nested `_debugData` object;
there are no flat debug properties or legacy migration paths.
The project's `landscape-assets` Inspector places Debug Data last in a section
that starts collapsed and keeps the user's expansion state during value updates.

`Landscape` stores requested settings across enable/disable. `LandscapeRenderer`
applies only changed settings; `VTRenderer` retains the applied compose state,
which can intentionally lag requested settings while frozen. A batch freezes
before applying mode changes and unfreezes after recording the requested modes.
The initial readiness barrier still defers effective freezing until data is ready.
Shadow casting/receiving, LOD quality, global color map and strength remain
separate component properties. `showRanges` controls both LOD distance
ranges and sector boundaries, both shaded directly on the terrain surface.
Keyboard mappings belong to the project controls.
VT sampling always uses trilinear mip filtering; it has no debug toggle.

The optional `globalColorMap` is a regular `Texture2D` assigned on the component,
immediately below Landscape Asset in the Inspector. The scene reference handles
loading and packaging; terrain manifests contain no global-color file path.
Without a texture, composition binds a white fallback with zero blend strength.
Map/strength edits invalidate composed VT pages, deferred until unfreezing when frozen.

## Active frame

Every main-camera, directional-shadow cascade and spot-shadow pass traverses
the sector quadtrees from their roots with its own frustum. LOD and morph always
use the main-camera position; a shadow pass never starts from camera-visible
nodes. The legacy culling flow calls `preparePasses(camera, sceneData)` once after
updating the light frusta. It captures the main-camera position, records independent
node lists and prepares shared resources before publishing each pass's model list.
The first release supports only the legacy forward
pipeline, which batches the camera and its shadow passes. Landscape does not
participate in custom-pipeline culling or render queues.

Only the resource plan merges overlapping nodes and quadrant masks. Terrain and
displaced-decal models are owned by `LandscapeRenderer`, attached to the scene
but excluded from its generic model list/octree. Each pass collects only models
belonging to its own nodes/quadrants and culls their bounds against its frustum.
The renderer updates these models' transforms/UBOs after preparation and forwards
global pipeline state changes. Common CSM projection calculations are unchanged.
Freezing holds the main-camera LOD position and source/VT residency after readiness;
each pass still traverses and culls against its current frustum.

Resource preparation keeps two selections: geometry for all passes, and material
coverage for color passes only. Only material coverage is split at VT page
boundaries and contributes dynamic VT requests. Subtracting its quadrant masks
from geometry coverage leaves shadow-only quadrants, each using one 8x8 grid
without VT lookup or parent-normal requests. Shared quadrants reuse color-pass
models; the grid vertices, LOD and morph are identical across both paths.
The source tile pool still loads height/splat/normal together; this split avoids
extra material-detail requests without changing that storage format.
The color selection is reused directly as material coverage without a second
node-list copy. Cameras whose visibility mask excludes terrain skip preparation.
After independent pass collection, directional cascades honor the light's
`REMOVE_DUPLICATES` setting just like legacy `shadowCulling`: a model rendered
completely inside an earlier cascade is omitted from later ones. Models crossing
a cascade boundary remain eligible in both. This does not apply to spot lights
or fixed-area shadows and does not change the shared geometry/VT resource plan.

On enable, terrain and displaced decal models are withheld until the current
pass resource plan is ready. Source readiness checks exact height/splat/normal
requests (including normal parents and gutters), not resident ancestors. Cliff
references must be complete, and every budgeted VT page, including roots, must
have submitted its composition and all mip levels with the final inputs.
Initial preparation synchronously loads the exact source working set, resolves
new dependencies with a bounded loop, and submits all initial VT pages/mips
before models bind the atlas. Graphics queue ordering provides write-before-read
without an extra present or GPU-idle wait. A requested freeze takes effect after
preparation so it cannot prevent the first reveal. Normal streaming remains
asynchronous and budgeted after this first preparation.
`Landscape.isReady` is false until this one-time barrier passes, then remains true
for normal streaming. Disabling/reloading the terrain resets it. The plan follows
the current camera during preparation and retains the existing LOD/cache budgets;
it does not preload the whole world at maximum resolution or use a timed delay.
Failed source requests or insufficient residency stop initial preparation;
the terrain stays hidden and diagnostics instruct the caller to fix assets or
capacity and re-enable Landscape. This render readiness does not imply query
cache or collision-region readiness.

`LandscapeRenderer::sync()` runs these stages in order:

1. Check `LandscapeSyncCache` against both geometry and material selections.
   A stationary camera still polls source uploads; a source or VT revision change
   resumes the complete update. Changing only color coverage must also invalidate it.
2. `buildFramePlan()`: split color geometry into material-page patches and build a
   bounded `VTRequestPlan`, including permanent sector roots. Append shadow-only
   quadrant grids after material requests are complete.
3. `syncPageSources()`: protect the chosen VT working set; reset source protection;
   protect cliff references, all geometry heights and color-pass parent normals; resolve composition
   inputs; publish up to the source upload budget; re-resolve if residency changed;
   acquire/update the requested VT pages.
   Initial preparation instead uses `loadRequestedTiles()` synchronously, repeats
   dependency resolution within its bound, then submits all initial VT pages and
   mips and checks `initialDataReady()` before continuing to model publication.
4. `syncTerrainModels()`: retire invisible patches before reusing/creating models,
   then bind resident source tiles and, only for color coverage, ready VT pages
   (or ancestors).
5. `syncDecals()`: select displaced decals by distance once per decal, then emit
   fixed-lattice cells intersecting terrain patches, reusing model instances.
6. Rebuild the node-to-model index when models change and store the cache snapshot
   **before** VT composition.

`LandscapeRenderer::preparePasses()` then calls `VTRenderer::render()` explicitly,
after all pass requests are protected and before any pass consumes terrain:

1. Collect budgeted dirty pages, including dirty roots.
2. Prepare page instance data, decal indices and normal source tables.
3. Upload those tables.
4. Submit composition and all three mip levels on the graphics queue.
5. Mark pages rendered and advance the VT revision. The next frame can bind newly
   ready fine pages even if the camera has not moved.

## Invariants when editing

- Never upload/recycle source layers before protecting the frame's inputs.
  Invalidate `TilePageResolver` after source protection resets and after uploads
  change residency; its cached hits do not repeat pool protection calls.
- Poll source uploads at most once per frame, including the stationary-camera path.
- Budget dynamic VT requests before protecting them, or old requests can prevent
  new nearby pages from replacing distant pages. Keep sector roots permanent.
- Fine VT pages become selectable only after composition and all mip submissions.
  Roots bootstrap the first frame through the same queue before the Base Pass.
- Resolve normal gutters at one common resident source level to avoid partial
  resolution seams. Request fine sources while using complete ancestor coverage.
- Keep morphing geometry inside its selected VT page for its entire trajectory.
- Decal tessellation uses a fixed landscape lattice, independent of terrain LOD.
  Zero-displacement decals are composed into RVT without separate grid models.
- Frozen VT pages must retain matching shader decoding. Deferred mode changes are
  applied on unfreeze; do not change only one side of composition/decoding.
- Initialize each object once. Cleanup belongs to failure/destruction paths.
  Destroy the source resolver before the tile pool it references.
- Manifest sections parse into temporary state and publish together after all
  validation succeeds. Material IDs, decals and cliff overrides come from assets.

## Allocation and validation

Frame vectors and hash-table bucket storage are reused. The stable-camera fast
path does not rebuild them. GPU texture sizes, shaders, material sampling,
composition passes and upload budgets are unchanged by the orchestration refactor.
Retained scratch capacity trades some CPU memory for fewer allocation spikes;
hash-table entries can still allocate when rebuilding a changed frame.

The standalone `native/tests/landscape-vt` CMake target tests paging coverage,
morph containment, cache invalidation and request-plan equivalence with the
pre-refactor loop across 400 changing frames (including duplicates, empty views,
equal priorities and a cache occupied entirely by roots).
It also checks independent material-coverage invalidation and subtraction of
shared quadrant masks from shadow-only geometry.

Native compilation and these tests do not establish visual or frame-time parity.
Runtime acceptance should compare startup streaming, near-ground travel, distant
terrain, cache pressure, freeze/unfreeze, normal/cliff toggles and decal fades.

## CPU surface queries

The component exposes `addQuerySource(node, preloadRadius = 64)`,
`setQuerySourceRadius`, `removeQuerySource`, `getQuerySourceStatus`,
`isQuerySourceReady` and `sampleSurface(worldPosition, output)`.
Sources follow nodes and preload/pin square world-XZ regions independently of
camera LOD. Overlapping sources share tiles; removing one source only releases
its own protection. Unprotected entries can be evicted. Inactive sources stop
pinning and destroyed nodes are removed.

`sampleSurface` reads resident CPU memory only, ignoring input Y. It returns
`Hit`, `Miss`, `NotReady` or `Error`; only `Hit` writes the output position,
normal, surface type and weight. Height uses the fixed finest source grid and
B-C triangle interpolation; normals are decoded/interpolated stored geometry
normals. No readback, GPU resource or synchronous disk I/O is required by a
sample call. No Landscape raycast API is provided.

Set `queryCacheCapacity` before enabling the component. The default 64 tiles at
129x129 require about 7.1 MiB for height/RGB normals/splat, excluding at most two
in-flight decodes and temporary allocations. An over-budget source stays
`NotReady` rather than evicting another source's protected tiles. This native
query path supports translation-only placement and excludes displaced decals.

## Regional physics

`landscape.physics` owns a separate `LandscapePhysics` manager. Configure it
before registering regions; defaults are `maxTiles: 64`,
`maxConcurrentLoads: 1`, `maxCreationsPerStep: 1`, `group: 1`, `mask: -1`,
`material: null`. These are count limits, not millisecond guarantees.

| API | Contract |
| --- | --- |
| `addRegion(bounds)` / `setRegionBounds(id, bounds)` | World-XZ `{minX, minZ, maxX, maxZ}`, clipped to terrain and rounded outward to complete source tiles |
| `removeRegion(id)` / `clearRegions()` | Release ownership; overlapping regions keep shared tiles alive |
| `getRegionStatus(id)` / `isRegionReady(id)` | Report whole-region readiness, including explicit `OutOfCapacity` and `Error` states |
| `isAreaReady(bounds)` | Check existing collision for a local movement guard; does not load or pin data |
| `getRegionBounds(id, output)` / `getRegionError(id)` | Inspect actual aligned coverage and diagnostics |
| `getStats()` | Resident tiles, raw source bytes, in-flight loads and queued tiles; excludes backend allocations |
| `forEachCollider(visitor)` | Visit submitted colliders for optional debug rendering; creates no geometry or GPU resources itself |
| `update()` | Automatic simulation calls this before physics; manual simulation must call it before scene synchronization/step, outside contact callbacks |

Physics uses fixed finest-source height data and B-C triangles independently of
camera LOD, with translation-only placement and no displaced-decal collision.
Applications control regions and wait for collision readiness before birth or
teleport. The manager does not move or pause gameplay objects. Loads are
asynchronous, backend creation is budgeted, and obsolete completions cannot
restore unloaded data. Query, physics and render caches have separate budgets
and lifetimes despite reading the same dataset.

Backend adaptations stay in `cocos/landscape/landscape-physics-collider.ts`.
Bullet reuses the existing heightfield implementation; Cannon coordinate/sample
mapping and Web PhysX cooking/ownership are Landscape-specific. Native
`LandscapeHeightfield` owns its resources while reusing existing JSB shape
registration. The global physics factory, original Terrain interfaces/caches,
and public backend destruction paths are unchanged.

At 64 tiles of 129x129 Uint16 samples, raw physics source data is about 2.03 MiB;
backend representations, acceleration structures and cooking temporaries are
additional. In particular, Cannon JS arrays cannot be counted as Uint16 storage.
The project F12 overlay creates debug meshes only when enabled and uses shader
depth offset without global GLES/WebGL polygon-offset changes.

As of 2026-09-24, automated checks cover Bullet, Cannon and Web PhysX, plus JSB
adapter tests with mocked native SDK calls. Windows Release compilation/linking
passed with native PhysX disabled; the added C++ source also compiled separately
with PhysX enabled. Full native PhysX linking/simulation, long-running resource
reclamation and Release frame-time/memory measurements remain delivery checks.
Editor terrain preview/editing is deferred to version two.

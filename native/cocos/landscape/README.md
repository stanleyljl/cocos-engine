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
| `MaterialLibrary` | Asset-defined material/decal texture arrays and global color texture | Application-specific material presets |

The asset, tile pool and VT have different lifetimes and revisions. A source tile
is an input shared by geometry and composition; a VT page is cached composed
material output. Their LODs and page identities are independent.

## Active frame

On enable, terrain and displaced decal models are withheld until the current
main-camera plan is ready. Source readiness checks exact height/splat/normal
requests (including normal parents and gutters), not resident ancestors. Cliff
references must be complete, and every budgeted VT page, including roots, must
have submitted its composition and all mip levels with the final inputs.
Streaming and `BeforeRender` continue during this preparation; a requested F6
freeze takes effect after preparation so it cannot prevent the first reveal.
`Landscape.isReady` is false until this one-time barrier passes, then remains true
for normal streaming. Disabling/reloading the terrain resets it. The plan follows
the current camera during preparation and retains the existing LOD/cache budgets;
it does not preload the whole world at maximum resolution or use a timed delay.
Failed source requests or insufficient residency cannot satisfy the barrier;
the terrain stays hidden and the existing asset/pool diagnostics report the issue.

`LandscapeRenderer::sync()` runs these stages in order:

1. Check `LandscapeSyncCache`. A stationary camera still polls source uploads;
   a source or VT revision change resumes the complete update.
2. `buildFramePlan()`: split selected geometry into material-page patches and
   build a bounded `VTRequestPlan`, including permanent sector roots.
3. `syncPageSources()`: protect the chosen VT working set; reset source protection;
   protect cliff references, geometry and parent normals; resolve composition
   inputs; publish up to the source upload budget; re-resolve if residency changed;
   acquire/update the requested VT pages.
4. `syncTerrainModels()`: retire invisible patches before reusing/creating models,
   then bind resident source tiles and ready VT pages (or ancestors).
5. `syncDecals()`: select displaced decals by distance once per decal, then emit
   fixed-lattice cells intersecting terrain patches, reusing model instances.
6. Store the cache snapshot **before** the `BeforeRender` callback.

`VTRenderer::render()` runs in `BeforeRender`, before any terrain Base Pass:

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

Native compilation and these tests do not establish visual or frame-time parity.
Runtime acceptance should compare startup streaming, near-ground travel, distant
terrain, cache pressure, freeze/unfreeze, normal/cliff toggles and decal fades.

# Landscape Asset Tools

## Splat tile format

Material distribution is authored by the application or its asset tools.
The engine does not prescribe material names, IDs or elevation presets.

Splat PNGs are **16-bit grayscale**, decoded/uploaded as **R16UI**:

| Bits | Value |
| --- | --- |
| 0..4 | First material layer ID |
| 5..9 | Second (upper) material layer ID |
| 10..15 | Second layer weight, divided by 63 |

The first layer has weight `1 - secondWeight`. IDs refer to `materialLibrary.layers`.
The encoding allows any pair of IDs from 0 through 31. Coarser splat levels
must preserve valid material IDs; never average packed uint16 values.

The runtime samples the three splat texels of the containing triangle. Import
must constrain each triangle to at most three unique materials. The compose
pass combines barycentric interpolation with per-pair height blending and
writes the result into RVT. Height, splat and normal tiles share residency and
are published together. A failed tile set never becomes resident.

## Packing terrain material textures

`pack-material-textures.py` reads downloaded `*_1k.blend.zip` or numbered `00.zip` source
materials and creates two 1024x1024 RGBA8 PNGs per material. It requires a
64-bit Python (3.11 or later); `uv run` installs the script's isolated OpenEXR,
NumPy, Pillow and PyPNG dependencies.

```text
uv run --python 3.12 scripts/landscape/pack-material-textures.py --raw <dataset>/raw --manifest <dataset>/<name>.json --output <empty-staging-directory>
```

The staging directory contains `textures/`, an updated copy of the manifest,
the original manifest backup and a packing report with source ranges and PNG
hashes. Copy the generated PNGs and updated manifest into the dataset after
verification. Source ZIPs and existing assets are not overwritten by the tool.

To add a single downloaded material when numbered source archives differ from
runtime layer IDs,
use `--append-archive <source-material.zip>` instead of `--raw`. The material name
is read from its albedo filename. A new name receives the next free layer ID;
repacking the same name keeps its existing ID and authored parameters. Other
manifest entries and texture files are preserved. This mode must retain the array's
existing resolution. The bulk `--raw` mode rejects an incomplete source library
instead of silently dropping generated layers.

Use `--layers 13,14` to pack only newly added layers, preserving other manifest
entries and their existing PNGs. Numbered ZIPs remain contiguous; new layer names
are discovered from the archive's albedo filename, and existing names are
validated against the manifest. New layers default to 128 source pixels per meter, detail height scale 1.0 and
bias 0.0. Author `pixelsPerMeter` in the manifest to choose each material density. The staged manifest
includes both old and new layers; only selected PNGs are emitted.

| PNG | RGB / RG | B | A |
| --- | --- | --- | --- |
| `AlbedoHeight_00.png` | RGB: sRGB albedo | Part of RGB | Linear detail height |
| `NormalRoughnessAO_00.png` | RG: encoded tangent-space normal XY | Roughness | AO (255 when absent) |

Named archives are sorted by material name before assigning ids starting at zero.
Numbered archives must be contiguous from `00.zip`; their material names are checked against
matching manifest layer IDs, preserving the chosen terrain order.
The demo uses IDs 00–11 for the soil-to-rock sequence and 12 for the artificial
stone path, which should be excluded from future automatic elevation blending.
The manifest's
`materialLibrary` records `dir: "textures"`, `resolution`, `format: "RGBA8"`
and each layer's `id`, `name`, `albedoHeight`, `normalRoughnessAO`,
`detailHeightScale` and `detailHeightBias`. Detail height scale defaults
to 1.0 and bias to 0.0; repacking preserves existing parameters by material name. Re-running
the height tile generator with `--force` preserves this explicit material
library from the existing manifest instead of rescanning the old `materials/`
directory. It validates referenced PNG paths before replacing height tiles.

Height is read at its source precision before rounding to 8-bit. Normal vectors
are normalized before encoding XY; negative Z is reflected into the positive
hemisphere used by shader reconstruction. Roughness is clamped to [0,1]. No
gamma, exposure, alpha premultiplication or per-image range normalization is
applied during packing. All source images must match `--resolution` (1024 by
default); the tool rejects mismatches instead of resizing individual channels.

## Height tile generation

`heightmap-tiles.js` converts a one-meter sampled height source into the
Landscape node pyramid described by `Landscape-Design-and-Implementation.md`.

The output layout is:

```text
assets/landscape/<name>/
  <name>.json
  nodes/L0/h_<x>_<y>.png
  nodes/L1/h_<x>_<y>.png
  ...
```

Each height tile is a 16-bit grayscale PNG (`color type=0`, `bit depth=16`).
After PNG decoding, each pixel is one `R16UI` unsigned 16-bit sample. The
encoded value is:

```text
full 16 bits quantized height in [0, 65535]
```

LOD numbering follows CDLOD: `L0` is the finest (smallest) node level and
larger level numbers are progressively coarser, with `Lmax` as the root. The
global node subdivision at level `L` is `2^(maxLevel-L)` per sector axis.

The node coordinates are global within each level. A sector coordinate is
derived by shifting the node coordinate right by `maxLevel-level`.

Each level entry in the manifest also carries `heightRange.{min,max}`: per-node
quantized height bounds (`uint16`, row-major `iz*nx+ix`, global per-level
coords), merged bottom-up so a node encloses its whole subtree. The runtime
turns these into a tight per-node AABB (`heightMeters = heightBias +
(v/65535)*heightScale`) for frustum culling / LOD selection, instead of the
whole `[minY,maxY]` band. `heightmap-tiles.js` emits this automatically.

Generate a deterministic one-Sector fixture:

```powershell
node scripts/landscape/heightmap-tiles.js `
  --procedural `
  --out <output-directory> `
  --name heightmap-demo `
  --sectors-x 1 `
  --sectors-y 1 `
  --force
```

Convert an external source. The input must have one sample per meter and the
full world dimensions, so a 5x5 Sector source is `20481x20481`:

```powershell
node scripts/landscape/heightmap-tiles.js `
  --input source-height.png `
  --out <output-directory> `
  --name world `
  --sectors-x 5 `
  --sectors-y 5 `
  --height-scale 2000 `
  --height-bias 0
```

## Patching an older dataset with per-node height bounds

Datasets generated before `heightRange` existed lack the tight per-node
bounds, so the runtime falls back to the global `[minY,maxY]` band (loose AABBs,
oversized debug boxes). Backfill `heightRange` from the already-generated tiles
— no source PNG needed, tiles are not rewritten:

```powershell
node scripts/landscape/patch-height-range.js `
  --manifest <dataset>/<name>.json
```

It scans every `h_<x>_<y>.png`, computes each node's `min/max`, merges bottom-up,
and writes `heightRange` back into the manifest. Refresh the project assets so
the running app reloads the patched manifest.

## RVT terrain normals

`Landscape.rvtNormalEnabled` defaults to true. Composition selects the existing
filtered normal tiles by the RVT page footprint, independently of geometry LOD.
Material and planar-decal normals are combined first, then transformed through
the terrain normal frame. The Base Pass reads this cached terrain-local normal;
it no longer samples current/parent terrain normals on the enabled path.

The two RGBA8 atlases and three physical mips retain their sizes. Normal X/Z
occupy the normal atlas RG channels; signed Y occupies the previously unused
albedo alpha. Roughness/AO remain BA. Mip generation filters full vectors, and
terrain/decal output remains opaque. An atlas viewer should display RGB without blending
by the normal-bearing alpha channel.

A 64 KiB RGBA32F lookup stores each page's 2x2 normal-source regions plus a
neighbor ring for filtering gutters. Sources within a page use one resident
resolution, falling back together while finer tiles load. Changes invalidate
the affected page. Source normals still share the height/splat streaming pool;
this can increase tile requests and composition cost during camera movement.

Stationary frames reuse source mappings and instance data once both tile and VT
content revisions stop changing. Async completions are still polled; uploads,
page publication and material invalidation wake the update. During active
updates each source tile is resolved once per residency revision, and normal
sources are only resolved a second time if uploads changed the available data.

`Landscape.rvtNormalEnabled` switches between cached normals and the original
geometry-LOD normal blend. Switching invalidates pages; permanent roots provide
a fallback until fine pages are rebuilt. Changes defer while residency is frozen.
`Landscape.decal3DEnabled` controls raised decal geometry independently.

Restart Creator, rebuild the project, then rebuild the native executable so
Effects, TypeScript and the new native binding agree. Existing source normal
assets do not need regeneration for this change. After compiling both Effects
with Creator's Effect compiler, set `LANDSCAPE_EFFECT_JSON`,
`LANDSCAPE_BASE_EFFECT_JSON` and `CHROME_PATH`, then run:

```powershell
node --test scripts/landscape/rvt-normal-baking.test.js scripts/landscape/decal-rendering.test.js
```

These tests cover signed normal composition/filtering, gutter source selection,
decal composition order, Base Pass morph independence and conforming decal
geometry. Native visual and performance comparison remains a separate check.

## Cached cliff projection

`Landscape.cliffEnabled` controls cached triplanar projection. The optional
terrain-manifest `cliffMaterial` object selects a slope-based material override:

- `layer`: an existing material-library ID.
- `maxNormalY`: normal Y threshold in [0, 1]; the override applies below it.
- `globalColorInfluence`: multiplier in [0, 1] of the global color strength.

Omit the object to use painted splat materials and normal global color behavior.
An override uses the same slope coverage with projection enabled or disabled;
reference tiles remain resident in either mode. No material name or ID is
selected by the engine. Planar decals compose after terrain materials.
Reimport the terrain and rebuild project assets after changing its manifest.

Projection changes defer while residency is frozen. Restart Creator, rebuild
the project, then rebuild native code after engine TypeScript, Effect or native
binding changes. Shortcut assignments belong to the application.

References: `TerrainRenderingFarCry5.pdf`, PDF pages 92-112, and the original
`Jiao_Hang_Delta+Force+Performant.pdf`, PDF pages 76-81 (both in the terrain docs
directory). The latter explicitly notes that VT-baked cliffs lose maximum
close-up resolution. This implementation adopts cached projection, but uses
deterministic three-plane blending instead of their stochastic axis selection.

The compose pass samples the existing materials from XZ, signed ZY and signed
XY planes. Normal-direction weights suppress side projection on gentle slopes;
negligible axes are skipped. Flat pixels evaluate one projection, cliffs usually
one or two, and three-way transitions at most three. Each projection retains the
existing triangle/pair height blend. Projection normal frames are rotated
onto the terrain before mixing; neutral material normals reproduce the terrain
normal on every signed axis. Planar decals compose afterwards, preserving their authored coverage. Global-color modulation and three physical mips
remain enabled. The Base Pass shader and its sample count are unchanged.

Height and projection-direction normals come from a fixed reference tile level,
independent of geometry morph and VT page size. This prevents the side UV phase
from shifting as the camera approaches. The whole reference fits at most 256
tiles (one quarter of the existing 1024-layer source cache); a 4 km, 2x2-sector
terrain with 129-sample tiles uses L4 / 2 m spacing. References use the existing
height/normal arrays; the only additional GPU allocation is a lookup texture of
at most 4 KiB. The corresponding height/splat/normal tiles occupy up to 24.4 MiB
of the already allocated source pool, reducing space available for other tiles.
Initial background loading therefore adds I/O and upload work. All references
are protected and published together, followed by one VT invalidation; the log
`Cliff reference ready` marks publication. Until then the baseline stays valid.
Without a material override, disabling projection allows references to be evicted;
reenabling it waits for
a complete reference again. Stationary frames retain the sync-cache fast path.

Patch height ranges conservatively estimate surface stretch and request up to
two finer VT levels (4x linear density). At asset load, the maximum local stretch
is propagated from fine height-range nodes to their ancestors; a distant region
must not average away a narrow cliff by dividing its height range by a much
larger width. This uses one additional float per range node (about 341 KiB for
a 2x2-sector, L0-L7 dataset), with no extra texture or asset reimport.
Required VT pages inherit their visible patch's surface-footprint priority;
each fallback ancestor halves that priority instead of inflating it using the
ancestor's larger world size. Permanent roots remain available during updates.
Atlas capacity and per-frame update
budget stay fixed. This may increase patch/model count and cache pressure, and
does not guarantee every requested page fits. Geometry-cell containment, cache
fallback, fixed reference resolution and the top-down atlas still limit extreme
close-up cliffs. Neither overhang geometry nor a separate cliff mesh is added.

Costs concentrate in VT generation and camera movement: each active projection
can sample up to three albedo/height and three normal/roughness/AO textures.
This trades more page-generation work for stable, noise-free blending rather
than promising a free performance improvement. Compare stationary and moving
native GPU/CPU frame times after the reference and fine VT pages have settled.
The existing GPU regression harness also checks side UV height decoding, signed
axes, projection phase across page sizes, reference seams, normal-mode color invariance,
height blending, flat-ground equivalence and decal composition over cliffs.

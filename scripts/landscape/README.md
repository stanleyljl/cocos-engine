# Landscape Asset Tools

## Generating paired splat tiles

```powershell
node scripts/landscape/splatmap-tiles.js --manifest <dataset>/<name>.json --output <new-staging-directory>
```

The demo preset uses the existing natural material sequence 00 through 11;
layer 12 (artificial stone path) is retained but excluded from automatic splats.
When layers 13 (`coral_ground_02`) and 14 (`brown_mud_leaves_01`) are present,
a continuous 220m regional mask adds pale debris on low/mid-elevation gentle
slopes and leafy soil on lower, gentler ground. The strongest two contributions
are retained and normalized to fit the packed format. Relative elevation selects soil,
grass, gravel and exposed rock, with slopes from 25 to 55 degrees increasingly
favoring rock. Continuous world-space noise at 320m and 96m scales breaks up
elevation bands. `--seed` defaults to 1337. The tool validates the material names
and derives heights from the existing tiles, so the original source PNG is not
required. `--height-root <dataset-directory>` allows reading height tiles from
the original dataset when `--manifest` points to a staged material update.
This is an artistic terrain distribution, not an ecological simulation.

Each `nodes/L<level>/h_<x>_<z>.png` gets a paired `s_<x>_<z>.png` with the same
129x129 resolution, orientation and coverage. In the current demo, L3 samples
every 1m; L4 through L7 sample every 2/4/8/16m. Shared border samples and samples
at matching positions across levels come from one global classification field.

Splat PNGs are **16-bit grayscale**, decoded/uploaded as **R16UI**:

| Bits | Value |
| --- | --- |
| 0..4 | First material layer ID |
| 5..9 | Second (upper) material layer ID |
| 10..15 | Second layer weight, divided by 63 |

The first layer has weight `1 - secondWeight`. IDs refer to `materialLibrary.layers`.
The current preset blends adjacent IDs for continuous transitions, but the
encoding allows any pair of IDs from 0 through 31.

The output contains `nodes/`, an updated manifest with `splatMap`, the original
`manifest.before.json`, and `splat-report.json` with coverage and validation results.
Every output PNG is decoded and compared against the intended uint16 samples;
height tile borders and the height pyramid are checked as well. After validation,
copy the splat PNGs and updated manifest into the dataset. Regenerate splats after
changing height data or reordering materials. The tool never modifies its input.

The native renderer uses one `TilePagePool` for both height and splat. One job
loads the pair; both textures upload to the same array layer before it becomes
resident. Upload budgets count pairs. LRU eviction, parent fallback and F6
freezing therefore apply to both textures together. A failed pair never becomes
resident. The loader requires the manifest's `splatMap` encoding and validates
that decoded layer IDs exist in the material library.

`MaterialLibrary` uploads all material layers into two RGBA8 texture arrays with
mip chains. The shader fetches four neighboring splat texels using an unsigned
integer sampler with point filtering and no mipmaps, then combines the pair
weights with spatial bilinear weights. Up to eight IDs can contribute; duplicate
IDs are merged before material sampling. Albedo blends in linear space; normals
are reconstructed and renormalized after blending. Roughness and AO blend with
the same weights. Each layer uses its own `uvScale`, `detailHeightScale` and
`detailHeightBias`; vertex displacement uses the same splat interpolation at
fixed material mip 0 and remains excluded from bounds and LOD calculations.
Material displacement defaults to off. F7 toggles it live (including while F6
freezes LOD), without changing per-layer height parameters. LOD debug colors
replace albedo while retaining lighting. No virtual texture
composition is used. Coarser splat levels must come from the global field,
never from averaging packed uint16 values.

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

Use `--layers 13,14` to pack only newly added layers, preserving other manifest
entries and their existing PNGs. Numbered ZIPs remain contiguous; new layer names
are discovered from the archive's albedo filename, and existing names are
validated against the manifest. New layers inherit layer 0's `uvScale` (0.01 in
the current dataset), with detail height scale 1.0 and bias 0.0. The staged manifest
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
`detailHeightScale` and `detailHeightBias`. Both detail-height parameters default
to zero; repacking preserves existing parameters by material name. Re-running
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
  --out D:\work\editors\projects\landscape\assets\landscape `
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
  --out D:\work\editors\projects\landscape\assets\landscape `
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
  --manifest D:\work\editors\projects\landscape\assets\landscape\heightmap-demo\heightmap-demo.json
```

It scans every `h_<x>_<y>.png`, computes each node's `min/max`, merges bottom-up,
and writes `heightRange` back into the manifest. Refresh the project assets so
the running app reloads the patched manifest.

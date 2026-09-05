# Landscape Height Tiles

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

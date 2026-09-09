# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2,<3", "OpenEXR>=3.3,<4", "Pillow>=11,<13", "pypng==0.20220715.0"]
# ///
"""Pack source ZIP materials into RGBA8 PNGs without color or alpha transforms.

Run with uv run. Output is a staging directory containing textures/ and an
updated copy of the supplied manifest. Source assets are never overwritten.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import tempfile
import zipfile

import numpy as np
import OpenEXR
from PIL import Image
import png


def decode(data, suffix, scratch):
    if suffix == ".exr":
        path = scratch / "source.exr"
        path.write_bytes(data)
        with OpenEXR.File(str(path), separate_channels=True) as image:
            if len(image.parts) != 1:
                raise ValueError("Expected a single-part EXR")
            channels = image.channels()
            names = ["R", "G", "B"] if "R" in channels else ["Y"]
            return np.stack([channels[n].pixels for n in names], axis=-1).astype(np.float32)
    if suffix == ".png":
        # Pillow can reduce 16-bit multichannel PNGs to 8-bit on load. Decode
        # original samples here so height uses rounding, rather than truncation.
        width, height, rows, info = png.Reader(bytes=data).asDirect()
        pixels = np.vstack([np.asarray(row, dtype=np.float32) for row in rows])
        return pixels.reshape(height, width, info["planes"]) / ((1 << info["bitdepth"]) - 1)
    with Image.open(io.BytesIO(data)) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0


def quantize(pixels, label, allow_clip=False):
    if not np.all(np.isfinite(pixels)):
        raise ValueError(f"{label}: non-finite samples")
    minimum, maximum = float(pixels.min()), float(pixels.max())
    if not allow_clip and (minimum < -0.001 or maximum > 1.001):
        raise ValueError(f"{label}: expected normalized [0,1] samples, got {minimum}..{maximum}")
    # No per-image auto-normalization, gamma, exposure, or premultiplication.
    clipped = np.clip(pixels, 0.0, 1.0)
    result = np.floor(clipped * 255.0 + 0.5).astype(np.uint8)
    error = float(np.max(np.abs(result.astype(np.float32) / 255.0 - clipped)))
    return result, {"min": minimum, "max": maximum, "maxQuantizationError": error,
                    "clippedSamples": int(np.count_nonzero(pixels != clipped))}


def find_source(entries, tokens, required=True):
    matches = [n for n in entries if Path(n).suffix.lower() in (".png", ".jpg", ".jpeg", ".exr")
               and any(f"_{token}_" in Path(n).stem.lower() for token in tokens)]
    if len(matches) != 1:
        if not matches and not required:
            return None
        raise ValueError(f"Expected one {tokens} source, got {matches}")
    return matches[0]


def save_verified(path, pixels):
    Image.fromarray(pixels).save(path, format="PNG", compress_level=6)
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[24:26] != bytes((8, 6)):
        raise ValueError(f"{path}: not an 8-bit RGBA PNG")
    width, height, rows, info = png.Reader(bytes=data).asDirect()
    decoded = np.vstack([np.asarray(row, dtype=np.uint8) for row in rows]).reshape(height, width, 4)
    if info["bitdepth"] != 8 or not np.array_equal(decoded, pixels):
        raise ValueError(f"{path}: PNG round-trip changed channel data")
    return hashlib.sha256(data).hexdigest()


def replace_library(text, library):
    match = re.search(r'(?m)^\s*"materialLibrary":\s*', text)
    if match is None:
        raise ValueError("Manifest has no materialLibrary field")
    _, end = json.JSONDecoder().raw_decode(text, match.end())
    eol = "\r\n" if "\r\n" in text else "\n"
    replacement = json.dumps(library, ensure_ascii=False, indent=2).replace("\n", eol + "  ")
    return text[:match.end()] + replacement + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resolution", type=int, default=1024)
    parser.add_argument("--layers", help="Comma-separated layer IDs to pack; preserve other manifest layers")
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError("Output staging directory must be empty")
    archives = sorted(args.raw.glob("*.zip"), key=lambda p: p.name.removesuffix("_1k.blend.zip"))
    if not 1 <= len(archives) <= 32:
        raise ValueError(f"Expected 1..32 material archives, got {len(archives)}")
    manifest_text = args.manifest.read_bytes().decode("utf-8")
    original = json.loads(manifest_text)
    previous = {layer.get("name", Path(layer.get("file", "")).stem): layer
                for layer in original.get("materialLibrary", {}).get("layers", [])}
    previous_by_id = {layer["id"]: layer for layer in previous.values()}
    archive_names = []
    for index, archive in enumerate(archives):
        if re.fullmatch(r"\d{2}\.zip", archive.name):
            if int(archive.stem) != index:
                raise ValueError(f"{archive.name}: numbered ZIPs must be contiguous")
            with zipfile.ZipFile(archive) as source:
                albedo = find_source(source.namelist(), ["diff", "col"])
                name = re.sub(r"_(?:diff|col)_\d+k$", "", Path(albedo).stem)
            if index in previous_by_id and name != previous_by_id[index]["name"]:
                raise ValueError(f"{archive.name}: source name differs from manifest")
            archive_names.append(name)
        else:
            archive_names.append(archive.name.removesuffix("_1k.blend.zip"))
    selected = set(range(len(archives))) if args.layers is None else {int(v) for v in args.layers.split(",")}
    if not selected or not selected.issubset(range(len(archives))):
        raise ValueError("--layers contains invalid IDs")
    textures = args.output / "textures"
    textures.mkdir(parents=True)
    library = {"count": len(archives), "dir": "textures", "resolution": args.resolution,
               "format": "RGBA8", "layers": []}
    report = []
    with tempfile.TemporaryDirectory(prefix="landscape-exr-") as temp:
        for index, archive in enumerate(archives):
            name = archive_names[index]
            if index not in selected:
                old = previous_by_id.get(index)
                if old is None or old["name"] != name:
                    raise ValueError(f"Layer {index} must be packed before it can be preserved")
                for key in ("albedoHeight", "normalRoughnessAO"):
                    if not (args.manifest.parent / original["materialLibrary"]["dir"] / old[key]).is_file():
                        raise ValueError(f"Missing preserved texture: {old[key]}")
                library["layers"].append(old.copy())
                continue
            stats, decoded, source_names = {}, {}, {}
            with zipfile.ZipFile(archive) as source:
                for role, tokens in {"albedo": ["diff", "col"], "height": ["disp", "height"],
                                     "normal": ["nor_gl"], "roughness": ["rough"], "ao": ["ao"]}.items():
                    entry = find_source(source.namelist(), tokens, required=role != "ao")
                    if entry is None:
                        decoded[role] = np.full((args.resolution, args.resolution, 1), 255, dtype=np.uint8)
                        stats[role] = {"default": 1.0}
                        continue
                    source_names[role] = entry
                    pixels = decode(source.read(entry), Path(entry).suffix.lower(), Path(temp))
                    if pixels.shape[:2] != (args.resolution, args.resolution):
                        raise ValueError(f"{entry}: resolution mismatch {pixels.shape[:2]}")
                    if role in ("height", "roughness", "ao"):
                        # Grayscale/gray+alpha: first plane is the data, not alpha.
                        # RGB scalar maps must agree across RGB; never mix in alpha.
                        if pixels.shape[-1] >= 3 and np.max(np.abs(pixels[..., :3] - pixels[..., :1])) > 0.001:
                            raise ValueError(f"{entry}: scalar map has different RGB values")
                        pixels = pixels[..., :1]
                    elif pixels.shape[-1] < 3:
                        raise ValueError(f"{entry}: expected RGB")
                    elif role == "normal":
                        normal = pixels[..., :3] * 2.0 - 1.0
                        lengths = np.linalg.norm(normal, axis=-1, keepdims=True)
                        if not np.all(np.isfinite(lengths)) or np.any(lengths < 1e-8):
                            raise ValueError(f"{entry}: non-finite or zero-length normal")
                        stats["normalSource"] = {
                            "minLength": float(lengths.min()), "maxLength": float(lengths.max()),
                            "negativeZPixels": int(np.count_nonzero(normal[..., 2] < 0)),
                        }
                        # The agreed XY layout reconstructs positive Z. Normalize
                        # filtered EXR normals before discarding Z; reflect negative
                        # Z into the representable hemisphere without changing XY.
                        normal[..., 2] = np.abs(normal[..., 2])
                        normal /= lengths
                        pixels = normal[..., :2] * 0.5 + 0.5
                    else:
                        pixels = pixels[..., :3]
                    decoded[role], stats[role] = quantize(pixels, entry, allow_clip=role == "roughness")
            ah = np.concatenate((decoded["albedo"], decoded["height"]), axis=-1)
            nra = np.concatenate((decoded["normal"], decoded["roughness"], decoded["ao"]), axis=-1)
            ah_name, nra_name = f"AlbedoHeight_{index:02d}.png", f"NormalRoughnessAO_{index:02d}.png"
            hashes = {ah_name: save_verified(textures / ah_name, ah),
                      nra_name: save_verified(textures / nra_name, nra)}
            old = previous.get(name, {})
            library["layers"].append({"id": index, "name": name, "albedoHeight": ah_name,
                                      "normalRoughnessAO": nra_name,
                                      "uvScale": old.get("uvScale", previous_by_id.get(0, {}).get("uvScale", 0.1)),
                                      "detailHeightScale": old.get("detailHeightScale", 1.0),
                                      "detailHeightBias": old.get("detailHeightBias", 0.0)})
            report.append({"id": index, "name": name, "sourceArchive": archive.name,
                           "sources": source_names, "statistics": stats, "sha256": hashes})
            print(f"{index:02d} {name}: {ah_name}, {nra_name}", flush=True)
    updated_text = replace_library(manifest_text, library)
    updated = json.loads(updated_text)
    if {k: v for k, v in updated.items() if k != "materialLibrary"} != {
            k: v for k, v in original.items() if k != "materialLibrary"}:
        raise ValueError("Non-material manifest data changed")
    (args.output / args.manifest.name).write_bytes(updated_text.encode("utf-8"))
    (args.output / "packing-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output / "manifest-before.json").write_bytes(manifest_text.encode("utf-8"))
    print(f"Verified {len(report) * 2} RGBA8 PNGs at {args.resolution}x{args.resolution}.")


if __name__ == "__main__":
    main()

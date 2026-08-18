#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Repack a GLB: drop what nothing references, then emit both required variants.

glTF-Sample-Assets requires every model to ship a `glTF/` variant (separate
.gltf + .bin + image files), not just `glTF-Binary/`. It also runs the
glTF-Validator in CI, and while UNUSED_OBJECT is only an "info", shipping an
asset that carries a texture no material samples and vertex attributes no shader
reads is the kind of thing a reviewer asks about — reasonably, since a
conformance asset is supposed to be a precise statement of what it tests.

Our sweep builder emits one mesh layout for every asset (position, normal, uv,
tangent) plus a gradient image, because it is shared with the in-tree grid that
DOES use them. Most conformance assets here use neither. So rather than fork the
builder — and risk moving the frozen regression baseline it also emits — the
pruning happens here, downstream, on the finished GLB.

Approach is a mark-and-sweep over the glTF object graph: start from the scene,
mark every accessor / bufferView / image / texture / sampler that is genuinely
reachable, drop the rest, and rebuild the binary payload so the surviving
bufferViews are contiguous and 4-byte aligned. Both variants are then written
from the SAME pruned document, so they cannot disagree about texture indices.
"""

import json
import struct

_GLB_MAGIC = b"glTF"
_CHUNK_JSON = 0x4E4F534A
_CHUNK_BIN = 0x004E4942


def parse_glb(data):
    """-> (gltf_dict, bin_bytes)."""
    assert data[:4] == _GLB_MAGIC, "not a GLB"
    total = struct.unpack("<I", data[8:12])[0]
    assert total == len(data), "GLB length header disagrees with file size"
    pos, js, binary = 12, None, b""
    while pos < len(data):
        clen, ctype = struct.unpack("<II", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + clen]
        if ctype == _CHUNK_JSON:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == _CHUNK_BIN:
            binary = chunk
        pos += 8 + clen
    return js, binary


def _texture_indices(node):
    """Every textureInfo index reachable in an arbitrary material subtree.

    Walks generically instead of naming known properties, because the whole point
    of these assets is DRAFT extensions whose property names are still moving —
    a hardcoded list would silently miss coatNormalTexture the day it is renamed.
    """
    found = set()
    if isinstance(node, dict):
        for k, v in node.items():
            if (k.endswith("Texture") or k.endswith("texture")) \
                    and isinstance(v, dict) and "index" in v:
                found.add(v["index"])
            else:
                found |= _texture_indices(v)
    elif isinstance(node, list):
        for v in node:
            found |= _texture_indices(v)
    return found


def _retarget(node, tex_map):
    """Rewrite textureInfo.index in place through tex_map."""
    if isinstance(node, dict):
        for k, v in node.items():
            if (k.endswith("Texture") or k.endswith("texture")) \
                    and isinstance(v, dict) and "index" in v:
                v["index"] = tex_map[v["index"]]
            else:
                _retarget(v, tex_map)
    elif isinstance(node, list):
        for v in node:
            _retarget(v, tex_map)


def prune(gltf, binary, drop_attributes=()):
    """Mark-and-sweep. drop_attributes removes named mesh attributes first."""
    gltf = json.loads(json.dumps(gltf))          # own it; caller keeps theirs

    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            for attr in drop_attributes:
                prim.get("attributes", {}).pop(attr, None)

    # --- textures / images / samplers -------------------------------------
    used_tex = _texture_indices(gltf.get("materials", []))
    old_tex = gltf.get("textures", [])
    keep_tex = sorted(used_tex)
    tex_map = {old: new for new, old in enumerate(keep_tex)}
    new_tex = [old_tex[i] for i in keep_tex]
    _retarget(gltf.get("materials", []), tex_map)

    old_img = gltf.get("images", [])
    used_img = sorted({t["source"] for t in new_tex if "source" in t})
    img_map = {old: new for new, old in enumerate(used_img)}
    new_img = [old_img[i] for i in used_img]

    old_smp = gltf.get("samplers", [])
    used_smp = sorted({t["sampler"] for t in new_tex if "sampler" in t})
    smp_map = {old: new for new, old in enumerate(used_smp)}
    new_smp = [old_smp[i] for i in used_smp]

    for t in new_tex:
        if "source" in t:
            t["source"] = img_map[t["source"]]
        if "sampler" in t:
            t["sampler"] = smp_map[t["sampler"]]

    # --- accessors ---------------------------------------------------------
    used_acc = set()
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            used_acc |= set(prim.get("attributes", {}).values())
            if "indices" in prim:
                used_acc.add(prim["indices"])
    old_acc = gltf.get("accessors", [])
    keep_acc = sorted(used_acc)
    acc_map = {old: new for new, old in enumerate(keep_acc)}
    new_acc = [dict(old_acc[i]) for i in keep_acc]

    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            prim["attributes"] = {k: acc_map[v]
                                  for k, v in prim.get("attributes", {}).items()}
            if "indices" in prim:
                prim["indices"] = acc_map[prim["indices"]]

    # --- bufferViews: rebuild the payload ----------------------------------
    old_bv = gltf.get("bufferViews", [])
    used_bv = sorted({a["bufferView"] for a in new_acc if "bufferView" in a}
                     | {i["bufferView"] for i in new_img if "bufferView" in i})
    bv_map = {old: new for new, old in enumerate(used_bv)}

    blob = bytearray()
    new_bv = []
    for old in used_bv:
        v = dict(old_bv[old])
        start = v.get("byteOffset", 0)
        data = binary[start:start + v["byteLength"]]
        while len(blob) % 4:                      # every view 4-byte aligned
            blob.append(0)
        v["byteOffset"] = len(blob)
        blob.extend(data)
        new_bv.append(v)

    for a in new_acc:
        if "bufferView" in a:
            a["bufferView"] = bv_map[a["bufferView"]]
    for i in new_img:
        if "bufferView" in i:
            i["bufferView"] = bv_map[i["bufferView"]]

    for key, val in (("textures", new_tex), ("images", new_img),
                     ("samplers", new_smp), ("accessors", new_acc),
                     ("bufferViews", new_bv)):
        if val:
            gltf[key] = val
        else:
            gltf.pop(key, None)

    gltf["buffers"] = [{"byteLength": len(blob)}]
    return gltf, bytes(blob)


def write_glb(gltf, binary):
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    bn = binary + b"\x00" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(js) + (8 + len(bn) if bn else 0)
    out = bytearray()
    out += _GLB_MAGIC + struct.pack("<II", 2, total)
    out += struct.pack("<II", len(js), _CHUNK_JSON) + js
    if bn:
        out += struct.pack("<II", len(bn), _CHUNK_BIN) + bn
    return bytes(out)


def write_gltf_variant(gltf, binary, outdir, stem):
    """Separate .gltf + .bin + image files — the required `glTF/` variant.

    Images move OUT of the buffer into their own files, so their bufferViews are
    dropped and the payload is rebuilt without them; otherwise the image bytes
    would ship twice and the validator would flag the orphaned views.
    """
    gltf = json.loads(json.dumps(gltf))
    outdir.mkdir(parents=True, exist_ok=True)

    image_bytes = []
    for n, img in enumerate(gltf.get("images", [])):
        bv = gltf["bufferViews"][img["bufferView"]]
        start = bv.get("byteOffset", 0)
        image_bytes.append((n, binary[start:start + bv["byteLength"]]))

    keep_bv = sorted({a["bufferView"] for a in gltf.get("accessors", [])
                      if "bufferView" in a})
    bv_map = {old: new for new, old in enumerate(keep_bv)}
    blob = bytearray()
    new_bv = []
    for old in keep_bv:
        v = dict(gltf["bufferViews"][old])
        start = v.get("byteOffset", 0)
        data = binary[start:start + v["byteLength"]]
        while len(blob) % 4:
            blob.append(0)
        v["byteOffset"] = len(blob)
        blob.extend(data)
        new_bv.append(v)
    for a in gltf.get("accessors", []):
        if "bufferView" in a:
            a["bufferView"] = bv_map[a["bufferView"]]

    for n, data in image_bytes:
        fn = "%s_%d.png" % (stem, n)
        (outdir / fn).write_bytes(data)
        gltf["images"][n] = {"uri": fn}

    gltf["bufferViews"] = new_bv
    gltf["buffers"] = [{"byteLength": len(blob), "uri": stem + ".bin"}]
    (outdir / (stem + ".bin")).write_bytes(bytes(blob))
    (outdir / (stem + ".gltf")).write_text(
        json.dumps(gltf, indent=2) + "\n", encoding="utf-8")
    return len(blob), len(image_bytes)

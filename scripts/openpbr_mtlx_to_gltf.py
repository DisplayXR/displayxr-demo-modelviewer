#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""
Convert objects from an OpenUSD scene with OpenPBR MaterialX materials into
glTF 2.0 (.glb) the model viewer can load. Part of issue #70.

Why this exists
---------------
The viewer's OpenPBR support is glTF-extension-only: it shades OpenPBR's lobes
once they arrive as KHR_materials_* (coat, fuzz, scatter, transmission, volume,
...), but has no MaterialX reader, and its USD backend reads UsdPreviewSurface
only. Real OpenPBR content -- the reference being the ASWF "OpenPBR Shader
Playground" scene (github.com/DigitalProductionExampleLibrary/
OpenPBRShaderPlayground) -- ships as USD + .mtlx node graphs, with no glTF
export. This script is that export, i.e. the "author in OpenPBR -> glTF ->
viewer" pipeline of docs/openpbr-to-gltf.md, made runnable.

What it does
------------
Materials are the .mtlx graph PLUS the inputs the scene's USD layers author over
it (usd_overrides()) -- the Playground re-sets 23 materials that way.

Geometry: every visible UsdGeomMesh under each /World/<group>, in world space,
metres, fan-triangulated, merged into one primitive per material, one .glb per
group (re-centred, resting on y=0).

Materials: the MaterialX graph feeding each open_pbr_surface input is EVALUATED
PER PIXEL in numpy (image / extract / invert / combine3 / normalmap / remap /
multiply), then baked into core glTF + the extensions above. Nothing is dropped
silently -- every unmapped input or unevaluated node is a WARN line.

Mapping decisions worth knowing (see docs/openpbr-to-gltf.md for the table):
  * base_weight is premultiplied into baseColorFactor.
  * Where a surface transmits, baseColor is baked as mix(base_color,
    transmission_color, transmission_weight): glTF tints transmitted light by
    baseColor, OpenPBR by transmission_color. Without it the Playground's
    painted mason jar went opaque teal and the bottle-plane lost its clear green.
  * A textured transmission_weight (no subsurface) becomes transmissionTexture.
  * subsurface + transmission both replace the diffuse lobe in OpenPBR
    (mix(mix(diffuse, sss, sw), transmission, tw)), so the glTF
    transmissionFactor is their union and KHR_materials_scatter carries the
    subsurface share; subsurface_radius (scene units) -> attenuationDistance.
  * Volume thickness is the thinnest bbox axis of that material's meshes -- a
    crude stand-in; OpenPBR has no thickness parameter.
  * thin_film_thickness is micrometres in OpenPBR, nanometres in glTF (x1000).
  * emission_luminance maps 1:1 to glTF emissive strength by default
    (--nits-per-unit 1). Arnold treats it as scene radiance in practice: the
    Playground's meetMAT peaks at ~2 and glows hard in the reference render.
  * Coat is written as KHR_materials_coat AND a KHR_materials_clearcoat
    fallback; fuzz likewise with a KHR_materials_sheen fallback.

UDIMs: glTF has none, so each mesh's UVs are moved back to [0,1) from its own
tile, and a material whose meshes span tiles is baked once per tile
("<name>.<tile>"). Exact unless a single mesh straddles tiles.

GeomSubsets: each subset's faces take its own material; unclaimed faces take the
mesh binding. A group may be an absolute path -- "/World" converts the scene.

Lights: UsdLux lights under a group are written as KHR_lights_punctual (see
usd_lights()), which the viewer draws in its Scene lighting mode.

Known limits: colorcorrect / ramp / heighttonormal
nodes pass their input through (warned); textured specular weights, and a
textured transmission weight combined with subsurface, are averaged (warned). Catmull-Clark meshes export their
cage, not the limit surface.

Requirements (a venv is easiest):  pip install usd-core numpy pillow tifffile imagecodecs
  (imagecodecs: the Playground's .tif textures are LZW-compressed.)
  --decimate additionally needs: pip install fast-simplification

Usage:
    python scripts/openpbr_mtlx_to_gltf.py <scene.usda> <outdir> \
        --groups mug_grp,lamp_grp,meetMAT_grp [--exclude pCube7,pCube28] [--max-tex 1024]

Slim recipe for the Playground (85 -> 50 MB for nine groups, each lever
measured in isolation with scripts/compare_captures.py against the unslimmed
render): --jpeg-quality 90 --decimate bubbles=0.2,bubblesMasonJar=0.2,OJ=0.4.
Bubbles and juice decimate for free (mean |diff| 0.000 -- the bubbles alone
were 18 MB); JPEG costs <= 0.09/255. NOT iceCube (1.1/255, visible) and not
OJglass (textured normal map -> the seam approximation shows as streaks).

The scene's textures need only be present for the groups you convert -- the
Playground is 1.9 GB, almost all of it textures, so fetch per-material rather
than cloning.

Licensing: the Playground is under the ASWF Digital Assets License v1.1
(demos/benchmarks OK, keep the notice). Every .glb this writes carries that
notice in asset.copyright. Do not commit converted assets without checking it.
"""
import argparse, io, json, os, struct, sys
import xml.etree.ElementTree as ET
import numpy as np
from PIL import Image

WARN = []
def warn(msg):
    WARN.append(msg); print("WARN", msg, file=sys.stderr)

# ---------------------------------------------------------------- colour helpers
def srgb_to_lin(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)
def lin_to_srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * x ** (1 / 2.4) - 0.055)

# ---------------------------------------------------------------- texture I/O
def load_image(path, max_tex):
    if path.lower().endswith((".tif", ".tiff")):
        import tifffile
        a = tifffile.imread(path)
    else:
        a = np.asarray(Image.open(path))
    if a.dtype == np.uint8:     a = a.astype(np.float32) / 255
    elif a.dtype == np.uint16:  a = a.astype(np.float32) / 65535
    else:                       a = a.astype(np.float32)
    if a.ndim == 2: a = a[..., None]
    a = a[..., :3] if a.shape[2] >= 3 else a[..., :1]
    h, w = a.shape[:2]
    s = min(1.0, max_tex / max(h, w))
    if s < 1.0:
        nw, nh = max(1, int(w * s)), max(1, int(h * s))
        a = np.stack([np.asarray(Image.fromarray(a[..., c]).resize((nw, nh), Image.BILINEAR))
                      for c in range(a.shape[2])], -1)
    return a

def match(a, b):
    """Broadcast two values (scalar/vec constants or HxWxC images) to a common grid."""
    if a.ndim == 3 and b.ndim == 3 and a.shape[:2] != b.shape[:2]:
        big = a if a.shape[0] * a.shape[1] >= b.shape[0] * b.shape[1] else b
        H, W = big.shape[:2]
        rs = lambda x: np.stack([np.asarray(Image.fromarray(x[..., c]).resize((W, H), Image.BILINEAR))
                                 for c in range(x.shape[2])], -1)
        a = a if a.shape[:2] == (H, W) else rs(a)
        b = b if b.shape[:2] == (H, W) else rs(b)
    return a, b

def is_img(v): return isinstance(v, np.ndarray) and v.ndim == 3

# ---------------------------------------------------------------- MaterialX eval
def parse_val(s, typ):
    if typ in ("float", "integer"): return np.array([float(s)], np.float32)
    if typ == "boolean": return s.strip().lower() == "true"
    if typ in ("color3", "vector3", "vector2", "color4"):
        return np.array([float(x) for x in s.split(",")], np.float32)
    return s

class Graph:
    def __init__(self, path, max_tex, tile=1001, overrides=None):
        txt = open(path).read().replace("<UDIM>", "@UDIM@")
        self.root = ET.fromstring(txt)
        self.dir = os.path.dirname(path)
        self.nodes = {n.get("name"): n for n in self.root}
        self.max_tex = max_tex
        self.tile = tile   # the UDIM tile this instance of the material is baked for
        # {node name: {input name: value}} authored over the .mtlx in USD layers
        # (usd_overrides()); applied per USD's rule, see inp().
        self.overrides = overrides or {}
        self.cache = {}

    def inp(self, node, name, default=None):
        """Last-wins, like MaterialX (the mug declares base_color twice).

        USD composition rule for the overrides: a CONNECTION authored in the
        .mtlx (weaker) still wins over a value authored by a stronger USD layer
        (UsdShadeInput resolves connections before values); an unconnected
        input takes the override."""
        hit = None
        for i in node.findall("input"):
            if i.get("name") == name: hit = i
        if hit is not None and hit.get("nodename"): return self.eval(hit.get("nodename"))
        ov = self.overrides.get(node.get("name"), {})
        if name in ov: return ov[name]
        if hit is None: return default
        return parse_val(hit.get("value"), hit.get("type"))

    def eval(self, name):
        if name in self.cache: return self.cache[name]
        n = self.nodes[name]; t = n.tag; typ = n.get("type")
        if t == "image":
            f = n.find("input[@name='file']")
            rel = self.overrides.get(name, {}).get("file") or f.get("value")
            if "@UDIM@" in rel:
                # glTF has no UDIMs. Each mesh's UVs are moved back to [0,1) from its
                # own tile (mesh_arrays), and a material is baked once PER TILE its
                # meshes use, so take this instance's tile. A texture that lacks it
                # (a mask painted on one tile only) falls back to the lowest present.
                import glob
                want = rel.replace("@UDIM@", str(self.tile))
                if os.path.exists(os.path.join(self.dir, want)):
                    rel = want
                else:
                    tiles = sorted(glob.glob(os.path.join(self.dir, rel.replace("@UDIM@", "[0-9][0-9][0-9][0-9]"))))
                    if tiles: warn(f"{name}: no tile {self.tile} - using {os.path.basename(tiles[0])}")
                    rel = os.path.relpath(tiles[0], self.dir) if tiles else want
            path = os.path.normpath(os.path.join(self.dir, rel))
            if not os.path.exists(path):
                warn(f"{name}: texture missing on disk ({rel}) - using node default")
                v = np.array([0.5], np.float32) if typ == "float" else np.array([0.5, 0.5, 0.5], np.float32)
            else:
                v = load_image(path, self.max_tex)
                if f.get("colorspace") == "srgb_tx": v = srgb_to_lin(v)
                if typ == "float": v = v[..., :1]
                elif v.shape[2] == 1: v = np.repeat(v, 3, 2)
        elif t == "extract":
            src = self.inp(n, "in"); idx = int(self.inp(n, "index", np.array([0]))[0])
            v = src[..., idx:idx + 1] if is_img(src) else src[idx:idx + 1]
        elif t == "invert":
            amt = self.inp(n, "amount", np.array([1.0], np.float32))
            v = amt - self.inp(n, "in")
        elif t == "combine3":
            a, b, c = (self.inp(n, k) for k in ("in1", "in2", "in3"))
            a, b = match(a, b); a, c = match(a, c); b, c = match(b, c)
            v = np.concatenate([a, b, c], -1)
        elif t == "normalmap":
            v = self.inp(n, "in")          # tangent-space, [0,1]-encoded, +Y up: glTF's convention
            s = self.inp(n, "scale", np.array([1.0]))
            if float(s[0]) != 1.0: warn(f"{name}: normalmap scale {s[0]} ignored")
        elif t == "remap":
            x = self.inp(n, "in")
            il = self.inp(n, "inlow", np.array([0.0])); ih = self.inp(n, "inhigh", np.array([1.0]))
            ol = self.inp(n, "outlow", np.array([0.0])); oh = self.inp(n, "outhigh", np.array([1.0]))
            v = ol + (x - il) / (ih - il) * (oh - ol)
        elif t == "multiply":
            a, b = match(self.inp(n, "in1"), self.inp(n, "in2")); v = a * b
        else:
            up = self.inp(n, "in")
            warn(f"{name}: <{t}> not evaluated - passing its 'in' through" if up is not None
                 else f"{name}: <{t}> not evaluated - input treated as unset")
            v = up
        self.cache[name] = v
        return v

    def surface(self):
        s = self.root.find("open_pbr_surface")
        if s is None: raise SystemExit("no open_pbr_surface")
        names = {i.get("name") for i in s.findall("input")} | set(self.overrides.get(s.get("name"), {}))
        return {k: self.inp(s, k) for k in names}

# ---------------------------------------------------------------- glTF writer
class GLB:
    def __init__(self):
        self.g = {"asset": {"version": "2.0", "generator": "openpbr_mtlx_to_gltf.py (DisplayXR model viewer)"},
                  "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
                  "materials": [], "accessors": [], "bufferViews": [], "buffers": [],
                  "images": [], "textures": [], "samplers": [{"wrapS": 10497, "wrapT": 10497}]}
        self.bin = bytearray(); self.used = set()
        self.jpeg_quality = 0; self.tex_cache = {}

    def view(self, data, target=None):
        while len(self.bin) % 4: self.bin += b"\0"
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target: bv["target"] = target
        self.bin += data; self.g["bufferViews"].append(bv)
        return len(self.g["bufferViews"]) - 1

    def accessor(self, arr, typ, target):
        # glTF's JSON cannot carry NaN/Inf (min/max would be invalid JSON and the
        # viewer rejects the whole file), so refuse rather than write a broken asset.
        if arr.dtype.kind == "f" and not np.isfinite(arr).all():
            raise ValueError(f"non-finite values in a {typ} accessor")
        comp = 5125 if arr.dtype == np.uint32 else 5126
        acc = {"bufferView": self.view(arr.tobytes(), target), "componentType": comp,
               "count": len(arr), "type": typ}
        if typ == "VEC3" and comp == 5126:
            acc["min"] = arr.min(0).tolist(); acc["max"] = arr.max(0).tolist()
        self.g["accessors"].append(acc)
        return len(self.g["accessors"]) - 1

    def texture(self, img01, name, colour=False):
        """img01: HxWxC float in [0,1], already in the encoding glTF wants for its slot.

        `colour` marks an sRGB colour slot (baseColor, emissive). Those go out as
        JPEG when --jpeg-quality is set and there is no alpha; data textures
        (normals, metal/rough, masks) always stay lossless PNG, where block
        artefacts would read as bumps or roughness noise. Byte-identical images
        are stored once (a material often feeds one map to two inputs)."""
        a = (np.clip(img01, 0, 1) * 255 + 0.5).astype(np.uint8)
        if a.shape[2] == 1: a = a[..., 0]
        buf = io.BytesIO()
        if colour and self.jpeg_quality and a.ndim == 3 and a.shape[2] == 3:
            Image.fromarray(a).save(buf, "JPEG", quality=self.jpeg_quality, optimize=True)
            mime = "image/jpeg"
        else:
            Image.fromarray(a).save(buf, "PNG", optimize=True); mime = "image/png"
        data = buf.getvalue()
        if data in self.tex_cache: return {"index": self.tex_cache[data]}
        self.g["images"].append({"name": name, "mimeType": mime, "bufferView": self.view(data)})
        self.g["textures"].append({"sampler": 0, "source": len(self.g["images"]) - 1})
        self.tex_cache[data] = len(self.g["textures"]) - 1
        return {"index": self.tex_cache[data]}

    def write(self, path):
        for k in [k for k, v in self.g.items() if v == []]: del self.g[k]
        self.g["buffers"] = [{"byteLength": len(self.bin)}]
        if self.used: self.g["extensionsUsed"] = sorted(self.used)
        js = json.dumps(self.g, separators=(",", ":")).encode()
        js += b" " * ((4 - len(js) % 4) % 4)
        while len(self.bin) % 4: self.bin += b"\0"
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
            f.write(struct.pack("<II", len(self.bin), 0x004E4942)); f.write(self.bin)

# ---------------------------------------------------------------- OpenPBR -> glTF
D = {  # OpenPBR 1.1 defaults, for the inputs this mapping reads
    "base_weight": 1.0, "base_color": [0.8] * 3, "base_metalness": 0.0, "base_diffuse_roughness": 0.0,
    "specular_weight": 1.0, "specular_color": [1.0] * 3, "specular_roughness": 0.3, "specular_ior": 1.5,
    "specular_roughness_anisotropy": 0.0,
    "transmission_weight": 0.0, "transmission_color": [1.0] * 3, "transmission_depth": 0.0,
    "subsurface_weight": 0.0, "subsurface_color": [0.8] * 3, "subsurface_radius": 1.0,
    "subsurface_radius_scale": [1.0, 0.5, 0.25], "subsurface_scatter_anisotropy": 0.0,
    "coat_weight": 0.0, "coat_color": [1.0] * 3, "coat_roughness": 0.0, "coat_ior": 1.6, "coat_darkening": 1.0,
    "fuzz_weight": 0.0, "fuzz_color": [1.0] * 3, "fuzz_roughness": 0.5,
    "thin_film_weight": 0.0, "thin_film_thickness": 0.5, "thin_film_ior": 1.4,
    "emission_luminance": 0.0, "emission_color": [1.0] * 3,
    "geometry_opacity": 1.0, "geometry_thin_walled": False,
}
HANDLED = set(D) | {"geometry_normal"}

def const(v):  return v is not None and not is_img(v)
def scal(v):   return float(np.mean(v))
def vec3(v):   v = np.asarray(v, np.float32).reshape(-1); return (np.repeat(v, 3) if v.size == 1 else v[:3]).tolist()
def as3(v):    return v if v.shape[-1] == 3 else np.repeat(v, 3, -1)

def build_material(glb, name, p, mpu, thickness_m, nits_per_unit):
    get = lambda k: p.get(k) if p.get(k) is not None else np.asarray(D[k], np.float32) if not isinstance(D[k], bool) else D[k]
    for k in sorted(set(p) - HANDLED):
        warn(f"{name}: OpenPBR input '{k}' has no mapping here - dropped")
    m = {"name": name, "pbrMetallicRoughness": {}, "extensions": {}}
    pbr = m["pbrMetallicRoughness"]; ext = m["extensions"]
    def use(e, body): ext[e] = body; glb.used.add(e)

    # base colour (premultiplied by base_weight) + opacity
    bc = get("base_color") * get("base_weight"); op = get("geometry_opacity")
    # glTF tints TRANSMITTED light by baseColor; OpenPBR tints it by
    # transmission_color and uses base_color only for the diffuse it replaces.
    # So where a surface transmits, glTF's baseColor must BE transmission_color:
    # bake mix(base_color, transmission_color, transmission_weight). Exact at
    # weight 0 and 1. Skipped with subsurface, whose colour rides the scatter
    # extension instead. (Painted glass: without this the paint's base colour
    # tinted the whole jar.)
    tw_ = get("transmission_weight")
    if (is_img(tw_) or scal(tw_) > 0) and not is_img(get("subsurface_weight")) and scal(get("subsurface_weight")) == 0:
        tc = get("transmission_color")
        tcol = as3(tc) if is_img(tc) else np.float32(vec3(tc))
        base = as3(bc) if is_img(bc) else np.float32(vec3(bc))
        w = tw_[..., :1] if is_img(tw_) else np.float32(scal(tw_))
        if is_img(base) and is_img(w): base, w = match(base, w)
        if is_img(tcol) and is_img(base): tcol, base = match(tcol, base)
        bc = base * (1 - w) + tcol * w
        if not is_img(bc): bc = np.asarray(bc, np.float32).reshape(-1)
    if is_img(bc) or is_img(op):
        bc3 = as3(bc) if is_img(bc) else np.broadcast_to(vec3(bc), (1, 1, 3)).astype(np.float32)
        tex = lin_to_srgb(bc3)
        if is_img(op):
            tex, op = match(tex if is_img(bc) else np.broadcast_to(tex, op.shape[:2] + (3,)).copy(), op)
            tex = np.concatenate([tex, op[..., :1]], -1)
        pbr["baseColorTexture"] = glb.texture(tex, f"{name}_baseColor", colour=True)
        pbr["baseColorFactor"] = [1, 1, 1, 1 if is_img(op) else scal(op)]
    else:
        pbr["baseColorFactor"] = vec3(bc) + [scal(op)]
    if (is_img(op) or scal(op) < 1.0): m["alphaMode"] = "BLEND"

    # metallic / roughness
    r, mt = get("specular_roughness"), get("base_metalness")
    if is_img(r) or is_img(mt):
        rr = r if is_img(r) else np.full((1, 1, 1), scal(r), np.float32)
        mm = mt if is_img(mt) else np.full((1, 1, 1), scal(mt), np.float32)
        rr, mm = match(rr, mm)
        if rr.shape[:2] != mm.shape[:2]:   # one side is a 1x1 constant
            H, W = max(rr.shape[0], mm.shape[0]), max(rr.shape[1], mm.shape[1])
            rr = np.broadcast_to(rr, (H, W, 1)); mm = np.broadcast_to(mm, (H, W, 1))
        mr = np.concatenate([np.zeros_like(rr), rr, mm], -1)
        pbr["metallicRoughnessTexture"] = glb.texture(mr, f"{name}_metalRough")
        pbr["roughnessFactor"] = 1.0; pbr["metallicFactor"] = 1.0
    else:
        pbr["roughnessFactor"] = scal(r); pbr["metallicFactor"] = scal(mt)

    # normal
    n = p.get("geometry_normal")
    if is_img(n): m["normalTexture"] = glb.texture(n, f"{name}_normal")

    # specular / ior / anisotropy / diffuse roughness
    sw, sc = get("specular_weight"), get("specular_color")
    if not (const(sw) and const(sc)): warn(f"{name}: textured specular weight/colour not baked - using mean")
    if abs(scal(sw) - 1) > 1e-4 or any(abs(c - 1) > 1e-4 for c in vec3(np.mean(sc.reshape(-1, sc.shape[-1]), 0) if is_img(sc) else sc)):
        use("KHR_materials_specular", {"specularFactor": scal(sw), "specularColorFactor": vec3(sc if const(sc) else np.mean(sc.reshape(-1, 3), 0))})
    ior = scal(get("specular_ior"))
    if abs(ior - 1.5) > 1e-4: use("KHR_materials_ior", {"ior": ior})
    an = scal(get("specular_roughness_anisotropy"))
    if an > 0: use("KHR_materials_anisotropy", {"anisotropyStrength": an})
    dr = get("base_diffuse_roughness")
    if scal(dr) > 0: use("KHR_materials_diffuse_roughness", {"diffuseRoughnessFactor": scal(dr)})

    # coat (+ clearcoat fallback for viewers without the draft)
    cw = get("coat_weight")
    if is_img(cw) or scal(cw) > 0:
        body = {"coatRoughnessFactor": scal(get("coat_roughness")), "coatIor": scal(get("coat_ior")),
                "coatColorFactor": vec3(get("coat_color")), "coatDarkeningFactor": scal(get("coat_darkening"))}
        cc = {"clearcoatRoughnessFactor": body["coatRoughnessFactor"]}
        if is_img(cw):
            t = glb.texture(cw[..., :1], f"{name}_coatWeight")
            body["coatFactor"] = 1.0; body["coatTexture"] = t
            cc["clearcoatFactor"] = 1.0; cc["clearcoatTexture"] = t
        else:
            body["coatFactor"] = cc["clearcoatFactor"] = scal(cw)
        use("KHR_materials_coat", body); use("KHR_materials_clearcoat", cc)

    # fuzz (+ sheen fallback)
    fw = get("fuzz_weight")
    if is_img(fw) or scal(fw) > 0:
        fc, fr = vec3(get("fuzz_color")), scal(get("fuzz_roughness"))
        body = {"fuzzColorFactor": fc, "fuzzRoughnessFactor": fr}
        if is_img(fw): body["fuzzFactor"] = 1.0; body["fuzzTexture"] = glb.texture(fw[..., :1], f"{name}_fuzz")
        else: body["fuzzFactor"] = scal(fw)
        use("KHR_materials_fuzz", body)
        use("KHR_materials_sheen", {"sheenColorFactor": [c * scal(fw) for c in fc], "sheenRoughnessFactor": fr})

    # transmission + subsurface. OpenPBR's dielectric base is
    #   mix(mix(diffuse, subsurface, sw), transmission, tw)
    # Both replace the diffuse lobe, so the glTF transmission weight is their union
    # and KHR_materials_scatter says what share of it is scattered.
    tw, sw_ = get("transmission_weight"), get("subsurface_weight")
    # A textured transmission weight WITHOUT subsurface maps straight onto glTF's
    # transmissionTexture (R) -- e.g. clear glass with painted-on smears, where the
    # mean would turn the whole jar semi-opaque. Mixed with subsurface the union
    # below is nonlinear in the two, so that case still averages.
    tw_tex = None
    if is_img(tw) and not is_img(sw_) and scal(sw_) == 0:
        tw_tex = glb.texture(tw[..., :1], f"{name}_transmission")
        tw = np.float32(1.0)
    elif is_img(tw) or is_img(sw_):
        warn(f"{name}: textured transmission/subsurface weight - using mean")
    tw, sw_ = scal(tw), scal(sw_)
    total = tw + (1 - tw) * sw_
    thin = bool(get("geometry_thin_walled"))
    if total > 0:
        body = {"transmissionFactor": total}
        if tw_tex: body["transmissionTexture"] = tw_tex
        use("KHR_materials_transmission", body)
        vol = {"thicknessFactor": 0.0 if thin else thickness_m}
        depth = scal(get("transmission_depth"))
        if sw_ > 0:
            rad = scal(get("subsurface_radius")) * float(np.mean(vec3(get("subsurface_radius_scale"))))
            sc_ = get("subsurface_color")
            use("KHR_materials_scatter", {
                "scatterStrengthFactor": (1 - tw) * sw_ / total,
                "multiscatterColorFactor": vec3(sc_ if const(sc_) else np.mean(sc_.reshape(-1, 3), 0)),
                "scatterAnisotropy": scal(get("subsurface_scatter_anisotropy"))})
            vol["attenuationColor"] = vec3(sc_ if const(sc_) else np.mean(sc_.reshape(-1, 3), 0))
            vol["attenuationDistance"] = max(rad * mpu, 1e-5)
        elif depth > 0:
            vol["attenuationColor"] = vec3(get("transmission_color"))
            vol["attenuationDistance"] = depth * mpu
        elif any(c < 0.999 for c in vec3(get("transmission_color"))):
            warn(f"{name}: transmission_color with depth 0 (surface tint) has no glTF slot - dropped")
        use("KHR_materials_volume", vol)

    # thin film: OpenPBR thickness is micrometres, glTF nanometres
    tfw = scal(get("thin_film_weight"))
    if tfw > 0:
        nm = scal(get("thin_film_thickness")) * 1000
        use("KHR_materials_iridescence", {"iridescenceFactor": tfw, "iridescenceIor": scal(get("thin_film_ior")),
                                          "iridescenceThicknessMinimum": nm, "iridescenceThicknessMaximum": nm})
    elif p.get("thin_film_thickness") is not None or p.get("thin_film_ior") is not None:
        warn(f"{name}: thin_film_* authored but thin_film_weight is 0 (the default) - film is OFF in OpenPBR too")

    # emission: luminance in nits; glTF is relative, so it is scaled by nits_per_unit
    L, C = get("emission_luminance"), get("emission_color")
    if is_img(L) or scal(L) > 0:
        if is_img(L) or is_img(C):
            LC = (as3(C) if is_img(C) else np.float32(vec3(C))) * (L[..., :1] if is_img(L) else scal(L))
            LC = np.maximum(LC, 0); peak = float(LC.max()) or 1.0
            m["emissiveTexture"] = glb.texture(lin_to_srgb(LC / peak), f"{name}_emissive", colour=True)
            m["emissiveFactor"] = [1, 1, 1]; strength = peak / nits_per_unit
        else:
            m["emissiveFactor"] = vec3(C); strength = scal(L) / nits_per_unit
        if strength > 1: use("KHR_materials_emissive_strength", {"emissiveStrength": strength})
        elif strength < 1: m["emissiveFactor"] = [c * strength for c in m["emissiveFactor"]]

    if not ext: del m["extensions"]
    glb.g["materials"].append(m)
    return len(glb.g["materials"]) - 1

# ---------------------------------------------------------------- USD geometry
def flat(pv, interp, fvi, counts, nfv):
    """Expand a primvar to one value per face-vertex."""
    v = np.asarray(pv)
    if interp in ("vertex", "varying"): return v[fvi]
    if interp == "faceVarying": return v
    if interp == "uniform": return np.repeat(v, counts, 0)
    return np.repeat(v.reshape(1, -1), nfv, 0)

def mesh_arrays(prim, xf_cache, mpu, faces=None):
    """World-space corner arrays for a mesh, or for only `faces` (a GeomSubset)."""
    from pxr import UsdGeom
    mesh = UsdGeom.Mesh(prim)
    pts = np.asarray(mesh.GetPointsAttr().Get(), np.float64)
    counts = np.asarray(mesh.GetFaceVertexCountsAttr().Get(), np.int64)
    fvi = np.asarray(mesh.GetFaceVertexIndicesAttr().Get(), np.int64)
    if len(pts) == 0 or len(counts) == 0: return None
    nfv = len(fvi)
    pvapi = UsdGeom.PrimvarsAPI(prim)
    st = None
    for nm in ("st", "st0", "UVMap", "uv"):
        pv = pvapi.GetPrimvar(nm)
        if pv and pv.HasValue():
            st = flat(pv.ComputeFlattened(), pv.GetInterpolation(), fvi, counts, nfv); break
    nrm = None
    pv = pvapi.GetPrimvar("normals")
    if pv and pv.HasValue():
        nrm = flat(pv.ComputeFlattened(), pv.GetInterpolation(), fvi, counts, nfv)
    elif mesh.GetNormalsAttr().HasValue():
        nrm = flat(mesh.GetNormalsAttr().Get(), mesh.GetNormalsInterpolation(), fvi, counts, nfv)
    M = np.array(xf_cache.GetLocalToWorldTransform(prim), np.float64)   # row-vector convention
    P = pts[fvi] @ M[:3, :3] + M[3, :3]
    flip = (np.linalg.det(M[:3, :3]) < 0) ^ (mesh.GetOrientationAttr().Get() == "leftHanded")
    # fan triangulation over face-vertex corners
    starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
    tris = [np.stack([s + 0 * np.arange(c - 2), s + np.arange(1, c - 1), s + np.arange(2, c)], 1)
            for s, c in zip(starts, counts) if c >= 3]
    face_of = np.concatenate([np.full(c - 2, f) for f, c in enumerate(counts) if c >= 3])
    tris = np.concatenate(tris)
    if faces is not None:
        tris = tris[np.isin(face_of, faces)]
        if len(tris) == 0: return None
    if flip: tris = tris[:, [0, 2, 1]]
    if nrm is None:   # smooth normals over shared points (subdiv cages are authored smooth)
        fn = np.cross(P[tris[:, 1]] - P[tris[:, 0]], P[tris[:, 2]] - P[tris[:, 0]])
        acc = np.zeros_like(pts)
        for k in range(3): np.add.at(acc, fvi[tris[:, k]], fn)
        nrm = acc[fvi]
    else:
        nrm = np.asarray(nrm, np.float64) @ np.linalg.inv(M[:3, :3]).T
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
    uv = np.zeros((nfv, 2)) if st is None else np.asarray(st, np.float64)
    # UDIM: a mesh on tile 1001 + 10*v + u has UVs in [u, u+1) x [v, v+1). Move the
    # WHOLE mesh by its tile (the median, so a vertex sitting exactly on a tile
    # edge is not split off) back to [0,1)^2 -- both axes: the Playground's
    # pacifier and plane plastic live on 1011/1012, one tile up in V.
    tile = np.floor(np.median(uv[np.unique(tris)], axis=0)).clip(0, None)
    uv = uv - tile
    udim = 1001 + int(tile[0]) + 10 * int(tile[1])
    uv = np.stack([uv[:, 0], 1 - uv[:, 1]], 1)
    return P * mpu, nrm, uv, tris, udim, bool(mesh.GetDoubleSidedAttr().Get())

COPYRIGHT = ("OpenPBR Shader Playground Copyright 2024 Adobe. All rights reserved. "
             "ASWF Digital Assets License v1.1. Modified: a subset of meshes converted from "
             "OpenUSD + MaterialX OpenPBR to glTF 2.0 by openpbr_mtlx_to_gltf.py (DisplayXR); materials are "
             "baked approximations of the originals.")

def usd_overrides(stage, mat_name):
    """Inputs authored on /World/Looks/<mat>/<node> by the scene's USD layers.

    The Playground binds each material as `references = @x.mtlx@` PLUS an
    `over` in materials/material_assignment.usda that re-sets OpenPBR inputs
    and graph-node inputs (a swapped texture file, remap ranges, a bottle whose
    coat is switched OFF). Those overrides ARE the authored look -- reading the
    .mtlx alone gets 23 of the scene's materials wrong. OpenUSD cannot load the
    .mtlx reference without its usdMtlx plugin (usd-core ships none), so the
    only things composed under a Looks material are exactly these overs."""
    from pxr import Sdf, Gf
    prim = stage.GetPrimAtPath(f"/World/Looks/{mat_name}")
    out = {}
    if not prim: return out
    for child in prim.GetAllChildren():   # overs only: GetChildren() skips undefined prims
        for attr in child.GetAuthoredAttributes():
            nm = attr.GetName()
            if not nm.startswith("inputs:") or not attr.HasAuthoredValue(): continue
            v = attr.Get()
            if isinstance(v, Sdf.AssetPath): v = v.path
            elif isinstance(v, bool): pass
            elif isinstance(v, (int, float)): v = np.array([float(v)], np.float32)
            elif hasattr(v, "__len__"): v = np.array(list(v), np.float32)
            else: continue
            out.setdefault(child.GetName(), {})[nm[len("inputs:"):]] = v
    return out

def decimate(P, UV, T, keep, name):
    """Quadric decimation to `keep` of the triangles (fast-simplification).

    The simplifier needs connected topology, so corners are welded by POSITION
    first; replaying its collapses yields an original->decimated vertex map that
    carries each surviving vertex's UV across. Two approximations follow, which
    is why this is opt-in per material and must be eyeballed: a vertex on a UV
    seam keeps ONE of its UVs (a texture can smear across the seam), and normals
    are recomputed smooth (hard edges soften). Right for dense smooth untextured
    or low-frequency meshes (the Playground's bubbles, juice, ice); wrong for a
    printed label."""
    import fast_simplification as fs
    uq, pid = np.unique(np.round(P, 6), axis=0, return_inverse=True)
    pid = pid.reshape(-1)
    F = pid[T]
    F = F[(F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])]
    _, _, coll = fs.simplify(uq, F, target_reduction=1.0 - keep, return_collapses=True)
    dp, dt, vmap = fs.replay_simplification(uq, F, coll)
    # Many tiny disconnected components (the Playground's bubbles are thousands
    # of spheres) can collapse to degenerate points the simplifier emits as NaN.
    # Drop every triangle touching one; say how many.
    bad = ~np.isfinite(dp).all(1)
    if bad.any():
        tbad = bad[dt].any(1)
        warn(f"{name}: decimation left {int(bad.sum())} non-finite vertices - dropped {int(tbad.sum())} tris")
        dt = dt[~tbad]
    # Keep only referenced vertices (the non-finite ones are now orphans).
    used, dt = np.unique(dt, return_inverse=True)
    dt = dt.reshape(-1, 3)
    remap = np.full(len(dp), -1); remap[used] = np.arange(len(used))
    dp = dp[used]
    vmap = np.where(vmap >= 0, remap[np.maximum(vmap, 0)], -1)
    duv = np.zeros((len(dp), 2))
    ok = vmap[pid] >= 0
    duv[vmap[pid][ok]] = UV[ok]
    fn = np.cross(dp[dt[:, 1]] - dp[dt[:, 0]], dp[dt[:, 2]] - dp[dt[:, 0]])
    dn = np.zeros_like(dp)
    for k in range(3): np.add.at(dn, dt[:, k], fn)
    dn /= np.maximum(np.linalg.norm(dn, axis=1, keepdims=True), 1e-12)
    print(f"  decimate {name}: {len(T)} -> {len(dt)} tris")
    return dp, dn, duv, dt

def group_meshes(stage, root, xf, mpu, exclude=()):
    from pxr import UsdGeom, UsdShade
    out = {}
    from pxr import Usd
    it = iter(Usd.PrimRange(stage.GetPrimAtPath(root)))
    for prim in it:
        if prim.GetName() in exclude:
            it.PruneChildren(); continue
        if not prim.IsA(UsdGeom.Mesh): continue
        if UsdGeom.Imageable(prim).ComputeVisibility() == "invisible": continue
        api = UsdShade.MaterialBindingAPI(prim)
        mat, _ = api.ComputeBoundMaterial()
        # GeomSubset bindings: each subset's faces take ITS material, and whatever
        # faces no subset claims take the mesh's own binding (if it has one). The
        # Playground's dresser is bound this way only, with nothing on the mesh.
        parts, claimed = [], []
        for sub in api.GetMaterialBindSubsets():
            sm, _ = UsdShade.MaterialBindingAPI(sub.GetPrim()).ComputeBoundMaterial()
            idx = np.asarray(sub.GetIndicesAttr().Get() or [], np.int64)
            claimed.append(idx)
            if sm: parts.append((sm, idx))
            else: warn(f"{sub.GetPrim().GetPath()}: subset with no material - skipped")
        if parts:
            nface = len(UsdGeom.Mesh(prim).GetFaceVertexCountsAttr().Get())
            rest = np.setdiff1d(np.arange(nface), np.concatenate(claimed))
            if len(rest) and mat: parts.append((mat, rest))
        elif mat:
            parts = [(mat, None)]
        else:
            warn(f"{prim.GetPath()}: no material bound - skipped"); continue
        for m, faces in parts:
            r = mesh_arrays(prim, xf, mpu, faces)
            if r: out.setdefault((m.GetPrim().GetName(), r[4], r[5]), []).append((str(prim.GetPath()), r))
    return out

def usd_lights(stage, root, xf, mpu):
    """UsdLux lights under `root` -> KHR_lights_punctual-ready dicts (metres).

    Same unit mapping as the viewer's native USD loader (ModelLight in
    model_common/model_loader.h): intensity J such that irradiance =
    J * shape / d^2. normalize=1 divides emission by area, so a small light is
    a point emitter of J = I*2^E*mpu^2 (rect/disk) or I*2^E*mpu^2/4 (sphere);
    normalize=0 multiplies by the projected area in m^2. glTF has no area
    light: a rect/disk becomes a 90-degree spot (falloff cos^2 instead of a
    one-sided emitter's cos -- the native loader keeps the true rect). Dome
    lights are skipped (no glTF equivalent)."""
    from pxr import Usd, UsdGeom, UsdLux, Gf
    out = []
    for prim in Usd.PrimRange(stage.GetPrimAtPath(root)):
        if not prim.HasAPI(UsdLux.LightAPI): continue
        if UsdGeom.Imageable(prim).ComputeVisibility() == "invisible": continue
        t = prim.GetTypeName()
        if t not in ("SphereLight", "RectLight", "DiskLight", "DistantLight"):
            if t != "DomeLight": warn(f"{prim.GetPath()}: {t} not exported")
            continue
        L = UsdLux.LightAPI(prim)
        get = lambda n, d: (prim.GetAttribute("inputs:" + n).Get() if prim.GetAttribute("inputs:" + n) and prim.GetAttribute("inputs:" + n).Get() is not None else d)
        E = float(get("intensity", 1.0)) * 2.0 ** float(get("exposure", 0.0))
        norm = bool(get("normalize", False))
        M = xf.GetLocalToWorldTransform(prim)
        sx = M.TransformDir(Gf.Vec3d(1, 0, 0)).GetLength(); sy = M.TransformDir(Gf.Vec3d(0, 1, 0)).GetLength()
        axis = M.TransformDir(Gf.Vec3d(0, 0, -1)).GetNormalized()
        pos = M.ExtractTranslation() * mpu
        if t == "DistantLight":
            typ, J = "directional", E
        elif t == "RectLight":
            w = float(get("width", 1.0)) * sx * mpu; h = float(get("height", 1.0)) * sy * mpu
            typ, J = "spot", (E * mpu * mpu if norm else E * w * h)
        elif t == "DiskLight":
            r = float(get("radius", 0.5)) * sx * mpu
            typ, J = "spot", (E * mpu * mpu if norm else E * np.pi * r * r)
        else:
            r = float(get("radius", 0.5)) * sx * mpu
            typ, J = "point", (E * mpu * mpu / 4.0 if norm else E * np.pi * r * r)
        out.append({"name": prim.GetName(), "type": typ, "intensity": float(J),
                    "color": [float(c) for c in get("color", (1, 1, 1))],
                    "pos": np.array([pos[0], pos[1], pos[2]]), "axis": np.array([axis[0], axis[1], axis[2]])})
    return out

def quat_from_neg_z(a):
    """Quaternion (x,y,z,w) rotating local -Z onto unit vector a."""
    v = np.array([0.0, 0.0, -1.0]); d = float(np.dot(v, a))
    if d < -0.999999: return [0.0, 1.0, 0.0, 0.0]     # antiparallel: 180 deg about Y
    c = np.cross(v, a); q = np.array([c[0], c[1], c[2], 1.0 + d])
    return (q / np.linalg.norm(q)).tolist()

def write_group(groups, out, mdir, mpu, a, lights=()):
    glb = GLB()
    glb.jpeg_quality = a.jpeg_quality
    glb.g["asset"]["copyright"] = a.copyright
    allP = np.concatenate([r[0] for g in groups.values() for _, r in g])
    lo, hi = allP.min(0), allP.max(0)
    centre = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])   # centred, resting on y=0
    print(f"{os.path.basename(out)}: bbox {np.round(hi - lo, 3)} m")
    multi = {k[0] for k in groups if sum(1 for j in groups if j[0] == k[0] and j[1] != k[1]) > 0}
    for (mat_name, udim, double_sided), items in sorted(groups.items()):
        # A material whose meshes span UDIM tiles becomes one glTF material per
        # tile; USD's doubleSided lives on the gprim, glTF's on the material, so
        # sidedness splits an instance too.
        mname = f"{mat_name}.{udim}" if mat_name in multi else mat_name
        if any(k[0] == mat_name and k[1] == udim and k[2] != double_sided for k in groups):
            mname += ".2s" if double_sided else ".1s"
        P = np.concatenate([r[0] for _, r in items]) - centre
        thickness = float(np.sort(P.max(0) - P.min(0))[0])   # thinnest bbox axis: crude volume thickness
        ov = usd_overrides(a.stage, mat_name)
        if ov: print(f"  {mname}: USD overrides on {sorted(ov)}")
        g = Graph(os.path.join(mdir, mat_name + ".mtlx"), a.max_tex, udim, ov)
        mi = build_material(glb, mname, g.surface(), mpu, thickness, a.nits_per_unit)
        if double_sided: glb.g["materials"][mi]["doubleSided"] = True
        N = np.concatenate([r[1] for _, r in items]); UV = np.concatenate([r[2] for _, r in items])
        off, T = 0, []
        for _, r in items: T.append(r[3] + off); off += len(r[0])
        T = np.concatenate(T)
        keep = a.decimate.get(mname, a.decimate.get(mat_name))
        if keep is not None and keep < 1.0:
            P, N, UV, T = decimate(P, UV, T, keep, mname)
        key = np.concatenate([np.round(P, 6), np.round(N, 4), np.round(UV, 6)], 1)   # weld corners
        uniq, inv = np.unique(key, axis=0, return_inverse=True)
        inv = inv.reshape(-1)
        prim = {"attributes": {
                    "POSITION": glb.accessor(uniq[:, 0:3].astype(np.float32), "VEC3", 34962),
                    "NORMAL":   glb.accessor(uniq[:, 3:6].astype(np.float32), "VEC3", 34962),
                    "TEXCOORD_0": glb.accessor(uniq[:, 6:8].astype(np.float32), "VEC2", 34962)},
                "indices": glb.accessor(inv[T].reshape(-1).astype(np.uint32), "SCALAR", 34963),
                "material": mi}
        glb.g["meshes"].append({"name": mname, "primitives": [prim]})
        glb.g["nodes"].append({"name": mname, "mesh": len(glb.g["meshes"]) - 1})
        glb.g["scenes"][0]["nodes"].append(len(glb.g["nodes"]) - 1)
        print(f"  {mname:14s} {len(items):3d} meshes {len(T):7d} tris  {sorted(glb.g['materials'][mi].get('extensions', {}))}")
    if lights:
        ext = []
        for L in lights:
            body = {"name": L["name"], "type": L["type"], "intensity": L["intensity"], "color": L["color"]}
            if L["type"] == "spot":
                body["spot"] = {"innerConeAngle": 0.0, "outerConeAngle": float(np.pi / 2)}
            ext.append(body)
            glb.g["nodes"].append({"name": L["name"],
                                   "translation": (L["pos"] - centre).tolist(),
                                   "rotation": quat_from_neg_z(L["axis"]),
                                   "extensions": {"KHR_lights_punctual": {"light": len(ext) - 1}}})
            glb.g["scenes"][0]["nodes"].append(len(glb.g["nodes"]) - 1)
        glb.g.setdefault("extensions", {})["KHR_lights_punctual"] = {"lights": ext}
        glb.used.add("KHR_lights_punctual")
        print(f"  {len(lights)} light(s) -> KHR_lights_punctual")
    glb.write(out)
    print(f"  -> {out} ({os.path.getsize(out) / 1e6:.1f} MB)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene"); ap.add_argument("outdir")
    ap.add_argument("--groups", required=True, help="comma-separated prim names under /World")
    ap.add_argument("--exclude", default="",
                    help="comma-separated prim names to skip with their subtrees (strays that "
                         "would blow up a group's bounds, e.g. spare cardboard far from the plane)")
    ap.add_argument("--max-tex", type=int, default=1024)
    ap.add_argument("--jpeg-quality", type=int, default=0,
                    help="encode sRGB colour textures (no alpha) as JPEG at this quality; "
                         "0 = lossless PNG everywhere (default)")
    ap.add_argument("--decimate", default="",
                    help="per-material triangle budget, e.g. 'bubbles=0.25,iceCube=0.4' keeps 25%%/40%%. "
                         "Opt-in: see decimate() for what it approximates")
    ap.add_argument("--copyright", default=COPYRIGHT,
                    help="asset.copyright string (default: the Shader Playground's required notice)")
    ap.add_argument("--nits-per-unit", type=float, default=1.0,
                    help="emission_luminance (nits) that maps to glTF emissive 1.0")
    a = ap.parse_args()
    a.decimate = {k: float(v) for k, v in (kv.split("=") for kv in a.decimate.split(",") if kv)}
    from pxr import Usd, UsdGeom
    stage = Usd.Stage.Open(a.scene)
    a.stage = stage
    mpu = UsdGeom.GetStageMetersPerUnit(stage)
    xf = UsdGeom.XformCache(Usd.TimeCode.EarliestTime())
    mdir = os.path.join(os.path.dirname(a.scene), "materials")
    os.makedirs(a.outdir, exist_ok=True)
    for grp in a.groups.split(","):
        # A group is a prim under /World, or an absolute path ("/World" = the scene).
        root = grp if grp.startswith("/") else "/World/" + grp
        groups = group_meshes(stage, root, xf, mpu, set(filter(None, a.exclude.split(","))))
        if not groups: warn(f"{grp}: no meshes"); continue
        stem = root.rstrip("/").split("/")[-1].replace("_grp", "")
        write_group(groups, os.path.join(a.outdir, stem + ".glb"), mdir, mpu, a,
                    usd_lights(stage, root, xf, mpu))
    print(f"{len(WARN)} warnings")

if __name__ == "__main__":
    main()

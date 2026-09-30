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
Geometry: every visible UsdGeomMesh under each /World/<group>, in world space,
metres, fan-triangulated, merged into one primitive per material, one .glb per
group (re-centred, resting on y=0).

Materials: the MaterialX graph feeding each open_pbr_surface input is EVALUATED
PER PIXEL in numpy (image / extract / invert / combine3 / normalmap / remap /
multiply), then baked into core glTF + the extensions above. Nothing is dropped
silently -- every unmapped input or unevaluated node is a WARN line.

Mapping decisions worth knowing (see docs/openpbr-to-gltf.md for the table):
  * base_weight is premultiplied into baseColorFactor.
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

Known limits: one UDIM tile per texture (the lowest present; mesh UVs are
wrapped into [0,1), exact for a mesh on one tile); GeomSubset per-face materials
collapse to the mesh binding (warned); colorcorrect / ramp / heighttonormal
nodes pass their input through (warned); textured specular / transmission /
subsurface weights are averaged (warned). Catmull-Clark meshes export their
cage, not the limit surface.

Requirements (a venv is easiest):  pip install usd-core numpy pillow tifffile imagecodecs
  (imagecodecs: the Playground's .tif textures are LZW-compressed.)

Usage:
    python scripts/openpbr_mtlx_to_gltf.py <scene.usda> <outdir> \
        --groups mug_grp,lamp_grp,meetMAT_grp [--max-tex 1024]

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
    def __init__(self, path, max_tex):
        txt = open(path).read().replace("<UDIM>", "@UDIM@")
        self.root = ET.fromstring(txt)
        self.dir = os.path.dirname(path)
        self.nodes = {n.get("name"): n for n in self.root}
        self.max_tex = max_tex
        self.cache = {}

    def inp(self, node, name, default=None):
        """Last-wins, like MaterialX (the mug declares base_color twice)."""
        hit = None
        for i in node.findall("input"):
            if i.get("name") == name: hit = i
        if hit is None: return default
        if hit.get("nodename"): return self.eval(hit.get("nodename"))
        return parse_val(hit.get("value"), hit.get("type"))

    def eval(self, name):
        if name in self.cache: return self.cache[name]
        n = self.nodes[name]; t = n.tag; typ = n.get("type")
        if t == "image":
            f = n.find("input[@name='file']")
            rel = f.get("value")
            if "@UDIM@" in rel:
                # glTF has no UDIMs. Take the lowest tile present; mesh UVs are wrapped
                # into [0,1) at load, which is exact for a mesh living on one tile.
                import glob
                tiles = sorted(glob.glob(os.path.join(self.dir, rel.replace("@UDIM@", "[0-9][0-9][0-9][0-9]"))))
                if len(tiles) > 1: warn(f"{name}: {len(tiles)} UDIM tiles - using {os.path.basename(tiles[0])} only")
                rel = os.path.relpath(tiles[0], self.dir) if tiles else rel.replace("@UDIM@", "1001")
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
        names = {i.get("name") for i in s.findall("input")}
        return {k: self.inp(s, k) for k in names}

# ---------------------------------------------------------------- glTF writer
class GLB:
    def __init__(self):
        self.g = {"asset": {"version": "2.0", "generator": "openpbr_mtlx_to_gltf.py (DisplayXR model viewer)"},
                  "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
                  "materials": [], "accessors": [], "bufferViews": [], "buffers": [],
                  "images": [], "textures": [], "samplers": [{"wrapS": 10497, "wrapT": 10497}]}
        self.bin = bytearray(); self.used = set()

    def view(self, data, target=None):
        while len(self.bin) % 4: self.bin += b"\0"
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target: bv["target"] = target
        self.bin += data; self.g["bufferViews"].append(bv)
        return len(self.g["bufferViews"]) - 1

    def accessor(self, arr, typ, target):
        comp = 5125 if arr.dtype == np.uint32 else 5126
        acc = {"bufferView": self.view(arr.tobytes(), target), "componentType": comp,
               "count": len(arr), "type": typ}
        if typ == "VEC3" and comp == 5126:
            acc["min"] = arr.min(0).tolist(); acc["max"] = arr.max(0).tolist()
        self.g["accessors"].append(acc)
        return len(self.g["accessors"]) - 1

    def texture(self, img01, name):
        """img01: HxWxC float in [0,1], already in the encoding glTF wants for its slot."""
        a = (np.clip(img01, 0, 1) * 255 + 0.5).astype(np.uint8)
        if a.shape[2] == 1: a = a[..., 0]
        buf = io.BytesIO(); Image.fromarray(a).save(buf, "PNG", optimize=True)
        self.g["images"].append({"name": name, "mimeType": "image/png", "bufferView": self.view(buf.getvalue())})
        self.g["textures"].append({"sampler": 0, "source": len(self.g["images"]) - 1})
        return {"index": len(self.g["textures"]) - 1}

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
    if is_img(bc) or is_img(op):
        bc3 = as3(bc) if is_img(bc) else np.broadcast_to(vec3(bc), (1, 1, 3)).astype(np.float32)
        tex = lin_to_srgb(bc3)
        if is_img(op):
            tex, op = match(tex if is_img(bc) else np.broadcast_to(tex, op.shape[:2] + (3,)).copy(), op)
            tex = np.concatenate([tex, op[..., :1]], -1)
        pbr["baseColorTexture"] = glb.texture(tex, f"{name}_baseColor")
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
    if is_img(tw) or is_img(sw_): warn(f"{name}: textured transmission/subsurface weight - using mean")
    tw, sw_ = scal(tw), scal(sw_)
    total = tw + (1 - tw) * sw_
    thin = bool(get("geometry_thin_walled"))
    if total > 0:
        use("KHR_materials_transmission", {"transmissionFactor": total})
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
            m["emissiveTexture"] = glb.texture(lin_to_srgb(LC / peak), f"{name}_emissive")
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

def mesh_arrays(prim, xf_cache, mpu):
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
    tris = np.concatenate(tris)
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
    uv = np.stack([uv[:, 0] - np.floor(uv[:, 0]).clip(0, None) * (uv[:, 0] >= 1), 1 - uv[:, 1]], 1)
    return P * mpu, nrm, uv, tris

COPYRIGHT = ("OpenPBR Shader Playground Copyright 2024 Adobe. All rights reserved. "
             "ASWF Digital Assets License v1.1. Modified: a subset of meshes converted from "
             "OpenUSD + MaterialX OpenPBR to glTF 2.0 by openpbr_mtlx_to_gltf.py (DisplayXR); materials are "
             "baked approximations of the originals.")

def group_meshes(stage, root, xf, mpu):
    from pxr import UsdGeom, UsdShade
    out = {}
    from pxr import Usd
    for prim in Usd.PrimRange(stage.GetPrimAtPath(root)):
        if not prim.IsA(UsdGeom.Mesh): continue
        if UsdGeom.Imageable(prim).ComputeVisibility() == "invisible": continue
        api = UsdShade.MaterialBindingAPI(prim)
        mat, _ = api.ComputeBoundMaterial()
        names = set()
        for sub in api.GetMaterialBindSubsets():
            sm, _ = UsdShade.MaterialBindingAPI(sub.GetPrim()).ComputeBoundMaterial()
            if sm: names.add(sm.GetPrim().GetName())
            if not mat: mat = sm
        if mat: names.add(mat.GetPrim().GetName())
        if len(names) > 1:
            warn(f"{prim.GetPath()}: per-face materials {sorted(names)} - whole mesh takes {mat.GetPrim().GetName()}")
        if not mat: warn(f"{prim.GetPath()}: no material bound - skipped"); continue
        r = mesh_arrays(prim, xf, mpu)
        if r: out.setdefault(mat.GetPrim().GetName(), []).append((str(prim.GetPath()), r))
    return out

def write_group(groups, out, mdir, mpu, a):
    glb = GLB()
    glb.g["asset"]["copyright"] = a.copyright
    allP = np.concatenate([r[0] for g in groups.values() for _, r in g])
    lo, hi = allP.min(0), allP.max(0)
    centre = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])   # centred, resting on y=0
    print(f"{os.path.basename(out)}: bbox {np.round(hi - lo, 3)} m")
    for mname, items in sorted(groups.items()):
        P = np.concatenate([r[0] for _, r in items]) - centre
        thickness = float(np.sort(P.max(0) - P.min(0))[0])   # thinnest bbox axis: crude volume thickness
        g = Graph(os.path.join(mdir, mname + ".mtlx"), a.max_tex)
        mi = build_material(glb, mname, g.surface(), mpu, thickness, a.nits_per_unit)
        N = np.concatenate([r[1] for _, r in items]); UV = np.concatenate([r[2] for _, r in items])
        off, T = 0, []
        for _, r in items: T.append(r[3] + off); off += len(r[0])
        T = np.concatenate(T)
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
    glb.write(out)
    print(f"  -> {out} ({os.path.getsize(out) / 1e6:.1f} MB)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene"); ap.add_argument("outdir")
    ap.add_argument("--groups", required=True, help="comma-separated prim names under /World")
    ap.add_argument("--max-tex", type=int, default=1024)
    ap.add_argument("--copyright", default=COPYRIGHT,
                    help="asset.copyright string (default: the Shader Playground's required notice)")
    ap.add_argument("--nits-per-unit", type=float, default=1.0,
                    help="emission_luminance (nits) that maps to glTF emissive 1.0")
    a = ap.parse_args()
    from pxr import Usd, UsdGeom
    stage = Usd.Stage.Open(a.scene)
    mpu = UsdGeom.GetStageMetersPerUnit(stage)
    xf = UsdGeom.XformCache(Usd.TimeCode.EarliestTime())
    mdir = os.path.join(os.path.dirname(a.scene), "materials")
    os.makedirs(a.outdir, exist_ok=True)
    for grp in a.groups.split(","):
        groups = group_meshes(stage, "/World/" + grp, xf, mpu)
        if not groups: warn(f"{grp}: no meshes"); continue
        write_group(groups, os.path.join(a.outdir, grp.replace("_grp", "") + ".glb"), mdir, mpu, a)
    print(f"{len(WARN)} warnings")

if __name__ == "__main__":
    main()

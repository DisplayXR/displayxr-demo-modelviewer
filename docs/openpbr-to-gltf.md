# OpenPBR → glTF: what survives the trip

Reference for [#70](https://github.com/DisplayXR/displayxr-demo-modelviewer/issues/70).
The recommended pipeline is **author in OpenPBR → export to glTF 2.0 → view in the
DisplayXR model viewer**, and the export step is lossy. This is the record of
*how* it is lossy, so a difference between the authoring tool and the viewer can
be attributed to the right cause.

Three distinct things get conflated when someone says "it doesn't match", and
they have different fixes:

1. **The export dropped it** — OpenPBR expresses something glTF has no slot for.
   No renderer can recover it. Listed under *Not representable* below.
2. **The export approximated it** — glTF has a nearby slot with different
   semantics. Listed under *Lossy* below.
3. **The viewer doesn't implement it** — the asset carries it and we ignore it.
   That set is in the [README support matrix](../README.md#material-feature-support),
   and the viewer says so out loud at load.

Only (3) is a bug in this repo. The first two are properties of the interchange
format, and the point of writing them down is that they stop being surprises.

---

## Direct mappings

These carry across with matching semantics.

| OpenPBR | glTF | Viewer |
|---|---|---|
| `base_metalness` | `pbrMetallicRoughness.metallicFactor` | ✅ |
| `specular_roughness` | `pbrMetallicRoughness.roughnessFactor` | ✅ |
| `specular_ior` | `KHR_materials_ior.ior` | ✅ |
| `specular_weight` | `KHR_materials_specular.specularFactor` | ✅ |
| `coat_weight` | `KHR_materials_clearcoat.clearcoatFactor` | ✅ |
| `coat_roughness` | `KHR_materials_clearcoat.clearcoatRoughnessFactor` | ✅ |
| `fuzz_roughness` | `KHR_materials_sheen.sheenRoughnessFactor` | ✅ |
| `transmission_weight` | `KHR_materials_transmission.transmissionFactor` | ✅ |
| `thin_film_ior` | `KHR_materials_iridescence.iridescenceIor` | ✅ |
| `thin_film_weight` | `KHR_materials_iridescence.iridescenceFactor` | ✅ |
| `geometry_normal` | `normalTexture` | ✅ |
| `geometry_tangent` | `TANGENT` attribute | ✅ |
| `geometry_opacity` | `baseColorFactor.a` + `alphaMode` | ✅ |

---

## Lossy mappings

Values arrive, but meaning shifts. These are where a comparison drifts without
anything looking obviously broken.

### `base_weight` × `base_color` → `baseColorFactor`

glTF has no separate base weight, so the exporter must premultiply. Harmless for
a static material; it stops round-tripping the moment either input is textured
or animated, because two independent controls have been collapsed into one.

### `thin_film_thickness` — micrometres → nanometres

OpenPBR specifies film thickness in **micrometres**; glTF's
`iridescenceThicknessMaximum` is in **nanometres**. A correct export multiplies
by 1000. Get it wrong and iridescence silently lands in a wavelength regime that
produces almost no visible interference — a plausible-looking result rather than
an obviously broken one, which is the worst failure mode. Worth checking first
whenever exported iridescence looks weaker than the authoring tool showed.

### `specular_color` on metals — edge tint is dropped

For dielectrics, `specular_color` tints the Fresnel factor and maps cleanly to
`KHR_materials_specular.specularColorFactor`. For **metals** it means something
different: the reflectivity at grazing incidence (~82°), i.e. the artistic edge
tint of a two-colour conductor model. glTF's metal is single-colour — `f0` comes
from base colour and there is no edge-tint slot — so this is discarded. Affects
metals with strongly coloured grazing response (gold, copper) most.

### `transmission_color` + `transmission_depth` → `attenuationColor` + `attenuationDistance`

Both are Beer-Lambert absorption and map directly in form. The catch is that
glTF's absorption only applies inside a `KHR_materials_volume` with a non-zero
`thicknessFactor`; an export that omits volume gets a thin transmissive surface
with **no absorption at all**, so tinted glass arrives colourless.

### `emission` + `emission_lum` → `emissiveFactor` + `KHR_materials_emissive_strength`

OpenPBR emission is photometric (real luminance units). glTF's is relative, with
no defined absolute scale. The shape survives; the absolute brightness is
whatever the exporter picks, so matching an authoring tool's emissive appearance
requires matching exposure too — which is exactly why this viewer pins exposure
and reports it.

### `specular_roughness_anisotropy` → `KHR_materials_anisotropy.anisotropyStrength`

Related quantities, not identical parameterisations, and the direction comes
from different places (`geometry_tangent` vs glTF's `TANGENT` plus
`anisotropyRotation`). Expect the *amount* of stretch to differ even when the
direction agrees. This viewer additionally applies anisotropy to direct light
only — see the README.

---

## Not representable

> **Ratified glTF only.** This table predates the draft extensions the viewer now
> implements. With `KHR_materials_coat` (#81), `coat_color`, `coat_ior`,
> `coat_darkening` and coat anisotropy survive; `KHR_materials_scatter` (#79)
> carries subsurface; `KHR_materials_fuzz` + `KHR_materials_diffuse_roughness`
> (#84) carry fuzz and `base_diffuse_roughness`. An exporter that targets the
> drafts — `scripts/openpbr_mtlx_to_gltf.py` does — loses much less than below.

glTF 2.0 has no slot for these. They are lost at export regardless of renderer.

| OpenPBR | Why it doesn't survive |
|---|---|
| `subsurface_weight`, `subsurface_color`, `subsurface_radius`, `subsurface_radius_scale`, `subsurface_scatter_anisotropy` | No ratified glTF subsurface extension. The single largest gap — skin, wax, marble and jade lose their defining behaviour and fall back to diffuse. |
| `coat_color` | glTF's clear coat is untinted. A coloured lacquer exports as a clear one. |
| `coat_ior` | glTF fixes the coat at IOR 1.5. |
| `coat_affect_color`, `coat_affect_roughness` | No equivalent for coat darkening or coat-induced roughening of the base. A coated material is noticeably lighter in glTF than in OpenPBR. |
| `coat_roughness_anisotropy` | glTF anisotropy applies to the base only. |
| `base_diffuse_roughness` | glTF's diffuse lobe is Lambertian; there is no Oren-Nayar roughness. Matte, dusty surfaces lose their retroreflective flattening. |
| `transmission_scatter`, `transmission_scatter_anisotropy` | glTF volume absorbs but does not scatter. Milky/cloudy interiors become clear. |
| `transmission_dispersion_scale`, `transmission_dispersion_abbe_number` | No wavelength-dependent IOR. No prismatic fringing. |
| `subsurface`/`fuzz`/`coat` layering weights as *layer* operations | glTF composes a fixed stack; OpenPBR's `layer()`/`mix()` graph is flattened at export, so energy moves between lobes slightly differently. |

`geometry_coat_normal` maps to glTF's `clearcoatNormalTexture`, so it survives
export — this viewer just doesn't read it yet. That is category (3), not (2).

---

## Practical guidance

- **Prefer the material grid over judgement by eye.** `assets/material_grid.glb`
  sweeps each property in isolation; comparing an authored material against a
  sweep localises a discrepancy far faster than comparing two hero renders.
- **Pin the viewing conditions before comparing anything.** Same HDRI, same
  exposure, same tone curve — see the README. Most reported "material
  mismatches" are lighting mismatches.
- **Check the load-time warning first.** If the viewer ignored an extension it
  says so, which rules category (3) in or out immediately.
- **Suspect units on iridescence.** The micrometre/nanometre factor of 1000 is
  the single most likely silent export error in this table.

---

## Converting a MaterialX OpenPBR scene

`scripts/openpbr_mtlx_to_gltf.py` is the export step made runnable for content
that ships as OpenUSD + `.mtlx` rather than glTF — the reference being the ASWF
[OpenPBR Shader Playground](https://dpel.aswf.io/openpbr-shader-playground).
It evaluates each material's node graph per pixel and bakes it into core glTF
plus the (draft) extensions above, one `.glb` per scene group; its docstring
lists every mapping decision and limit. Found by running it on the Playground:

- **`emission_luminance` is not absolute in practice.** The spec says nits, but
  in the Arnold reference render meetMAT peaks at ~2 and glows hard, so the
  script maps it 1:1 onto `emissiveStrength` (`--nits-per-unit 1`). Treating it
  as real nits (÷100 for SDR white) leaves every emitter in the scene dark.
- **Authored-but-disabled lobes are real.** The Playground's bubbles set
  `thin_film_thickness` and `thin_film_ior` but never `thin_film_weight`, whose
  default is 0 — so there is no iridescence in the *original* either. The script
  warns rather than "fixing" it; a mismatch there is not an export loss.
- **Screen-space transmission sees only opaque surfaces.** A transmissive object
  refracts the opaque scene behind it, never another transmissive one — so juice
  inside a glass largely vanishes. A property of this renderer, not the export.

---

## Loading OpenPBR USD directly

The viewer also opens an OpenPBR USD scene **without converting it**: drop
`ShdrPlygrnd_OpenPBR.usda` on it (macOS: `DXR_MODELVIEWER_MODEL=<path>`).
`model_common/model_loader_mtlx.cpp` is the C++ port of this script's material
path — same MaterialX evaluator, same mapping decisions — so everything in the
tables above applies unchanged; only the glTF step is gone. What that buys:

- no Python toolchain and no 1.9 GB → 150 MB export step; the whole Playground
  (61 MaterialX materials, 2.6 M vertices, ~110 maps) loads in ~13 s;
- USD's own semantics where glTF had none: per-gprim `doubleSided`, GeomSubset
  materials, per-tile UDIM instances, inherited visibility.

Measured against the converter's `.glb` of the same scene at the same texture
budget: mean |diff| **0.32/255** over the whole frame — the two paths agree.

Two things about the scene itself surfaced while building this. The
Playground's `materials/material_assignment.usda` re-sets inputs on 23 of its
54 materials, so reading the `.mtlx` files alone gets them wrong (both paths
now apply those overrides, with USD's rule that a connection authored in the
`.mtlx` beats a stronger layer's value). And it overrides a `place2d` node on
the walls that no `.mtlx` defines — a dangling override, correctly ignored.

---

## Scene lights and cameras

A file's own lights drive the viewer's `scene` lighting mode: natively from USD
(`UsdLux` sphere / rect / disk / distant) and from glTF `KHR_lights_punctual`,
which the converter now writes. glTF has no area light, so the converter turns a
rect or disk into a 90° spot. Its falloff is cos² against the native loader's
one-sided cos, which is the only difference between the two paths: on the whole
Playground, native vs converted, mean |diff| is **1.17/255**.

**Units.** `UsdLux` with `normalize = 1` divides emission by the light's area,
so a light small next to its distance acts as a point emitter of intensity
J = I·2^exposure·metersPerUnit² (rect/disk) or a quarter of that (sphere), with
irradiance J·shape/d². The renderer's light math is exact. A white Lambertian
plane under one J = 1 rect light at 1 m reads **0.271**, and the expected
1/π through PBR Neutral is 0.278.

**Shadows.** Each light renders a cube shadow map. Its six faces are layers of
one depth array, sampled as `sampler2DArrayShadow`, so no cube arrays and no
dynamic sampler indexing are needed. Transmissive materials cast nothing. That
keeps the LED inside meetMAT's head lighting the room and lets light through
glass. Arnold instead casts a coloured shadow through the bottle, which a
depth-only map cannot.

**Calibration.** One constant, **2.5**, maps J/d² onto the reference stills.
It's the median reference/ours ratio over directly lit pixels (lights only,
shadows on) at `renderCam_CU_planeTOP` against the published top-down still:
2.54 there, 2.43 over all pixels (IQR 1.4–3.7). Shadowed regions read low
because Arnold also carries bounce light and the dome, and this renderer has
neither. Before shadows the same fit gave 1.7 with a far wider spread, because
light leaking through the walls inflated the unshadowed render.

**Measure captures as linear.** The atlas PNG holds linear values (this
viewer's UNORM swapchain), not sRGB. Decoding them as sRGB, as I first did,
underestimates our radiance roughly 5× and points the blame at the light math.
The analytic check above is what caught it.

**Cameras.** `DXR_MODELVIEWER_CAMERA` (macOS) starts at a USD `GeomCamera`.
At `renderCam_CU_planeTOP` the framing matches the published top-down still
almost exactly. Not every still is a scene camera at its authored lens: the
"close up" stills are crops, and should be matched by feature alignment rather
than assumed.


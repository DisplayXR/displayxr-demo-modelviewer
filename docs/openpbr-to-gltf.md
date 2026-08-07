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

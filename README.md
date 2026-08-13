# DisplayXR Demo — 3D Model Viewer

Real-time PBR model viewer for glasses-free 3D displays, built on the
DisplayXR runtime via OpenXR with Vulkan. Loads **glTF 2.0** (`.glb` / `.gltf`),
**STL**, **OBJ**, **FBX**, and **USD** (`.usdz` / `.usd` / `.usda` / `.usdc`)
models and renders them with asymmetric per-eye Kooima projection for the full
multiview 3D experience.

Loads metallic-roughness PBR materials with textures and image-based lighting,
a blurred procedural skybox, and a transparent see-through mode (Windows). The
bundled sample is the Khronos DamagedHelmet, auto-loaded at startup.

> **Requires the DisplayXR runtime v1.9.1 or newer** (Windows) / **the latest
> macOS runtime `.pkg`**. Download the matching installer from the
> [`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases):
> `DisplayXRSetup-*.exe` on Windows or `DisplayXR-Installer-*.pkg` on macOS.
> v1.9.1 ships the Vulkan transparent-window bridge + in-place resize this demo
> relies on; older runtimes produce a broken/black window or flicker on resize.
> The shell ([`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases))
> is optional — only needed for the spatial workspace shell.

## Supported formats

| Format | Extensions | Materials | Notes |
|---|---|---|---|
| glTF 2.0 | `.glb` `.gltf` | Full metallic-roughness PBR + textures | Reference path |
| STL | `.stl` | Neutral default material | Binary + ASCII; geometry only |
| OBJ | `.obj` (+ `.mtl`) | Phong → metallic-roughness shim | Best-effort material fidelity |
| FBX | `.fbx` | PBR maps, Phong fallback | Skinned + animated (auto-plays first clip); no blend shapes yet |
| USD | `.usdz` `.usd` `.usda` `.usdc` | UsdPreviewSurface PBR | Base-color/emissive textures + PBR factors; normal & metallic-roughness *maps* not yet honoured |

Every format feeds the same renderer (metallic-roughness PBR + image-based
lighting). Not yet supported: **Draco** mesh compression and **KTX2 / Basis**
textures (textures are PNG/JPEG only). See [`PORTING.md`](PORTING.md) for the
per-backend breakdown and roadmap.

### Material feature support

The renderer implements core glTF metallic-roughness plus **every
`KHR_materials_*` extension listed below**. Anything not implemented still loads
and renders, but only its base layer does.

That fallback is what the spec prescribes, but it is **never silent**. Anything
the asset declares in `extensionsUsed` that the renderer lacks is listed on
stderr at load and summarised in the HUD (`! ignoring: clearcoat, sheen, +3
more`). A viewer whose job is "does this material match the authoring tool"
must not let a dropped extension be mistaken for a renderer or display
difference.

| Feature | Status |
|---|---|
| Base colour (factor + texture, sRGB-decoded) | ✅ |
| Metallic-roughness (factors + combined texture) | ✅ |
| Normal map (tangent-free, no `TANGENT` attribute needed) | ✅ |
| Occlusion, emissive | ✅ |
| Image-based lighting (irradiance + prefiltered specular + BRDF LUT) | ✅ |
| `KHR_materials_ior` | ✅ — drives dielectric f0 instead of the old hard-coded 0.04 |
| `KHR_materials_specular` | ✅ factors **+ textures** (`specularTexture` A, `specularColorTexture` RGB) |
| `KHR_materials_clearcoat` | ✅ factors **+ textures** (`clearcoatTexture` R, `clearcoatRoughnessTexture` G). No `clearcoatNormalTexture` |
| `KHR_materials_sheen` | ✅ factors **+ textures** (`sheenColorTexture` RGB, `sheenRoughnessTexture` A), Charlie + Ashikhmin with spec energy compensation |
| `KHR_materials_emissive_strength` | ✅ |
| `KHR_materials_anisotropy` | ⚠️ factors — spec D/V, **direct light only** (see below) |
| `KHR_materials_iridescence` | ✅ factors — full thin-film model, no thickness texture |
| `KHR_materials_transmission` | ✅ factor **+ `transmissionTexture`** (R) — refracts the rendered scene, roughness-blurred |
| `KHR_materials_volume` | ✅ factors **+ `thicknessTexture`** (G) — thickness-driven refraction + Beer-Lambert attenuation |
| `KHR_materials_coat` *(draft)* | ✅ factors **+ textures** (`coatTexture` R, `coatRoughnessTexture` G, `coatColorTexture` RGB, `coatAnisotropyTexture` B/RG, `coatNormalTexture`) — coloured tint, darkening, anisotropy, tunable IOR, and a coat-only shading normal. Every property in the spec's table is implemented |
| `KHR_materials_diffuse_roughness` *(draft)* | ⚠️ factor **+ `diffuseRoughnessTexture`** (R) — Fujii energy-preserving Oren-Nayar for direct light; IBL uses the spec's normal-bend approximation |
| `KHR_materials_fuzz` *(draft)* | ⚠️ factors **+ textures** (`fuzzTexture` R, `fuzzColorTexture` RGB, `fuzzRoughnessTexture` A) — replaces sheen, layered above the coat, weight separate from colour. Limited by the sheen LUT, see below |
| `KHR_materials_scatter` *(draft)* | ⚠️ factors **+ `scatterStrengthTexture`** (A) **+ `multiscatterColorTexture`** (RGB) — subsurface/multiple scattering. Volumetric mode is approximated with the spec-sanctioned thin-walled model; see the limits below |
| `KHR_texture_transform` | ✅ offset / rotation / scale, per texture slot (all 15). `texCoord` overrides ignored — single UV set |
| Draco, KTX2/Basis | ❌ |

**Scatter is an approximation, and here is exactly where it stops.** The spec
permits renderers without full volumetric transport to approximate volumetric mode
with the thin-walled model, which is what this does: a Lambertian lobe of the
multi-scatter albedo, split by anisotropy into a forward half (the most-blurred
scene copy, standing in for diffuse transmission) and a backward half (a diffuse
reflection). How much of the light takes that path is driven by optical depth
`thickness / attenuationDistance`, so density still matters. Measured against the
Khronos conformance assets (`ScatterColorAndDensity`, rasterizer reference), the
density response rises with density in both, and **ours saturates where the
reference keeps grading** — sampling the leftmost `multiscatterColorFactor`
column sparse→dense gives ours 141 → 182 → 181 → 181 against the reference's
98 → 123 → 141 → 135, i.e. ours is flat after the second step. Read those as a
direction, not a score: the absolute values are not comparable (the reference is
lit by an outdoor HDRI, we use the procedural analytic sky) and the percentages
move with where on the sphere you sample. The second known gap is that the result
is **over-saturated** — real multiple scattering desaturates as it redistributes
energy between bounces, and one Lambertian bounce cannot.

These were re-measured after the inverted-normal fix (#87); the figures quoted
here before that landed were taken with Fresnel pinned at grazing on every
material and should not be cited.

The obvious suspect for that second gap is the spec's Kulla-Conty multi→single
scatter albedo remap, which this deliberately does **not** apply to the lobe. It was
implemented and measured, not waved away: mean per-channel error against the
reference is **25.2 with the authored multi-scatter albedo vs 43.1 with the remapped
single-scatter one**, and at high anisotropy the remap sends the hue blue
(g=+1 → (153, 175, 194) against a reference of (75, 70, 57)). That is the expected
result once you notice the remap exists to derive *transport coefficients*: one
bounce carrying the multi-scatter albedo approximates the converged multi-bounce
appearance, whereas one bounce carrying the single-scatter albedo under-counts every
bounce after the first. Re-measure it any time with
`DXR_MODELVIEWER_KULLA_CONTY=1`.

**Sheen conserves energy.** The base layer is scaled by
`1 - max3(sheenColor) · E` before sheen is added, where `E` is the sheen
directional albedo — the hemispherical integral of the *same* Charlie/Ashikhmin
pair the shader evaluates, baked into a 64² table at startup
(`shaders/sheen_lut.frag`). Evaluator and integrand share `shaders/sheen.glsl`,
so the table cannot drift from the BRDF it is meant to integrate.

**Coat replaces clear coat, and reuses its lobe.** `KHR_materials_coat` is a
superset that the spec maps `KHR_materials_clearcoat` onto 1:1, so the loader
folds clearcoat's five properties into the coat fields and *one* shader lobe
serves both — a `hasCoat` flag gates only what coat adds. A clearcoat-only asset
therefore takes IOR 1.5 (f0 = 0.04, the constant the lobe used to hardcode),
white tint, no darkening and no anisotropy, and renders **byte-identically** to
the pre-coat build: mean 0.000 over 11,059,200 channels of
`assets/material_grid.glb`, and 0.000 on each of its ten rows individually. Coat
takes precedence where a material carries both, per spec.

Three notes where we knowingly diverge or where the draft is self-inconsistent,
all raised upstream:

- **`coatDarkeningFactor` defaults to 0 for a clearcoat-only asset**, not the
  spec's 1. Darkening is physically correct and coat turns it on, but clearcoat
  never had it, so applying it to a clearcoat asset would silently restyle it.
  With `KHR_materials_coat` present the spec's 1.0 applies.
- **Darkening is gated by coat weight.** The spec's composition applies
  `coatColor × coatDarkening` to the base *outside* the weighted mix, so a
  material with `coatFactor: 0` would still be tinted and darkened by a coat
  that is not there. We scale by the weight instead.
- **The spec's hemisphere average is 6.3x too high.** It computes the
  hemisphere-averaged reflectance for darkening as `F_0 + 0.5*F_90` = 0.54,
  describing it as "halfway between F_0 and F_90" — but that is not a hemisphere
  average, which for a Schlick Fresnel is `F_0 + (1-F_0)*0.0476` = 0.086. Taken
  literally it darkens ambient light 48% against direct light's 8%, putting a
  hard dark band on the unlit side of every coated surface. We use the true
  average; `DXR_MODELVIEWER_COAT_SPEC_HEMI=1` restores the literal formula.

**Diffuse roughness is a separate roughness.** The diffuse substrate gets a
microfacet model instead of pure Lambert, so a rough diffuse surface
back-scatters at grazing angles and flattens out — sandstone rather than matte
plastic. Direct light uses **Fujii's energy-preserving qualitative Oren-Nayar**,
not the EON model (arXiv 2410.18026) the spec points at; the spec explicitly
permits the substitution ("Implementations of the BRDF itself can vary based on
device performance… there is no single micro-facet model we can use as a ground
truth"). IBL uses the spec's third option, bending the shading normal toward the
view — which the spec itself calls the least correct and most performant of the
three, the other two meaning a rebuilt IBL pipeline for one draft extension.
Roughness 0 returns the Lambertian path unchanged.

**Spec defect:** `diffuseRoughnessFactor` defaults to `0.0` in the README
property table and `1.0` in the JSON schema. We take 0.0 — 1.0 would restyle
every existing asset.

**Fuzz replaces sheen, and reuses its lobe and its table.** `fuzzColorTexture`
and `fuzzRoughnessTexture` sample the *identical* channels as their sheen
counterparts, so fuzz rides the sheen lanes and slots; only the weight and a
`hasFuzz` flag are new. The two real differences are both implemented: fuzz sits
**above the coat** where sheen sits below it, and its weight is separate from its
colour, so fuzz can be *darker* than what it covers. Black soot is the motivating
case, and it is inexpressible in sheen — a black sheen colour just switches the
layer off. Measured on `assets/diffuse_fuzz_test.glb`, the same white→black
colour sweep written both ways: sheen −31.0 luma converging on the bare base,
fuzz −56.9 continuing past it into soot.

A note that used to sit here claimed the sheen/fuzz directional albedo saturated
below roughness ~0.5 and that a dark fuzz therefore nulled the surface. **That was
issue #87, not a lobe problem** — `ndotv` was pinned at its clamp, so every LUT
sample landed on the grazing edge where a directional albedo legitimately
approaches 1. Head-on with #87 fixed, E runs 0.000 at roughness 0.05 to 0.148 at
1.0. Issue #85 was closed as invalid.

**Spec defect:** `fuzzRoughnessFactor` defaults to `0.5` in the README table and
`0.0` in the schema. We take the schema.

**Texture-driven variants are read** for coat, clear coat, sheen/fuzz, diffuse
roughness, specular,
transmission and volume — each samples the channel its extension specifies and
multiplies the corresponding factor. Absent maps bind to 1×1 white, the
multiplicative identity, so a factors-only material behaves exactly as before.
Still **not** read: `clearcoatNormalTexture` (coat's equivalent IS read), `anisotropyTexture`,
`iridescenceTexture` and `iridescenceThicknessTexture`. Those are partial
implementations rather than missing ones, so they do **not** trip the
ignored-extension warning — this table is the reference.

Set 1 now binds 19 samplers and set 2 binds 5. Vulkan only *guarantees* 16 per
stage, so the renderer logs its budget against the device's actual limit at
startup (`sampled images per stage: need 24, device allows …`) rather than
letting a constrained device fail with an opaque pipeline-layout error.

That slot count and the material SSBO's two trailing UV-transform arrays are
sized from a single `#define` in `model_common/shaders/material_slots.glsl`,
included by both `pbr.frag` and `model_renderer.h`, with `static_assert`s tying
the enums and `sizeof(MaterialExtGpu)` to it. The count sets the struct's
*stride*, and when the two sides disagree only material 0 reads correctly —
which looks like whole-image corruption, not like a texture-slot problem. That
is issue #81; it cost a reverted branch to diagnose, and is now a build error.

**Transmission costs a scene-colour copy per view.** Refracting the *rendered
scene* (rather than only the environment, which the spec calls out as falling
short) means the opaque pass has to be captured before transmissive surfaces are
drawn. That copy happens once per `renderEye` — i.e. **once per view tile in the
atlas** — so its cost scales with view count, which is the one place where
driving a multiview 3D display genuinely changes the renderer's budget. It is
skipped entirely when no loaded material transmits, and in transparent-background
mode (where there is no opaque scene to refract, so glass falls back to IBL).

**What is captured is scene-linear radiance, not the displayed image.** The
opaque pass writes two colour attachments: the one that reaches the swapchain
(tone-mapped, and sRGB-encoded when the swapchain is UNORM) and a 16F twin
holding the same shading *before* the tone curve and the encode. Transmission
mips a copy of the twin. Capturing the displayed image instead — which is what
the renderer did up to v0.19.1 — put an already-graded value into a still-linear
`color` that then ran the whole grade again, so glass rendered washed out and
desaturated (issue #75). Sampling in radiance also means the `baseColorFactor`
tint, Beer-Lambert absorption and the diffuse-lobe replacement act on radiance,
which is the only space in which any of them means anything, and makes the
roughness mip chain a linear box filter rather than an average of encoded values.

To check it, run with `DXR_MODELVIEWER_TRANSMISSION_PROBE=1`: every transmissive
surface then outputs its raw scene sample through the shader's own display
transform instead of shading, so on `assets/transmission_test.glb` all six
transmissive spheres must reproduce the backdrop behind them and vanish, leaving
only the opaque control. `scripts/check_transmission_probe.py` measures that.

**Documented limitation — anisotropy is direct-light only.** The extension's
distribution and visibility terms are implemented verbatim, but anisotropy is
not applied to image-based lighting. The standard trick — bending the IBL
reflection vector toward the stretch direction — is implemented and then
deliberately disabled: it produces a hard vertical pinch on a sphere, and the
distortion is *non-monotonic* (strength 0.17 looks far worse than 1.0). The
cause is not the tangent frame; authoring `TANGENT` (which this viewer now
reads) disproved that hypothesis. Bending the reflection swings it across the
procedural sky's hard sky/ground horizon, and a two-tone environment turns a
smooth stretch into a visible seam. Anisotropic IBL is not spec text, so given
the choice between a visible artifact and an under-stated effect, a
material-fidelity viewer takes the under-stated one. Worth revisiting under a
real HDRI, which has no hard horizon for the bend to cross. Consequence: on a
metal lit mostly by an environment, anisotropy is measurable but close to
invisible.

**`TANGENT` is read when present.** The glTF `TANGENT` attribute now feeds the
shading frame, with the screen-space-derivative frame as the fallback for assets
that ship none. This is the correct source for normal mapping as well — the
derivative frame flips across UV seams and degenerates at poles — and the
material grid authors analytic tangents for its spheres.

**Both anisotropy and iridescence are subtle in the material grid**, and that is
physical rather than a defect. Anisotropy for the reason above; iridescence
because the grid's row is a 4 % dielectric under a broad sky, where thin-film
interference shifts the reflection only slightly. Pixel probes across each row
confirm both vary monotonically with their sweep.

Tracking issue: [#70 — OpenPBR reference scene and material interoperability](https://github.com/DisplayXR/displayxr-demo-modelviewer/issues/70).

**Authoring in OpenPBR?** [`docs/openpbr-to-gltf.md`](docs/openpbr-to-gltf.md)
records what survives the export, what arrives approximated, and what glTF has
no slot for at all — so a difference between the authoring tool and the viewer
can be attributed to the export, the format, or the renderer rather than guessed
at. (Subsurface is the big one: nothing carries.)

### The material grid

`material_grid.glb` is the reference scene the matrix above is measured against:
9 material families × a 7-step parameter sweep, plus a textured row — 70
spheres.

**It ships with the viewer.** The installers put it next to the executable on
Windows, macOS and Linux, so it is one *File ▸ Open* (`Ctrl+O`, or drag-and-drop
on Windows and macOS) away — no build required. It is deliberately **not** the startup
scene; the bundled helmet stays the default. If you want to see everything this
renderer does in one screen, open the grid:

| Platform | Where it lands |
|---|---|
| Windows | next to `model_viewer_handle_vk_win.exe` in the install dir |
| macOS | inside the `.app` bundle, `Contents/Resources/` |
| Linux | next to the binary (`.deb` / tarball) |

In a source tree it is `assets/material_grid.glb`, and the build copies it next
to the executable alongside `sample.glb`.

| Row | Family | Sweep |
|---|---|---|
| 0 | dielectric | roughness 0.03 → 1.0 |
| 1 | metal | roughness 0.03 → 1.0 |
| 2 | clearcoat | `clearcoatFactor` 0 → 1 over a rough red base |
| 3 | sheen | `sheenRoughnessFactor` 0.05 → 1.0 |
| 4 | anisotropy | `anisotropyStrength` 0 → 1 on brushed metal |
| 5 | iridescence | film thickness 200 → 800 nm |
| 6 | specular / IOR | `specularFactor` 0 → 1, `ior` 1.0 → 2.0 |
| 7 | transmission | `transmissionFactor` 0 → 1, `ior` 1.5, volume |
| 8 | emissive | `emissiveStrength` 0 → 6 |
| 9 | textured | one texture-driven property per column, all reading one ramp |

### coat_test.glb

`assets/coat_test.glb` is a second sweep, six rows by seven, for
`KHR_materials_coat` — Khronos publishes no conformance asset for it, so this
stands in. Every row shares one light neutral base; row 0 is plain
`KHR_materials_clearcoat` and row 1 is the same material in coat's spelling.

| Row | Sweep |
|---|---|
| 0 | CONTROL — `clearcoatFactor` 0 → 1 |
| 1 | `coatFactor` 0 → 1 (the same material, coat's spelling) |
| 2 | `coatColorFactor` white → amber, coat 0.5 |
| 3 | `coatDarkeningFactor` 0 → 1, coat 0.5 |
| 4 | `coatIor` 1.0 → 2.0 (f0 0 → 0.111) |
| 5 | `coatAnisotropyStrength` 0 → 1 |
| 6 | `coatNormalTexture` ripple, `coatFactor` 0 → 1 |

Measure with `python3 scripts/probe_coat_test.py <atlas.png>`. Rows 2 and 3 run
at coat 0.5 on purpose: at full coat the base is almost entirely displaced by
the coat's own mirror reflection of the sky, and tint and darkening both act on
the *base*, so there is nothing left for them to act on. The first cut of this
asset used a red base at full coat and measured a flat row for both.

### diffuse_fuzz_test.glb

`assets/diffuse_fuzz_test.glb`, six rows by seven, for `KHR_materials_fuzz` and
`KHR_materials_diffuse_roughness` — again, Khronos publishes no conformance asset
for either.

| Row | Sweep |
|---|---|
| 0 | CONTROL — `KHR_materials_sheen`, `sheenColorFactor` white → black |
| 1 | `fuzzFactor` 0 → 1, white fuzz |
| 2 | `fuzzColorFactor` white → **black** at weight 1 |
| 3 | `fuzzRoughnessFactor` 0.05 → 1 |
| 4 | `diffuseRoughnessFactor` 0 → 1, matte base |
| 5 | `diffuseRoughnessFactor` 0 → 1, semi-gloss base |

Measure with `python3 scripts/probe_fuzz_test.py <atlas.png>`. Rows 0 and 2 are
the same sweep under both extensions and must move in *opposite* directions —
that divergence is the extension's reason to exist. Row 5 exists to catch
coupling: diffuse roughness must not touch the specular lobe, and rows 4 and 5
responding differently is what shows it does not.

`material_grid.glb` is deliberately **not** extended when an extension lands. It
is the baseline every "does this change the render" measurement is taken
against, and an asset that moves each time cannot serve that purpose.

**The grid is a progress meter as much as a test asset**: a row is flat while
its extension is ignored and comes alive when the extension lands. All nine rows
now sweep, and the grid raises no ignored-extension warning at all. Anisotropy
and iridescence sweep only faintly for the physical reasons documented above —
measurable by pixel probe, easy to miss by eye. The transmission row is the
clearest demonstration: the emissive spheres from the row below appear refracted
inside each glass sphere, more strongly as `transmissionFactor` rises.

It is generated, not hand-authored, and the generator is the source of truth:

```bash
python3 scripts/make_material_grid.py          # → assets/material_grid.glb
```

A grid is a measuring instrument, so every value in it has to be inspectable and
re-derivable — when a shader change moves a pixel, the question is always "what
exactly is that sphere's roughness?", and a checked-in binary can't answer it.
The script can, and it regenerates byte-identical output. Materials are named
(`04_anisotropy_0.50`), so the glTF is self-documenting too.

The extensions are declared in `extensionsUsed`, never `extensionsRequired`, so
a loader that implements none of them still opens the file — which is the point.

Row 9 embeds a single 8×64 PNG whose **four channels all carry the same 0→1
ramp**. The extensions read different channels (clear coat R, clearcoat
roughness G, sheen roughness A, transmission R, thickness G…), so one image
drives every one of them and each sphere shows a pole-to-pole sweep of its own
property. If texture support regresses the row goes flat — the same tell the
rest of the grid uses.

## Environment and grading

Comparing a material against how it looked in the authoring tool only means
something when the **lighting and the grading are pinned and written down** —
otherwise every difference is attributable to the viewer's lighting rather than
to the material. The viewer therefore makes all three explicit and reports them
in the HUD, so a screenshot records the conditions it was taken under.

**Environment.** By default the IBL is baked from a procedural analytic sky, so
the viewer runs with no environment asset at all. Drop an equirectangular
`.hdr` on the window (Windows/macOS; or `Ctrl+O` it anywhere, or ship one as
`environment.hdr` next to
the executable to have it load at startup) and the irradiance + prefiltered
cubes are rebaked from it. Loading an HDRI also **switches the analytic key
light off** — a real capture already contains its own sun, and keeping both
would double-count the dominant light source.

**Exposure.** `[` / `]` in quarter stops; the shader multiplies linear radiance
by 2^EV.

**Tone curve.** `G` cycles:

| Curve | Use |
|---|---|
| **PBR Neutral** (default) | Khronos' glTF tone mapper. Preserves authored hue and saturation up to the compression knee — the curve to use for authoring-tool comparisons. |
| **ACES** | Stephen Hill's RRT/ODT fit. Filmic; matches DCC viewports that default to ACES. |
| **none (clamp)** | No curve, just clamp. Reproduces pre-#70 captures. |

The same exposure and curve are applied to the model and the background, so the
model never reads as pasted onto the environment. The background is
deliberately sampled from a high roughness mip: a sharp, high-contrast sky sits
far from the display's zero-disparity plane, where it causes lightfield
cross-talk. A soft background stays comfortable.

**Deterministic capture.** Set `DXR_MODELVIEWER_DETERMINISTIC=1` to pin the idle
auto-orbit off at startup. Reference renders are only comparable if nothing
moves between them, and the viewer starts slowly rotating the scene ~10 s after
the last input — long enough that a scripted *launch, wait, capture* sequence
lands at an unpredictable angle. Measured on the material grid, two captures 12
seconds apart:

| | channels differing |
|---|---|
| `DXR_MODELVIEWER_DETERMINISTIC=1` | **0 of 1,382,400 (0.00 %)** |
| default | 1,347,650 (97.49 %) |

An environment variable rather than a flag because the macOS build is an `.app`
bundle with no argv, and a capture harness needs to set this the same way
everywhere. Windows and macOS only — Linux has no auto-orbit.

**Reference renders.** `scripts/capture_reference.sh <asset.glb> [outdir]`
captures the viewer's output and writes a sidecar recording the conditions it
was taken under — asset, platform, viewer commit, active environment, and any
extensions the asset declared that were ignored. The sidecar is scraped from the
runtime's own log rather than restated, so it cannot drift from what the viewer
actually did. A reference nobody can reproduce is a screenshot, not a reference;
two independent runs of the script produce byte-identical PNGs.

None of this makes output identical across physical displays — panel
calibration, brightness, gamut and 3D cross-talk are separate concerns.

## Controls

| Input | Action |
|---|---|
| WASD / Q / E | Strafe the virtual display in 3D |
| Left-click drag | Rotate the virtual display |
| Scroll / trackpad | Zoom (virtual display height) |
| `-` / `=` | Decrease / increase depth + IPD together (10 %–100 %) |
| `M` | Auto-orbit: slow turntable rotation when idle |
| `V` | Cycle rendering modes advertised by the display runtime |
| `Ctrl+O` or top-bar **Open…** | Load a different model (glTF / STL / OBJ / FBX / USD) — or an `.hdr` environment |
| Drag-and-drop (Windows, macOS) | Load a supported model, or an `.hdr` environment, dropped onto the window |
| `[` / `]` | Exposure down / up, in quarter stops |
| `G` | Cycle the tone curve (PBR Neutral → ACES → none) |
| Space | Reset pose, zoom, depth |
| Tab | Toggle HUD |
| Ctrl+T | Toggle transparent background (desktop see-through; Windows only) |
| Esc | Quit |

## Build from source

### Prerequisites (both platforms)
- CMake ≥ 3.21 + Ninja
- [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) (includes `glslangValidator`)
- [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) (find_package-visible)
- A DisplayXR-compatible runtime (install via `DisplayXRSetup-*.exe` from
  [displayxr-runtime releases](https://github.com/DisplayXR/displayxr-runtime/releases))

`model_common/` fetches **tinygltf**, **glm**, and **tinyusdz** (USD) via CMake
`FetchContent` on first configure (no submodules); **tinyobjloader** (OBJ) and
**ufbx** (FBX) are vendored under `model_common/third_party/`. STL has no
dependency. The first configure builds tinyusdz from source, so it is slower.

### macOS
```bash
brew install cmake ninja vulkan-sdk openxr-loader
./scripts/build_macos.sh
# Run against an installed DisplayXR runtime (handles the Vulkan-loader setup):
./scripts/run_macos_dev.sh
```
> Launch the dev build with `scripts/run_macos_dev.sh`, not the bare binary.
> The dev binary links Homebrew's Vulkan loader while the installed runtime
> loads its own; the script converges both on one loader (and points Vulkan at
> the runtime's bundled MoltenVK) so the `xrGetVulkanGraphicsDeviceKHR`
> handshake succeeds. The distributed `.app` (`build_macos.sh --installer`)
> bundles a self-consistent Vulkan stack and needs none of this.

### Windows
```bat
REM Sets vcvars64 + OpenXR_ROOT + Vulkan SDK, then configures + builds.
scripts\build-with-deps.bat
REM Run
build\windows\model_viewer_handle_vk_win.exe
```
> Use `build-with-deps.bat`, not the bare `build_windows.bat` — the latter
> assumes you are already inside a VS developer environment.

## Repo layout

```
.
├── macos/                  Platform entry + window handling (Cocoa / MoltenVK)
├── windows/                Platform entry + window handling (Win32 / Vulkan)
├── model_common/           Multi-format PBR renderer: loaders + renderer + shaders
├── common/                 Shared helpers: Kooima math, input, HUD
├── openxr_includes/         Vendored OpenXR headers (incl. DisplayXR extensions)
├── installer/              Windows NSIS + macOS .pkg installers
├── scripts/                Build scripts for each platform
└── PORTING.md              Roadmap (port done; animation/skinning next)
```

`common/` and `openxr_includes/` are shared with the other DisplayXR demos and
were seeded from the runtime source tree.

## Why a glTF viewer (not a Gaussian-splat fork)

This is a separate demo, not a mode bolted onto the splat viewer: it shows a
different DisplayXR capability (mesh + PBR rendering) and grows the demo
gallery. The renderer draws on techniques from the MIT-licensed
[SaschaWillems/Vulkan-glTF-PBR](https://github.com/SaschaWillems/Vulkan-glTF-PBR).

## License

Apache-2.0 — see `LICENSE`. Bundled demo models carry their own licenses.
(Vendored OpenXR extension headers under `openxr_includes/` remain BSL-1.0 —
see their SPDX headers.)

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
| `KHR_texture_transform`, Draco, KTX2/Basis | ❌ |

**Sheen conserves energy.** The base layer is scaled by
`1 - max3(sheenColor) · E` before sheen is added, where `E` is the sheen
directional albedo — the hemispherical integral of the *same* Charlie/Ashikhmin
pair the shader evaluates, baked into a 64² table at startup
(`shaders/sheen_lut.frag`). Evaluator and integrand share `shaders/sheen.glsl`,
so the table cannot drift from the BRDF it is meant to integrate.

**Texture-driven variants are read** for clear coat, sheen, specular,
transmission and volume — each samples the channel its extension specifies and
multiplies the corresponding factor. Absent maps bind to 1×1 white, the
multiplicative identity, so a factors-only material behaves exactly as before.
Still **not** read: `clearcoatNormalTexture`, `anisotropyTexture`,
`iridescenceTexture` and `iridescenceThicknessTexture`. Those are partial
implementations rather than missing ones, so they do **not** trip the
ignored-extension warning — this table is the reference.

Set 1 now binds 13 samplers and set 2 binds 5. Vulkan only *guarantees* 16 per
stage, so the renderer logs its budget against the device's actual limit at
startup (`sampled images per stage: need 18, device allows …`) rather than
letting a constrained device fail with an opaque pipeline-layout error.

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
on macOS) away — no build required. It is deliberately **not** the startup
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
`.hdr` on the window (or `Ctrl+O` it, or ship one as `environment.hdr` next to
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
| `L` or top-bar **Open…** | Load a different model (glTF / STL / OBJ / FBX / USD) — or an `.hdr` environment |
| Drag-and-drop (macOS) | Load a supported model, or an `.hdr` environment, dropped onto the window |
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

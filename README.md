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

The renderer implements **core glTF metallic-roughness only**. No
`KHR_materials_*` extension is honoured yet — a glTF that uses one loads and
renders, but the extension's contribution is silently absent (a clear-coat
material renders as its base layer, a transmissive material renders opaque).

| Feature | Status |
|---|---|
| Base colour (factor + texture, sRGB-decoded) | ✅ |
| Metallic-roughness (factors + combined texture) | ✅ |
| Normal map (tangent-free, no `TANGENT` attribute needed) | ✅ |
| Occlusion, emissive | ✅ |
| Image-based lighting (irradiance + prefiltered specular + BRDF LUT) | ✅ |
| `KHR_materials_ior` / `_specular` / `_clearcoat` / `_sheen` / `_anisotropy` / `_iridescence` / `_emissive_strength` | ❌ planned |
| `KHR_materials_transmission` / `_volume` | ❌ planned (needs a scene-colour copy per view) |
| `KHR_texture_transform`, Draco, KTX2/Basis | ❌ |

Tracking issue: [#70 — OpenPBR reference scene and material interoperability](https://github.com/DisplayXR/displayxr-demo-modelviewer/issues/70).

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

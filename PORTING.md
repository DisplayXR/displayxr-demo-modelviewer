# Porting the skeleton into a working glTF model viewer

This repo was scaffolded from `displayxr-demo-gaussiansplat`. The DisplayXR
plumbing is done; the renderer is a stub. This file is the checklist to turn
it into a real viewer.

## Source of the renderer

Base the renderer on **[SaschaWillems/Vulkan-glTF-PBR](https://github.com/SaschaWillems/Vulkan-glTF-PBR)**
(MIT — compatible with this repo's BSL-1.0 distribution). The pieces to lift:

| From Vulkan-glTF-PBR | Into this repo |
|---|---|
| `base/VulkanglTFModel.{h,cpp}` (`vkglTF::Model`, loader + Vulkan upload) | `model_common/model_loader.*` + `model_common/model_renderer.cpp` (or drop in wholesale) |
| `data/shaders/pbr.{vert,frag}` | `model_common/shaders/pbr.{vert,frag}` (replace the skeleton shaders) |
| `data/shaders/genbrdflut.*`, `irradiancecube.*`, `prefilterenvmap.*`, `filtercube.vert` | `model_common/shaders/` (add for IBL) |
| BRDF-LUT / irradiance / prefiltered-env generation in `main.cpp` (`generateBRDFLUT`, `generateCubemaps`) | private setup in `ModelRenderer::init` |

`vkglTF::Model` already does parsing **and** Vulkan upload, so it can largely
replace both `model_loader.*` and the upload half of `model_renderer.cpp`. The
thin `model_loader.*` wrapper (tinygltf) is provided only as a lighter
starting point if you don't want the full class.

Loader choice: the skeleton wires **tinygltf** (what Vulkan-glTF-PBR uses).
`fastgltf` is a faster alternative but means writing the upload yourself.

## `model_common/` — fill in the stubs

- [ ] `model_renderer.cpp` — every method is a stub. Implement `init`
      (pipeline, descriptors, depth + offscreen targets, IBL), `loadModel`,
      `renderEye` (the core: PBR pass into the viewport region), `cleanup`.
- [ ] `model_loader.cpp` — extract vertices/indices/materials/textures + the
      real AABB (or delete in favour of `vkglTF::Model`).
- [ ] `model_common/shaders/` — replace the two skeleton shaders with the full
      PBR + IBL set; update `SHADER_SOURCES` in `model_common/CMakeLists.txt`.
- [x] `model_vulkan_utils.*` — generic VkBuffer/VkImage helpers, reused as-is.

## Platform glue — retarget the renderer call sites

`windows/main.cpp`, `windows/xr_session.cpp`, and `macos/main.mm` are the
**porting baseline** (banner at the top of each). Their window / OpenXR /
transparency / HUD / input code is reusable as-is. Only the renderer calls
change. Find every reference to the GS API and retarget:

| GS baseline call | Model viewer replacement |
|---|---|
| `#include "gs_renderer.h"`, `GsRenderer` | `#include "model_renderer.h"`, `ModelRenderer` |
| `loadScene(path)` / `loadDebugScene(...)` | `loadModel(path)` / `loadDebugModel()` |
| `hasScene()` / `scenePath()` / `gaussianCount()` | `hasModel()` / `modelPath()` / `primitiveCount()` |
| `pickGaussian(...)` | `pickSurface(...)` (ray/triangle vs mesh) |
| `getMainObjectBounds(...)` (splat flood-fill) | drop; use `getSceneBBox` / `getRobustSceneBounds` |
| `findBestYaw(...)` | kept (returns 0 for front-facing models) |
| `renderEye(...)` | same signature — already matched |
| `.ply` / `.spz` file-dialog filters + drag-drop | `.glb` / `.gltf` |
| bundled `butterfly.spz` auto-load | bundled `sample.glb` (see `*/assets/README.md`) |

Tip: `grep -rni "gs_\|gaussian\|GsRenderer\|\.spz\|\.ply" windows/ macos/` to
enumerate the call sites.

## Bundled asset

Add a CC0/CC-BY `sample.glb` under `windows/assets/` and `macos/assets/`
(e.g. Khronos DamagedHelmet). The build scripts copy it next to the exe and
the app auto-loads it. See `*/assets/README.md`.

## Installer + sidecar

- `installer/DisplayXRModelViewerInstaller.nsi` and the macOS `.pkg` scripts
  are renamed but assume `model_viewer_handle_vk_*` and `sample.glb`. Verify
  paths once the binary builds.
- `windows/displayxr/*.displayxr.json` + `macos/displayxr/*.displayxr.json`
  describe the shell tile. Replace the placeholder `icon.png` / `icon_sbs.png`
  (currently the splat demo's icons) with model-viewer artwork.

## Definition of done

`scripts/build_windows.bat` produces `model_viewer_handle_vk_win.exe` that
loads `sample.glb` and renders it in 3D on a DisplayXR display, with the HUD,
input, transparency toggle, and `L`-to-open dialog all working.

# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working on this repo.

## Project Overview

DisplayXR Demo — 3D Model Viewer. Real-time glTF 2.0 PBR model viewer for
glasses-free 3D displays, built on the DisplayXR OpenXR runtime via Vulkan.
Loads `.glb` / `.gltf` files, renders with asymmetric per-eye Kooima projection.

This is a **standalone repo**. It evolves independently — there is no
source-mirror from the runtime. Edit code here directly; cut your own release
tags here. The `common/` and `openxr_includes/` directories were seeded from
the runtime tree (and shared with the other DisplayXR demos) but are
maintained in this repo.

## ⚠️ Scaffold status — read PORTING.md first

This repo was scaffolded from `displayxr-demo-gaussiansplat`. State:
- **Done & reusable as-is:** `common/`, `openxr_includes/`, the window /
  OpenXR session / transparency / HUD / input glue in `windows/` + `macos/`,
  build wiring, installer, shell sidecar.
- **Skeleton (stubs):** `model_common/` — `model_renderer.*` (PBR renderer),
  `model_loader.*` (tinygltf wrapper), `shaders/pbr.{vert,frag}`.
- **Not yet retargeted:** the renderer call sites inside `windows/main.cpp`,
  `windows/xr_session.cpp`, `macos/main.mm` still call the Gaussian-splat API.
  Each carries a PORTING BASELINE banner. **These files don't compile until the
  port is done.**

`PORTING.md` has the full checklist + the file map from
[SaschaWillems/Vulkan-glTF-PBR](https://github.com/SaschaWillems/Vulkan-glTF-PBR)
(MIT — the renderer source).

## Runtime dependency

Requires the **DisplayXR runtime v1.3.0 or newer** (Vulkan transparent-window
bridge, PR #215 — the HWND is created with `WS_EX_NOREDIRECTIONBITMAP` and the
session with `transparentBackgroundEnabled = XR_TRUE`). Install via
`DisplayXRSetup-*.exe` from the
[`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases).
Apps load the runtime via the registry-resolved manifest (no `XR_RUNTIME_JSON`
needed). The shell is optional.

## Repo layout

```
.
├── macos/                 Platform entry + window handling (Cocoa/Metal/MoltenVK) — porting baseline
├── windows/               Platform entry + window handling (Win32) — porting baseline
│   ├── main.cpp           HWND creation, WindowProc message pump
│   ├── xr_session.cpp     OpenXR session create, GraphicsBindingVulkan, win32_window_binding
│   └── displayxr/         Shell tile sidecar (.displayxr.json + icons)
├── model_common/          glTF 2.0 PBR renderer: model_renderer, model_loader, model_vulkan_utils, shaders/
├── common/                Shared helpers: Kooima math, input, HUD (reused verbatim)
├── openxr_includes/       Vendored OpenXR + DisplayXR extension headers
├── installer/             Windows NSIS + macOS .pkg installers
├── scripts/               Build scripts per platform
└── PORTING.md             Skeleton → working-viewer checklist
```

## Build commands

### Windows (preferred dev path)
```bat
scripts\build_windows.bat
```
Outputs `build\windows\Release\model_viewer_handle_vk_win.exe` plus bundled DLLs.
`model_common` fetches tinygltf + glm via FetchContent on first configure.

### macOS
```bash
brew install cmake ninja vulkan-sdk openxr-loader
./scripts/build_macos.sh
```
Outputs `build/macos/model_viewer_handle_vk_macos`.

## OpenXR + Vulkan integration notes

- Uses `XR_KHR_vulkan_enable` (creates own VkInstance/Device).
- Uses `XR_EXT_win32_window_binding` for app-owned HWND.
- Uses `XR_EXT_display_info` (v12+) for display dims + rendering modes.
- Submits a single `XrCompositionLayerProjection` per frame.

The runtime's VK native compositor handles atlas → display processor →
present. The demo sets the `XR_EXT_win32_window_binding` transparency flags;
the runtime does the chroma-key / weave / DComp work.

## Transparency support (runtime ≥ v1.3.0)

App-side contract (already implemented in the platform baseline):
1. HWND created with `WS_EX_NOREDIRECTIONBITMAP` + null background brush.
2. `transparentBackgroundEnabled = XR_TRUE`, `chromaKeyColor = 0` at session create.
3. Renderer clears uncovered pixels to `RGBA(0,0,0,0)` (the `transparentBg`
   path in `ModelRenderer::renderEye`).
4. Projection layer sets `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`.

Anti-aliased edges become hard-mask alpha on Leia hardware (SR-weaver
limitation — alpha is 0 or 1).

## Coding conventions

- C++17 / C++20, Vulkan 1.0+, Objective-C++ on macOS.
- Naming: `lower_snake_case` for files/functions, `PascalCase` for C++ types.
- **Multiview-first language**: use `tile`, `view`, `atlas`. NEVER `stereo`,
  `left+right eye`, or `SBS` in new code, comments, docs, or chat.

## Build quirks

The CMakeLists has historically broken on dev paths with spaces (e.g.
`C:\Users\Sparks i7 3080\...`). Use relative manifest paths; quote paths in
custom commands. If `scripts\build_windows.bat` fails with a path-quoting
error, that's the symptom.

## Testing the dev build inside the DisplayXR Shell launcher

`scripts\dev_register.bat` drops a `%LOCALAPPDATA%\DisplayXR\apps\` manifest
pointing at this repo's dev build, which wins over the installed
`%ProgramData%` entry so the shell tile launches your dev binary.
`scripts\dev_register.bat --unregister` removes it. The build script does not
auto-register (kept hermetic).

## Releasing

Each demo cuts its own `vX.Y.Z` tag on its own cadence (independent of the
runtime). Build the installer, then `gh release create` with the asset
attached. No automated runtime-side trigger.

## Sibling repos

| Repo | Purpose |
|---|---|
| [`displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime) | The runtime. Releases ship `DisplayXRSetup-*.exe`. |
| [`displayxr-demo-gaussiansplat`](https://github.com/DisplayXR/displayxr-demo-gaussiansplat) | Sibling demo this repo was scaffolded from. |
| [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases) | Shell installer (optional add-on). |
| `displayxr-demo-<name>` | Other standalone demos with independent evolution. |

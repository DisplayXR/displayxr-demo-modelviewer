# DisplayXR Demo — 3D Model Viewer

Real-time glTF 2.0 PBR model viewer for glasses-free 3D displays, built on the
DisplayXR runtime via OpenXR with Vulkan. Loads `.glb` / `.gltf` models and
renders them with asymmetric per-eye Kooima projection for the full multiview
3D experience.

> **🚧 Scaffold status:** this repo is a scaffold seeded from
> [`displayxr-demo-gaussiansplat`](https://github.com/DisplayXR/displayxr-demo-gaussiansplat).
> The reusable DisplayXR plumbing (`common/`, `openxr_includes/`, window +
> OpenXR session + HUD + input + transparency) is in place, and `model_common/`
> holds the glTF/PBR renderer **skeleton**. The renderer itself and the
> platform renderer call sites are **not yet ported** — see
> [`PORTING.md`](PORTING.md) for the remaining work.

> **Requires the DisplayXR runtime v1.3.0 or newer** (Windows) / **the latest
> macOS runtime `.pkg`**. Download the matching installer from the
> [`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases):
> `DisplayXRSetup-*.exe` on Windows or `DisplayXR-Installer-*.pkg` on macOS.
> v1.3.0 ships the Vulkan transparent-window bridge that this demo's HWND +
> session rely on; older runtimes will produce a broken/black window. The
> shell ([`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases))
> is optional — only needed for the spatial workspace shell.

## Controls

Inherited from the demo baseline (subject to refinement during the renderer port):

| Input | Action |
|---|---|
| WASD / Q / E | Strafe the virtual display in 3D |
| Left-click drag | Rotate the virtual display |
| Scroll / trackpad | Zoom (virtual display height) |
| `-` / `=` | Decrease / increase depth + IPD together (10 %–100 %) |
| `M` | Auto-orbit: slow turntable rotation when idle |
| `V` | Cycle rendering modes advertised by the display runtime |
| `L` or top-bar **Open…** | Load a different `.glb` / `.gltf` file |
| Drag-and-drop (macOS) | Load a `.glb` / `.gltf` dropped onto the window |
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

`model_common/` fetches **tinygltf** and **glm** via CMake `FetchContent` on
first configure (no submodules).

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
REM Set OpenXR_ROOT to your OpenXR SDK install if find_package can't see it.
scripts\build_windows.bat
REM Run
build\windows\Release\model_viewer_handle_vk_win.exe
```

## Repo layout

```
.
├── macos/                  Platform entry + window handling (porting baseline)
├── windows/                Platform entry + window handling (porting baseline)
├── model_common/           glTF 2.0 PBR renderer (skeleton: loader + renderer + shaders)
├── common/                 Shared helpers: Kooima math, input, HUD (reused as-is)
├── openxr_includes/         Vendored OpenXR headers (incl. DisplayXR extensions)
├── installer/              Windows NSIS + macOS .pkg installers
├── scripts/                Build scripts for each platform
└── PORTING.md              What's left to turn the skeleton into a working viewer
```

`common/` and `openxr_includes/` are shared with the other DisplayXR demos and
were seeded from the runtime source tree.

## Why a glTF viewer (not a Gaussian-splat fork)

This is a separate demo, not a mode bolted onto the splat viewer: it shows a
different DisplayXR capability (mesh + PBR rendering) and grows the demo
gallery. The renderer is being ported from the MIT-licensed
[SaschaWillems/Vulkan-glTF-PBR](https://github.com/SaschaWillems/Vulkan-glTF-PBR).

## License

BSL-1.0 — see `LICENSE`. Bundled demo models carry their own licenses.

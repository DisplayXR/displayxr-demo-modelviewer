# Native-Wayland model viewer (Linux)

> **Branch layout note (for reviewers).** This branch is *purely additive* — it does
> not modify any existing file. The native-Wayland implementation is provided as
> reference to be folded into the unified per-app window helper
> (`feat/linux-wayland-x11-common-window`), **not** as a drop-in replacement:
> - `linux/main_native_wayland.cpp` — the full native-Wayland port (was authored as
>   `linux/main.cpp` on a July base; here renamed so the current `linux/main.cpp`
>   with the X11 CSD title bar + click-through is left untouched).
> - `linux/CMakeLists.native-wayland.txt` — the exact CMake additions
>   (wayland-scanner protocol gen, vendored libdecor header, libdecor link).
> - `scripts/`, `linux/third_party/`, `openxr_includes/` files apply as-is.
>
> Where this doc says `linux/main.cpp` / `linux/CMakeLists.txt` below, read the
> `*_native_wayland` / `.native-wayland` variants on this branch.

`linux/main.cpp` runs as a **genuine native-Wayland windowed app**
(`XR_DXR_wayland_surface_binding`) — app-owned `wl_surface` handed to the runtime,
NOT Xwayland. It's smooth / judder-free where the X11/Xwayland official build hits
the head-motion tracking judder (it bypasses the mutter desktop-capture path).

## Build

Deps (Ubuntu): `libwayland-dev wayland-protocols libdecor-0-0
libdecor-0-plugin-1-gtk` (libdecor `-dev` optional — `libdecor.h` is vendored under
`linux/third_party/`). Plus an OpenXR **loader** (`OpenXRConfig.cmake` on
`CMAKE_PREFIX_PATH`, or a discoverable `libopenxr_loader`).

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<openxr-loader-install>
cmake --build build --target model_viewer_handle_vk_linux -j$(nproc)
# exe: build/linux/model_viewer_handle_vk_linux  (loader + sample.glb auto-copied next to it)
```

## Run

Use the launcher (sets the installed-runtime + Leia SR env):

```sh
# WINDOWED, borderless, 2560x1440 (default) — place it on the 3D panel:
scripts/run_modelviewer_wayland.sh

# FULLSCREEN CRISP on the DS1 — panel must be the ONLY display at 100%:
scripts/ds1-only.sh                                   # laptop off (restore: scripts/restore-dual-monitor.sh)
DXR_MV_FULLSCREEN=1 scripts/run_modelviewer_wayland.sh
```

### Env knobs (`linux/main.cpp`)
| var | effect |
|---|---|
| `DXR_MV_WINDOW=WxH` | windowed size (launcher default 2560×1440) |
| `DXR_MV_NO_TITLEBAR=1` | borderless (launcher default); unset → libdecor title bar |
| `DXR_MV_FULLSCREEN=1` | fullscreen on the compositor-chosen output |
| `DXR_MV_OUTPUT=DP-1` | fullscreen on a named connector — **DS1-only desktops only** |
| `DXR_MV_BUFFER_SCALE=N` | override the fullscreen 1:1 buffer scale |

## Why the DS1-only recipe for crisp fullscreen

On a **mixed-DPI** desktop (e.g. laptop eDP @2× + DS1 @1×) the native-Wayland
window carries a scale-2 surface context, so the compositor configures a fullscreen
toplevel in **logical** px (1920×1080) while the panel needs a **physical**-px buffer
(3840×2160). `DXR_MV_OUTPUT` then applies a `buffer_scale=2` fix that clears the 1:1
gate but leaves a **half-height (broken) weave** — model centered in a black panel.
**DS1-only @100%** removes the mixed-DPI entirely: fullscreen configures at native
3840×2160, buffer_scale 1, and it weaves crisp. `scripts/ds1-only.sh` /
`scripts/restore-dual-monitor.sh` toggle that (GNOME/Mutter via D-Bus; edit the modes
for your hardware).

## What the port added (vs the hosted-NULL base)

1. Native-Wayland scaffold (registry / `wl_compositor` / `xdg_wm_base` / `wl_surface` /
   xdg-toplevel) → `XrWaylandSurfaceBindingCreateInfoDXR` at `xrCreateSession`.
2. **Title bar via libdecor** (client-side; GNOME has no server-side decorations).
   Drawn on separate subsurfaces so the woven content surface stays clean; the
   runtime's `#1654` logic anchors the weave to the content surface. `xdg-decoration`
   SERVER_SIDE is requested as a fallback for KDE/wlroots.
3. **`wl_output` + `DXR_MV_OUTPUT`** to fullscreen on a named connector (a Wayland
   client can't place a *windowed* toplevel on a chosen monitor).
4. **1:1 buffer-scale fix** for HiDPI-mixed fullscreen (see caveat above).
5. CMake: `wayland-scanner` for xdg-shell + xdg-decoration; link `libwayland-client` +
   `libdecor-0`. Vendors `openxr/XR_DXR_wayland_surface_binding.h`.

## Known limitations / TODO

- **No hotkeys**: no keyboard input is wired; the app auto-loads one `sample.glb`
  and runs in 3D. Model-cycle and 2D↔3D-toggle keys are a TODO (needs
  `wl_seat`/`wl_keyboard` + a model list + `xrRequestDisplayRenderingModeDXR`).
- Windowed weave is soft (per-eye half-scale); fullscreen-on-panel is crisp.
- Weave shimmers **while dragging** a windowed window, correct on drop (pre-existing
  Wayland position-during-move limitation).

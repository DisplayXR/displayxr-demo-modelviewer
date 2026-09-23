#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the NATIVE-WAYLAND model viewer against the installed DisplayXR runtime
# (XR_DXR_wayland_surface_binding). This is the app-owned-surface path — smooth /
# judder-free, unlike the X11/Xwayland official build. See linux/main.cpp.
#
# Two working configurations on a DisplayXR panel:
#
#   • WINDOWED (borderless, default here): opens a window you place on the 3D
#     panel; weaves within the window rect (soft — per-eye is half-scale). Good
#     for a desktop-style view.
#
#   • FULLSCREEN CRISP: the panel must be the ONLY display at 100% (no mixed-DPI),
#     then it fills the panel at native 3840x2160 and weaves crisp. Do:
#         ~/ds1-only.sh        # laptop off, DS1 @100%   (restore: ~/restore-dual-monitor.sh)
#         DXR_MV_FULLSCREEN=1 scripts/run_modelviewer_wayland.sh
#     NOTE: do NOT use DXR_MV_OUTPUT on a mixed-DPI desktop — the buffer-scale
#     path there produces a half-height (broken) weave. DS1-only + FULLSCREEN is
#     the crisp recipe.
#
# Env knobs (see linux/main.cpp):
#   DXR_MV_WINDOW=WxH      windowed size (default below = 2560x1440, i.e. 2x 720p)
#   DXR_MV_NO_TITLEBAR=1   borderless (default here); unset -> libdecor title bar
#   DXR_MV_FULLSCREEN=1    fullscreen on the compositor-chosen output
#   DXR_MV_OUTPUT=DP-1     fullscreen on a named connector (DS1-only desktops only)
#   DXR_MV_BUFFER_SCALE=N  override the fullscreen 1:1 buffer scale
#
# HOTKEYS: none yet — this build has no keyboard input, loads one bundled
# sample.glb, and runs in 3D. (Model-cycle / 2D-3D-toggle keys are a TODO.)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_DIR}/build/linux/model_viewer_handle_vk_linux"

# Installed runtime + Leia SR plugin (override any of these before invoking).
: "${XR_RUNTIME_JSON:=/etc/xdg/openxr/1/active_runtime.json}"
: "${XRT_PLUGIN_SEARCH_PATH:=/usr/lib/displayxr/plugins}"
: "${OXR_ENABLE_VK_NATIVE_COMPOSITOR:=1}"
: "${SR_RUNTIME_PATH:=/opt/leiasr/lib/libLeiaSR_runtime.so}"
: "${DXR_LEIA_FORCE_PROBE:=1}"
: "${DXR_MV_WINDOW:=2560x1440}"
: "${DXR_MV_NO_TITLEBAR:=1}"
export XR_RUNTIME_JSON XRT_PLUGIN_SEARCH_PATH OXR_ENABLE_VK_NATIVE_COMPOSITOR \
       SR_RUNTIME_PATH DXR_LEIA_FORCE_PROBE DXR_MV_WINDOW DXR_MV_NO_TITLEBAR

# The bundled OpenXR loader is copied next to the exe by the build; also allow a
# dev loader dir via OPENXR_LOADER_DIR.
export LD_LIBRARY_PATH="${OPENXR_LOADER_DIR:-}${OPENXR_LOADER_DIR:+:}$(dirname "${BIN}"):${LD_LIBRARY_PATH:-}"

if [[ ! -x "${BIN}" ]]; then
    echo "error: ${BIN} not built. Configure with an OpenXR loader on CMAKE_PREFIX_PATH, then:" >&2
    echo "       cmake --build build --target model_viewer_handle_vk_linux -j\$(nproc)" >&2
    exit 1
fi

echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON}"
echo "DXR_MV_WINDOW=${DXR_MV_WINDOW}  DXR_MV_NO_TITLEBAR=${DXR_MV_NO_TITLEBAR}  DXR_MV_FULLSCREEN=${DXR_MV_FULLSCREEN:-0}"
exec "${BIN}" "$@"

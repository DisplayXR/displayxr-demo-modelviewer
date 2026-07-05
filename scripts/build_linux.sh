#!/usr/bin/env bash
#
# scripts/build_linux.sh — Build the Linux demo binary (build-green, issue #40).
#
# Mirrors scripts/build_macos.sh per the runtime repo's
# docs/guides/linux-demo-port.md, with the Linux swaps: system Vulkan
# (libvulkan-dev — no MoltenVK, no ICD manifest), a from-source OpenXR loader
# pinned to loader release 1.1.43 (do NOT bump — keep it == the demo's other
# platform pins), and no installer step (Linux packaging is out of scope until
# on-screen lands).
#
# Usage:
#   ./scripts/build_linux.sh
#
# Deps (Ubuntu): see .github/workflows/build-linux.yml — build-essential cmake
#   ninja-build pkg-config libvulkan-dev vulkan-validationlayers glslang-tools,
#   plus the X11/Wayland + ALSA dev headers pulled in when the OpenXR loader is
#   built from source with GL/EGL headers present (GLX-flip: libxcb-glx0-dev
#   libxxf86vm-dev). modelviewer is a plain glTF/USD/OBJ/FBX/STL viewer — no
#   FFmpeg or SDL dependency, unlike mediaplayer.
#
# Env:
#   OPENXR_VERSION   OpenXR-SDK release tag for the loader (default 1.1.43).
#                    Keep this pin equal to CI (build-linux.yml). Do NOT bump.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Build OpenXR loader from source -----------------------------------
# Distro loaders lag; build the pinned Khronos loader and install it under
# /tmp/openxr-install (mirrors build_macos.sh + the runtime repo's
# scripts/build_linux.sh --apps). Cached: skipped if both the .so and the
# CMake package config are already present.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.43}"
OPENXR_DIR="/tmp/openxr-install"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.so" ] || \
   [ ! -f "$OPENXR_DIR/lib/cmake/openxr/OpenXRConfig.cmake" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION -> $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk "$OPENXR_DIR"
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
# Vulkan via the system libvulkan-dev; the OpenXR loader via CMAKE_PREFIX_PATH.
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build build

BIN="$REPO_ROOT/build/linux/model_viewer_handle_vk_linux"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

echo ""
echo "Built: $BIN"

# --- 2. dev run launcher --------------------------------------------------
# Emit a run script that points the app at a dev runtime + sim-display plug-in
# and forces the Linux Vulkan native compositor. (On-screen validation is a
# separate pass gated on the runtime's Linux present + a GPU + an X server.)
RUN="$REPO_ROOT/build/linux/run_modelviewer_linux.sh"
cat > "$RUN" <<'EOF'
#!/usr/bin/env bash
# Run model_viewer_handle_vk_linux against a dev DisplayXR runtime.
#   XR_RUNTIME_JSON            — dev runtime manifest (edit to your build).
#   XRT_PLUGIN_SEARCH_PATH     — sim-display plug-in dir (POSIX discovery).
#   OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 — Phase-1 vk_native path.
#   SIM_DISPLAY_OUTPUT         — anaglyph|sbs|blend for eyeball checks.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
: "${XR_RUNTIME_JSON:=$HOME/.config/openxr/1/active_runtime.json}"
: "${XRT_PLUGIN_SEARCH_PATH:=$DIR/lib/displayxr/plugins}"
: "${SIM_DISPLAY_OUTPUT:=anaglyph}"
export XR_RUNTIME_JSON XRT_PLUGIN_SEARCH_PATH SIM_DISPLAY_OUTPUT
export OXR_ENABLE_VK_NATIVE_COMPOSITOR=1
export LD_LIBRARY_PATH="$DIR:${LD_LIBRARY_PATH:-}"
exec "$DIR/model_viewer_handle_vk_linux" "$@"
EOF
chmod +x "$RUN"

echo "Run against a dev runtime: $RUN"

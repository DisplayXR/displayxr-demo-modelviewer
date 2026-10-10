#!/usr/bin/env bash
#
# scripts/run_macos_dev.sh — launch the locally-built model viewer against a
# DisplayXR macOS runtime: an explicit one (DISPLAYXR_RUNTIME_DIR /
# DISPLAYXR_RUNTIME_JSON), else the runtime repo's dev package if present, else
# an installed runtime (DisplayXRBundle .pkg). Same lookup as earthview's.
#
# Env:
#   DISPLAYXR_RUNTIME_DIR   a runtime package dir holding openxr_displayxr.json
#                           (a dev _package/DisplayXR-macOS, or an install dir).
#   DISPLAYXR_RUNTIME_JSON  a runtime manifest to use as-is (e.g. a runtime
#                           build tree's openxr_displayxr-dev.json); the
#                           MoltenVK ICD and plug-ins still come from the
#                           package found by the lookup below.
#   XRT_PLUGIN_SEARCH_PATH  kept if already set (e.g. a vendor plug-in's own
#                           build tree); otherwise derived from the package.
#
# Why this script exists
# ----------------------
# The dev binary links Homebrew's Vulkan loader (an absolute install name),
# while the runtime loads ITS libvulkan via @rpath. With two different
# libvulkan images in one process, the VkInstance the app creates is foreign
# to the runtime's loader and xrGetVulkanGraphicsDeviceKHR fails with
# VK_ERROR_INITIALIZATION_FAILED (the compositor then falls back to null).
#
# The fix is to make the app AND the runtime resolve a single shared loader.
# We point the runtime's @rpath libvulkan at Homebrew's loader (the one the app
# hardcodes) via DYLD_LIBRARY_PATH, and select the runtime's bundled MoltenVK
# (a portability driver) via VK_ICD_FILENAMES. DYLD_* survives because this
# script execs the binary DIRECTLY — exactly like the installed .app launcher.
# (Launching through a SIP-protected intermediary such as `nohup` would purge
# DYLD_* and reintroduce the mismatch.)
#
# The distributed .app (built by scripts/build_macos.sh --installer) bundles
# its own self-consistent Vulkan stack and needs none of this — this script is
# only for iterating on a dev build against a dev or installed runtime.
set -euo pipefail

# --- Locate a runtime: explicit, then dev build, then installed -------------
RT=""
DEV_RT="$HOME/Documents/GitHub/displayxr-runtime/_package/DisplayXR-macOS"
for d in "${DISPLAYXR_RUNTIME_DIR:-}" "$DEV_RT" \
         "/Library/Application Support/DisplayXR" \
         "$HOME/Library/Application Support/DisplayXR"; do
    if [ -n "$d" ] && [ -f "$d/openxr_displayxr.json" ]; then RT="$d"; break; fi
done
if [ -z "$RT" ]; then
    echo "Error: DisplayXR runtime not found." >&2
    echo "       Set DISPLAYXR_RUNTIME_DIR, build the runtime dev package" >&2
    echo "       ($DEV_RT), or install the macOS bundle (DisplayXRBundle-*.pkg)" >&2
    echo "       from https://github.com/DisplayXR/displayxr-installer/releases" >&2
    exit 1
fi
echo "==> Runtime package: $RT"

if [ -n "${DISPLAYXR_RUNTIME_JSON:-}" ]; then
    if [ ! -f "$DISPLAYXR_RUNTIME_JSON" ]; then
        echo "Error: DISPLAYXR_RUNTIME_JSON=$DISPLAYXR_RUNTIME_JSON not found." >&2
        exit 1
    fi
    export XR_RUNTIME_JSON="$DISPLAYXR_RUNTIME_JSON"
else
    export XR_RUNTIME_JSON="$RT/openxr_displayxr.json"
fi
echo "==> Runtime manifest: $XR_RUNTIME_JSON"

# Dev package keeps plug-ins under lib/displayxr/plugins; an installed runtime
# uses DisplayProcessors. A caller-provided search path wins.
if [ -z "${XRT_PLUGIN_SEARCH_PATH:-}" ]; then
    if [ -d "$RT/lib/displayxr/plugins" ]; then
        export XRT_PLUGIN_SEARCH_PATH="$RT/lib/displayxr/plugins"
    else
        export XRT_PLUGIN_SEARCH_PATH="$RT/DisplayProcessors"
    fi
fi
echo "==> Plug-in search path: $XRT_PLUGIN_SEARCH_PATH"
export VK_ICD_FILENAMES="$RT/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"

# Converge app + runtime on one libvulkan (see header).
VK_PREFIX="$(brew --prefix vulkan-loader 2>/dev/null || true)"
if [ -z "$VK_PREFIX" ] || [ ! -d "$VK_PREFIX/lib" ]; then
    echo "Error: Homebrew vulkan-loader not found — \`brew install vulkan-loader\`." >&2
    exit 1
fi
export DYLD_LIBRARY_PATH="$VK_PREFIX/lib:${DYLD_LIBRARY_PATH:-}"

# dyld resolves DYLD_LIBRARY_PATH by LEAF name even for the loader's absolute
# dlopen of the manifest's library_path, so any other openxr_displayxr.dylib on
# DYLD_LIBRARY_PATH (a vendor plug-in tree's env.sh adds its own runtime build)
# silently replaces the runtime the manifest names. Put the manifest's runtime
# directory first so the runtime you asked for is the one that loads.
RT_LIB="$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$XR_RUNTIME_JSON" | head -n 1)"
if [ -n "$RT_LIB" ]; then
    case "$RT_LIB" in
        /*) ;;
        *) RT_LIB="$(cd "$(dirname "$XR_RUNTIME_JSON")" && pwd)/$RT_LIB" ;;
    esac
    export DYLD_LIBRARY_PATH="$(dirname "$RT_LIB"):$DYLD_LIBRARY_PATH"
    echo "==> Runtime library: $RT_LIB"
fi

# --- Launch ----------------------------------------------------------------
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/macos/model_viewer_handle_vk_macos"
if [ ! -x "$BIN" ]; then
    echo "Error: $BIN not found — build first: ./scripts/build_macos.sh" >&2
    exit 1
fi
echo "==> Launching $BIN"
exec "$BIN" "$@"

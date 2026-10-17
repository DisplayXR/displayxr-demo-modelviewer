#!/usr/bin/env bash
#
# scripts/build_macos.sh — Build the demo binary, and optionally the .pkg
# installer for distribution.
#
# Usage:
#   ./scripts/build_macos.sh                run cmake build only
#   ./scripts/build_macos.sh --installer    also stage artifacts and build
#                                            _package/DisplayXRModelViewer-*.pkg
#   ./scripts/build_macos.sh -DFOO=bar      any -D argument is forwarded to the
#                                            configure step (offline overrides:
#                                            -DFETCHCONTENT_SOURCE_DIR_TINYUSDZ=...)
#
# Env:
#   DISPLAYXR_VERSION   version string baked into the .pkg + .app Info.plist
#                       (defaults to 0.0.0-dev if unset). CI sets this from
#                       the v* git tag.
#   OPENXR_VERSION      OpenXR-SDK release tag to build the loader from
#                       (defaults to 1.1.51 == XR_CURRENT_API_VERSION of the
#                       vendored openxr_includes/ headers; checked below).
#   OPENXR_SDK_SRC      offline: an existing OpenXR-SDK-Source checkout at
#                       release-$OPENXR_VERSION, used instead of git clone.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_INSTALLER=false
CMAKE_EXTRA_ARGS=()
for arg in "${@}"; do
    case "$arg" in
        --installer) BUILD_INSTALLER=true ;;
        -D*) CMAKE_EXTRA_ARGS+=("$arg") ;;   # forwarded to the configure step
        -h|--help)
            sed -n '1,23p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown arg: $arg" >&2
            exit 2
            ;;
    esac
done

# --- 0. Build OpenXR loader from source -----------------------------------
# Homebrew has no `openxr-loader` formula, so we build the loader from the
# Khronos SDK and install it under /tmp/openxr-install-<version>. This mirrors
# the DisplayXR runtime repo's approach (its scripts/build_macos.sh). The
# BUILD_WITH_SYSTEM_JSONCPP=OFF + jsoncpp include-order patch are required
# to avoid baking a Homebrew jsoncpp path into the redistributable dylib
# (runtime repo issue #205).
#
# The cache dir is keyed by version: other DisplayXR checkouts on the same Mac
# build their loader into /tmp too, and an unversioned /tmp/openxr-install would
# silently hand this build whatever release was built there first.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.51}"
HEADER_VERSION="$(sed -nE 's/^#define XR_CURRENT_API_VERSION XR_MAKE_VERSION\(([0-9]+), *([0-9]+), *([0-9]+)\).*/\1.\2.\3/p' \
    "$REPO_ROOT/openxr_includes/openxr/openxr.h")"
if [ "$OPENXR_VERSION" != "$HEADER_VERSION" ]; then
    echo "Warning: OpenXR loader $OPENXR_VERSION != vendored header rev $HEADER_VERSION" >&2
    echo "         (bump the default above with the headers; keep it == build-windows.yml)" >&2
fi
OPENXR_DIR="/tmp/openxr-install-$OPENXR_VERSION"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.dylib" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION → $OPENXR_DIR"
    OPENXR_SRC=/tmp/openxr-sdk-$OPENXR_VERSION
    rm -rf "$OPENXR_SRC"
    if [ -n "${OPENXR_SDK_SRC:-}" ]; then
        cp -R "$OPENXR_SDK_SRC" "$OPENXR_SRC"
        rm -rf "$OPENXR_SRC/build"   # a copied CMakeCache points at the original dir
    else
        git clone --depth 1 --branch "release-$OPENXR_VERSION" \
            https://github.com/KhronosGroup/OpenXR-SDK-Source.git "$OPENXR_SRC"
    fi

    # Idempotent (the address skips an already-patched line): an OPENXR_SDK_SRC
    # checkout reused from an earlier build may already carry the patch.
    /usr/bin/sed -i '' \
        '/BEFORE PRIVATE/!s|PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|' \
        "$OPENXR_SRC/src/loader/CMakeLists.txt"

    cmake -B "$OPENXR_SRC/build" -S "$OPENXR_SRC" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
        -DCMAKE_MAP_IMPORTED_CONFIG_RELEASE="Release;None;"
    cmake --build "$OPENXR_SRC/build"
    cmake --install "$OPENXR_SRC/build"
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR" \
    ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}
cmake --build build

BIN="$REPO_ROOT/build/macos/model_viewer_handle_vk_macos"
if [ ! -x "$BIN" ]; then
    echo "Error: expected binary not found at $BIN" >&2
    exit 1
fi

if [ "$BUILD_INSTALLER" != "true" ]; then
    echo ""
    # Use the dev launcher, not the bare binary: it converges the app and the
    # installed runtime on one Vulkan loader (the bare binary links Homebrew's
    # libvulkan while the runtime loads its own → xrGetVulkanGraphicsDeviceKHR
    # fails). The distributed .app handles this itself; this is dev-only.
    echo "Run: ./scripts/run_macos_dev.sh"
    exit 0
fi

# --- 2. stage artifacts for the installer ---------------------------------
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
echo "==> Staging installer artifacts (version=$VERSION)"

ARTIFACT_DIR="$REPO_ROOT/_artifact"
rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR/bin" "$ARTIFACT_DIR/lib" "$ARTIFACT_DIR/assets" "$ARTIFACT_DIR/displayxr"

cp "$BIN" "$ARTIFACT_DIR/bin/"

# Bundled scene + manifest + icons live in the repo.
if [ -f "$REPO_ROOT/assets/material_grid.glb" ]; then
    cp "$REPO_ROOT/assets/material_grid.glb" "$ARTIFACT_DIR/assets/"
fi
if [ -f "$REPO_ROOT/macos/assets/sample.glb" ]; then
    cp "$REPO_ROOT/macos/assets/sample.glb" "$ARTIFACT_DIR/assets/"
fi
if [ -d "$REPO_ROOT/macos/displayxr" ]; then
    cp -R "$REPO_ROOT/macos/displayxr/." "$ARTIFACT_DIR/displayxr/"
fi

# Bundled OpenXR loader (built from source above).
cp -P "$OPENXR_DIR"/lib/libopenxr_loader*.dylib "$ARTIFACT_DIR/lib/"

# Bundled Vulkan + MoltenVK via Homebrew. Resolve through `brew --prefix
# <formula>` so we don't hardcode arm64 (/opt/homebrew) vs x86_64
# (/usr/local) — CI runners on macos-14 and local Apple Silicon dev
# machines diverge here.
copy_from_brew_formula() {
    local formula="$1"
    local pattern="$2"
    local prefix
    prefix="$(brew --prefix "$formula" 2>/dev/null || true)"
    if [ -z "$prefix" ] || [ ! -d "$prefix" ]; then
        echo "Error: brew formula '$formula' not found — \`brew install $formula\` first" >&2
        return 1
    fi
    local found=0
    while IFS= read -r f; do
        cp -P "$f" "$ARTIFACT_DIR/lib/"
        found=1
    done < <(find "$prefix/lib" -maxdepth 1 \( -name "$pattern" -type f -o -name "$pattern" -type l \) 2>/dev/null)
    if [ "$found" = 0 ]; then
        echo "Error: no '$pattern' under $prefix/lib" >&2
        return 1
    fi
}

copy_from_brew_formula vulkan-loader 'libvulkan*.dylib'
copy_from_brew_formula molten-vk     'libMoltenVK*.dylib'

# Sanity-check the three families actually got something.
for required in libopenxr_loader libvulkan libMoltenVK; do
    if ! ls "$ARTIFACT_DIR/lib/${required}"* >/dev/null 2>&1; then
        echo "Error: $required not staged under _artifact/lib/" >&2
        echo "       Install with: brew install vulkan-loader molten-vk" >&2
        exit 1
    fi
done

# --- 3. .pkg ---------------------------------------------------------------
PKG_DIR="$REPO_ROOT/_package"
mkdir -p "$PKG_DIR"
PKG_PATH="$PKG_DIR/DisplayXRModelViewer-${VERSION}.pkg"

DISPLAYXR_VERSION="$VERSION" \
    bash "$REPO_ROOT/installer/macos/build_installer.sh" "$ARTIFACT_DIR" "$PKG_PATH"

echo ""
echo "==> Installer: $PKG_PATH"
ls -lh "$PKG_PATH"

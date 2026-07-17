#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the Linux model-viewer demo against a local DisplayXR dev runtime.
# Mirrors displayxr-demo-avatar/scripts/run_avatar_linux.sh so the runtime's
# scripts/run_linux_demo.sh harness (which searches THIS scripts/ dir for
# run_*linux*.sh) can launch it. build_linux.sh also emits an equivalent script
# into build/linux/, but that isn't on the harness's search path (#699 / #706).
#
# Usage: scripts/run_modelviewer_linux.sh [model.glb|.gltf] [extra args...]
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${REPO_DIR}/build/linux"
BIN="${BIN_DIR}/model_viewer_handle_vk_linux"
RUNTIME_BUILD="${REPO_DIR}/../displayxr-runtime/build"

# Sibling runtime checkout's dev manifest by default; override via XR_RUNTIME_JSON.
: "${XR_RUNTIME_JSON:=${RUNTIME_BUILD}/openxr_displayxr-dev.json}"
export XR_RUNTIME_JSON

# Sim-display plug-in discovery (POSIX search path; the runtime's build stages
# DisplayXR-SimDisplay.so + its manifest here).
: "${XRT_PLUGIN_SEARCH_PATH:=${RUNTIME_BUILD}/_plugins}"
export XRT_PLUGIN_SEARCH_PATH

# Native Vulkan compositor (Phase 1 vk_native + VK_KHR_xcb_surface path).
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"

# Sim-display weave for eyeball checks without 3D hardware.
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"

# The OpenXR loader is staged next to the binary by build_linux.sh.
export LD_LIBRARY_PATH="${BIN_DIR}:${LD_LIBRARY_PATH:-}"

if [[ ! -f "${XR_RUNTIME_JSON}" ]]; then
    echo "warning: XR_RUNTIME_JSON not found: ${XR_RUNTIME_JSON}" >&2
    echo "         build the runtime (scripts/build_linux.sh there) or set XR_RUNTIME_JSON." >&2
fi
if [[ ! -x "${BIN}" ]]; then
    echo "error: ${BIN} not built. Run: ./scripts/build_linux.sh" >&2
    exit 1
fi

echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON}"
echo "XRT_PLUGIN_SEARCH_PATH=${XRT_PLUGIN_SEARCH_PATH}"
exec "${BIN}" "$@"

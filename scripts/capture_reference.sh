#!/usr/bin/env bash
#
# scripts/capture_reference.sh — reproducible reference renders (issue #70 phase 3)
#
# Captures the viewer's output for a given asset and writes a sidecar recording
# the exact conditions it was taken under. A reference render nobody can
# reproduce is a screenshot, not a reference: the whole premise of #70 is that a
# material comparison only means something when the environment, exposure and
# tone curve are pinned AND written down.
#
# Determinism comes from DXR_MODELVIEWER_DETERMINISTIC=1, which pins the idle
# auto-orbit off. Without it the viewer starts rotating ~10 s after launch, so
# two runs of this script would frame the model differently — measured at 97.49%
# of channels differing between captures 12 s apart.
#
# Usage:
#   scripts/capture_reference.sh <asset.glb> [outdir] [--hdri env.hdr] [--wait N]
#
# Example:
#   scripts/capture_reference.sh assets/material_grid.glb docs/reference
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

ASSET=""; OUTDIR="reference"; HDRI=""; WAIT=15
while [ $# -gt 0 ]; do
    case "$1" in
        --hdri) HDRI="$2"; shift 2 ;;
        --wait) WAIT="$2"; shift 2 ;;
        -h|--help) sed -n '1,22p' "$0"; exit 0 ;;
        *) if [ -z "$ASSET" ]; then ASSET="$1"; else OUTDIR="$1"; fi; shift ;;
    esac
done
[ -n "$ASSET" ] || { echo "usage: $0 <asset.glb> [outdir] [--hdri env.hdr] [--wait N]" >&2; exit 2; }
[ -f "$ASSET" ] || { echo "no such asset: $ASSET" >&2; exit 2; }

case "$(uname -s)" in
    Darwin) EXE="build/macos/model_viewer_handle_vk_macos"; RUN="./scripts/run_macos_dev.sh" ;;
    Linux)  EXE="build/linux/model_viewer_handle_vk_linux"; RUN="./scripts/run_modelviewer_linux.sh" ;;
    *) echo "unsupported platform (Windows: run the .exe with DXR_MODELVIEWER_DETERMINISTIC=1)" >&2; exit 2 ;;
esac
[ -x "$EXE" ] || { echo "not built: $EXE — run the platform build script first" >&2; exit 2; }

EXEDIR="$(dirname "$EXE")"
STEM="$(basename "${ASSET%.*}")"
TMP="${TMPDIR:-/tmp}"
mkdir -p "$OUTDIR"

# The viewer loads whatever sits next to the executable as sample.glb. NOTE the
# build scripts re-copy the bundled asset over it, so this must run AFTER any
# build — staging before a build silently captures the wrong model.
cp "$ASSET" "$EXEDIR/sample.glb"
if [ -n "$HDRI" ]; then cp "$HDRI" "$EXEDIR/environment.hdr"; else rm -f "$EXEDIR/environment.hdr"; fi

pkill -f "$(basename "$EXE")" 2>/dev/null || true
sleep 1
LOG="$TMP/dxr_reference_capture.log"
( env DXR_MODELVIEWER_DETERMINISTIC=1 $RUN > "$LOG" 2>&1 & )
sleep "$WAIT"

rm -f "$TMP/displayxr_atlas.png"
touch "$TMP/displayxr_atlas_trigger"
sleep 4
if [ ! -f "$TMP/displayxr_atlas.png" ]; then
    echo "capture failed — no atlas written. Runtime log tail:" >&2
    tail -20 "$LOG" >&2
    pkill -f "$(basename "$EXE")" 2>/dev/null || true
    exit 1
fi
cp "$TMP/displayxr_atlas.png" "$OUTDIR/$STEM.png"

# The sidecar is the point: it records what the image is a reference OF.
# Pulled from the runtime's own log rather than restated here, so it cannot
# drift from what the viewer actually did.
{
    echo "asset:       $ASSET"
    echo "platform:    $(uname -s) $(uname -m)"
    echo "viewer:      $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "determinism: DXR_MODELVIEWER_DETERMINISTIC=1 (auto-orbit pinned off)"
    grep -E "environment →|No bundled environment" "$LOG" | tail -1 | sed 's/^/environment: /' || true
    grep -E "sampled images per stage" "$LOG" | tail -1 | sed 's/^/limits:      /' || true
    grep -E "NOT IMPLEMENTED" "$LOG" | tail -1 | sed 's/^/ignored:     /' || echo "ignored:     none"
    echo "note:        exposure and tone curve are the viewer defaults unless"
    echo "             changed interactively; the HUD reports both live."
} > "$OUTDIR/$STEM.txt"

pkill -f "$(basename "$EXE")" 2>/dev/null || true
echo "wrote $OUTDIR/$STEM.png"
echo "wrote $OUTDIR/$STEM.txt"

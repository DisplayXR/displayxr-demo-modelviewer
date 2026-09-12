#!/usr/bin/env bash
# Fetch the DisplayXR client wake library AAR pinned by android/build.gradle.
#
# The library ships as a release asset on the runtime repo (no Maven: the org's
# GitHub Packages budget is pinned at $0 with a hard stop, so publishing there
# would break every workflow in the org, not just this one).
#
#   ./android/fetch-client-aar.sh            # the pinned tag
#   ./android/fetch-client-aar.sh v2.16.29   # a specific tag
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TAG="${1:-$(grep -oE "dxrClientRuntimeTag *= *'[^']+'" "$HERE/build.gradle" | grep -oE "v[0-9]+\.[0-9]+\.[0-9]+")}"
[ -n "$TAG" ] || { echo "Could not read dxrClientRuntimeTag from build.gradle" >&2; exit 1; }
command -v gh >/dev/null || { echo "gh CLI required" >&2; exit 1; }
mkdir -p "$HERE/libs"
# Remove older copies so the gradle resolver cannot pick a stale one.
rm -f "$HERE"/libs/DisplayXR-Client-*.aar
gh release download "$TAG" -R DisplayXR/displayxr-runtime -p 'DisplayXR-Client-*.aar' -D "$HERE/libs"
ls -la "$HERE/libs"

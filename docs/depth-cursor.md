# Depth-aware cursor — how this demo adopts it (and how another demo can)

The DisplayXR runtime draws a **depth cursor**: where content pops out of the
glass under the pointer, the arrow is lifted just in front of it, at the right
disparity; over empty space or content on/behind the glass, nothing changes and
the OS cursor stays the cursor (runtime ADR-046 Amendment 2, extension
`XR_DXR_cursor_depth` v5+). The runtime owns the drawing, the margin, the
comfort clamp and the smoothing. **The app owns one thing: the depth under the
pointer.**

This demo is the pilot. Everything below is what it took.

## What it costs

| Piece | Where | Lines |
|---|---|---|
| Ray cast (BVH per rigid cluster) | `model_common/model_raycast.{h,cpp}` | 333 |
| Renderer hook (build at load, follow the pose) | `model_common/model_renderer.{h,cpp}` | 36 |
| Shared adoption helper (opt-out, scripted pointer, cost line) | `common/model_cursor_depth.h` | 136 |
| Per platform leg | `macos/main.mm` 22, `linux/main.cpp` 31, `windows/{main.cpp,xr_session.*}` 27 | ~20–30 each (most of it comments) |
| Vendored header + pins | `openxr_includes/openxr/XR_DXR_cursor_depth.h`, `VENDORED.*`, `common/CMakeLists.txt` | — |

The integration itself — the part every demo repeats — is ~15–30 lines per leg.
The depth source is the only part that is genuinely per-demo.

## The recipe

1. **Pin** displayxr-common **v2.29.0+** (`dxr::CursorDepth`, `common/cursor_depth.h`)
   and vendor `XR_DXR_cursor_depth.h` (spec ≥ 5) from the runtime into
   `openxr_includes/`, with a `VENDORED.json` pin
   (`scripts/check_vendored_headers.py` must pass).
2. **Enable the extension** at `xrCreateInstance` when the runtime lists it and
   `DISPLAYXR_CURSOR_DEPTH` is not `0`; keep its `extensionVersion` (0 = off).
3. **Set up the helper** once, before the frame loop:
   ```cpp
   dxr::CursorDepth cursor;
   modelviewer::CursorDepthAdoption cursorAdopt;          // or your own copy of it
   cursorAdopt.setup(cursor, renderer, specVersionOr0);   // enable + ray cast + scripted UV
   cursor.setWindowCocoa(view);    // macOS;  Windows: cursor.setWindowWin32(hwnd)
   ```
   On Linux (`displayxr::linux_window`, X11 or Wayland) use the manual path:
   `cursor.setPointerUV(x / w, y / h)` on Motion, `cursor.clearPointerUV()` on
   PointerLeave, and
   `cursor.onDepthCursorShownChanged([](bool s){ win.set_cursor(s ? DxrCursor::Hidden : DxrCursor::Default); })`.
4. **Per frame**, around the app's own `xrLocateViews` (after every other
   `locateInfo.next` / `viewState.next` chain is built — `chain()` prepends):
   ```cpp
   cursor.chain(locateInfo, viewState);
   xrLocateViews(...);
   cursor.afterLocate(views, activeViewCount);   // 1 in a 2D mode = no cursor work
   ```
   `views` must be the render-ready views in the locate space (under
   `XR_DXR_view_rig` they are the world the scene lives in).
5. **Depth source** — `setRayCast(ray -> optional<float t>)`: nearest hit along
   the ray, in the locate space. It is called ~20 times a frame (10 footprint
   points × the two outer views), so it must be cheap.

That is all. The OS cursor is hidden exactly while the runtime reports
`willDraw`, so a compositor that cannot draw yet (D3D11/D3D12 before runtime
#1944) simply leaves the flat OS cursor — never no cursor.

## This demo's depth source

`ModelRenderer::raycast` (`model_common/model_raycast.h`):

- **One BVH per rigid cluster**, built once at load in the cluster's own space
  (median split, ≤ 4 triangles per leaf, triangles stored pre-differenced for
  Möller–Trumbore, two-sided).
- A non-skinned primitive is one cluster, transformed by its model matrix; a
  skinned primitive is split by **dominant joint** and transformed by that
  joint's skinning matrix (rigid skinning — exact on a single-joint triangle,
  off by the blend in the seams). The ray is mapped into each cluster by the
  inverse 3×4; an affine map preserves `t`, so animation never rebuilds a BVH.
- Morph targets are not followed (the BVH holds the base shape).
- The model is everything that can stick out: the skybox is at infinity and all
  UI here is 2D, so no other geometry joins the ray cast.

Measured on an M-series Mac (Release, 20 rays/frame):

| Model | Triangles | BVH build | Ray cast / frame |
|---|---|---|---|
| DamagedHelmet (bundled) | 15 k | 4 ms | ~25 µs mean |
| CesiumMan (skinned, animating) | 4.7 k / 19 clusters | 1 ms | ~14 µs mean |
| photogrammetry boot | 2.35 M | ~1 s | ~50 µs mean, ~200 µs max |
| pointer over empty space | — | — | ~1 µs (root boxes miss) |

## Testing it without a 3D display

```bash
DISPLAYXR_RUNTIME_DIR=<runtime>/_package/DisplayXR-macOS \
DXR_MODELVIEWER_DETERMINISTIC=1 DISPLAYXR_CURSOR_DEPTH_UV=0.5,0.5 \
  ./scripts/run_macos_dev.sh
touch "$TMPDIR/displayxr_atlas_trigger.model_viewer_handle_vk_macos"   # vk_native capture
# -> $TMPDIR/displayxr_atlas.model_viewer_handle_vk_macos.png
```

`DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the pointer (canvas UV, top-left
origin) and adds the runtime's placement to the throttled log line:

```
cursor depth: ray cast 28.7 us/frame mean, 75.1 us max, 20.0 rays/frame over 600 frames
  (hit 600, runtime drew 600); placement active=1 disparity=-0.072 ...
```

Use the **per-exe** trigger: the plain `displayxr_atlas_trigger` is consumed by
whichever DisplayXR app polls first. Expected: over a part of the model in
front of the glass the arrow appears in both tiles with crossed disparity
(further right in the first tile); over empty space or a part behind the glass
nothing is drawn (`runtime drew 0`). `DISPLAYXR_CURSOR_DEPTH=0` turns it all off.

## For the other demos

The leg integration (steps 1–4) is copy-paste. What changes is step 5:

- **Mesh content** (avatar, earthview tiles): a CPU ray cast like this one —
  reuse `model_raycast` for the meshes, or the engine's own picking if it has
  one.
- **Gaussian splats**: no triangles. Ray-march the splat set against a coarse
  grid / octree of splat bounds, or (simpler, often enough) submit depth and
  use `useDepthLayer(true)` once the runtime reads submitted depth on that
  backend.
- **Video / photo** (mediaplayer): the depth lives in the frame (disparity /
  depth map); either sample the frame's depth under the pointer, or submit
  depth with the projection layer and use `useDepthLayer(true)`.

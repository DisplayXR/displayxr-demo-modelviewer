// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The viewer's load-time framing rule, kept in step with the WEB rule.
 *
 * `dxr::AutoFitVHeight` (displayxr-common) frames an asset so neither rendered
 * axis exceeds `fill` of the viewport, using the AABB's WIDTH as the
 * horizontal extent. That is the right rule for a static, face-on subject and
 * it is what this viewer used through v0.27.0.
 *
 * It is NOT the rule the page uses, and the page is the reference whenever a
 * model is UNDOCKED: the storefront lifts a product out of its tile into this
 * viewer at the tile's own pixel rect and sends no `--vh`, so the two framings
 * have to agree or the product visibly changes size as it leaves the page.
 * The web rule is `SceneViewer.fitTo` in the inline3d SDK, with the defaults
 * `addModel` passes (`fit:'contain'`, `margin:0.8`, `fitSweep:true`,
 * `depthLimit:4.0`):
 *
 *     vW    = vH * aspect
 *     horiz = hypot(extentX, extentZ)               // fitSweep
 *     s     = min(margin*vH / extentY,              // height cap
 *                 margin*vW / horiz,                // swept-width cap
 *                 depthLimit*vH / extentZ)          // depth backstop
 *
 * Two differences from `AutoFitVHeight`, both of which only bite on a DEEP
 * subject — which is why a bottle undocked at the right size and a backpack
 * did not:
 *
 *  1. THE HORIZONTAL EXTENT IS THE SWEPT DIAGONAL, NOT THE WIDTH. Both the
 *     page and this viewer turntable the subject about Y, which swings DEPTH
 *     into the horizontal, so a subject fitted to its face-on width overflows
 *     the tile the moment it turns. `hypot(W, D)` bounds every yaw. A subject
 *     with D <= W is unaffected in practice (the height cap still binds);
 *     a deep one is framed SMALLER by the page than by the old rule here,
 *     i.e. the undocked copy came out larger than the tile it left.
 *
 *  2. A DEPTH BACKSTOP. Something pathologically deep is scaled down so its
 *     total depth stays within `depthLimit` display heights, rather than
 *     pushing a metre of virtual space through the glass.
 *
 * Expressed as the rig vHeight this app actually sets (rendered height
 * fraction = extentY / vHeight, and `s = vH / vHeight` above), the whole rule
 * is vH-independent:
 *
 *     vHeight = max(extentY / fill,
 *                   hypot(extentX, extentZ) / (fill * aspect),
 *                   extentZ / depthLimit)
 *
 * The first two terms are exactly `dxr::AutoFitVHeight` with the swept extent
 * passed as its width, so this composes the shared helper rather than
 * re-deriving it. `--vh` still overrides the result: the caller asserting a
 * scale outranks a guess.
 *
 * WHAT GOES IN: ROBUST CENTRING, NOT THE BOUNDING BOX (#192, the default on
 * every leg). The box does three jobs and one thin outlier (a tail, an
 * antenna, a strap) skews all three, so the centre and the extents handed to
 * the rule above come from an AREA-WEIGHTED sample of the surface instead
 * (displayxr-common's `dxr::RobustFrameSampler`, `robust_frame.h`; computed at
 * load by `ModelRenderer::getCenteringFrame`):
 *
 *     pivot        = (area-weighted median x, box centre y, area-weighted
 *                    median z): what the turntable turns about
 *     screen plane = area-weighted 75th-percentile depth toward the viewer
 *                    (~25% of the surface in front of the glass), applied as
 *                    a SEPARATE display-space slide of (pivot.z - p75) x fit x
 *                    zoom (model_orbit.h), so it holds under zoom and the
 *                    model still turns about its median
 *     extent       = per-axis 1st..99th percentile, but never so small that
 *                    the FULL box, measured about the pivot, exceeds 90% of
 *                    the viewport (dxr::RobustFitExtent)
 *
 * The swept width and the depth backstop then apply to those extents as
 * before. This mirrors the web SDK's `addModel` robust framing
 * (displayxr-web#327 / #330, `fitTo(center, extent, { screenPlaneZ })`) for
 * the same undocking reason: the page and this viewer must agree on the pivot
 * and the screen plane as well as the size.
 * `DXR_MODELVIEWER_CENTER=box` (Android: `setprop debug.dxr.mv.center box`)
 * restores the box centre and the full box extent; `--vh` still overrides the
 * scale under either rule.
 */
#pragma once

#include <cmath>
#include <cstring>

#include "auto_fit.h"  // dxr::AutoFitVHeight / dxr::kAutoFitDefaultFill

namespace modelviewer {

//! Backstop on total subject depth, in display heights. Mirrors the inline3d
//! SDK's `DEFAULT_DEPTH_LIMIT` (SceneViewer `depthLimit`). Rarely binds —
//! depth placement is a z decision, not a scale one.
inline constexpr float kFitDepthLimit = 4.0f;

//! The horizontal extent the fit frames against: the box's horizontal
//! DIAGONAL, which bounds the subject's silhouette at every yaw. Pass
//! `extentX` and `extentZ` (FULL sizes, not half-extents).
inline float
SweptHorizontalExtent(float extentX, float extentZ)
{
	if (!(extentX > 0.0f)) {
		return (extentZ > 0.0f) ? extentZ : 0.0f;
	}
	if (!(extentZ > 0.0f)) {
		return extentX;
	}
	return std::sqrt(extentX * extentX + extentZ * extentZ);
}

//! The same rule from the CACHED content half — the swept horizontal extent,
//! the height, and the depth — so a viewport change re-derives the base
//! without re-measuring the scene.
inline float
FitVHeightFromCached(float sweptW,
                     float extentH,
                     float extentD,
                     float viewportW,
                     float viewportH,
                     float fill = dxr::kAutoFitDefaultFill)
{
	float vh = dxr::AutoFitVHeight(sweptW, extentH, viewportW, viewportH, fill);
	if (!(vh > 0.0f)) {
		return 0.0f;
	}
	if (extentD > 0.0f) {
		const float vhForDepth = extentD / kFitDepthLimit;
		if (vhForDepth > vh) {
			vh = vhForDepth;
		}
	}
	return vh;
}

//! Rig vHeight framing an asset of world-space size extent[3] (FULL sizes)
//! inside a viewport of viewportW x viewportH, under the page's rule. Only the
//! viewport's ASPECT matters, so metres and pixels mix. Returns 0.0f when no
//! fit is computable, exactly like `dxr::AutoFitVHeight` — callers keep their
//! `if (!(vh > 1e-3f)) vh = fallback;` guard.
//!
//! `outSweptW` (optional) receives the swept horizontal extent, so a caller
//! that caches the CONTENT half of the fit for a viewport-change refit caches
//! the same number this used.
inline float
FitVHeight(const float extent[3],
           float viewportW,
           float viewportH,
           float *outSweptW = nullptr,
           float fill = dxr::kAutoFitDefaultFill)
{
	const float sweptW = SweptHorizontalExtent(extent[0], extent[2]);
	if (outSweptW != nullptr) {
		*outSweptW = sweptW;
	}
	return FitVHeightFromCached(sweptW, extent[1], extent[2], viewportW, viewportH, fill);
}

//! Load-time centring rule (#192). Robust is the default everywhere; Box is
//! the pre-#192 rule (box centre on the screen plane, full box extent).
enum class Centering
{
	Robust,
	Box,
};

//! Parse the DXR_MODELVIEWER_CENTER value (or the Android property): "box"
//! selects Box; anything else, including unset, is Robust.
inline Centering
CenteringFromString(const char *value)
{
	return (value != nullptr && std::strcmp(value, "box") == 0) ? Centering::Box : Centering::Robust;
}

//! Apply the centring rule to the box centre/extent a leg just read from
//! `getRobustSceneBounds`. Under Robust (and when the renderer has surface
//! stats) `center` becomes the PIVOT and `extent` the guarded robust extent
//! from `getCenteringFrame(fill, ...)`; `outScreenPlaneShift` receives
//! pivot.z - p75 (model units: the slide model_orbit.h applies so p75 lands on
//! the glass); `outDelta` receives (pivot - box centre): a leg that re-centres
//! on the animated skeleton every frame adds it to that anchor, so the robust
//! frame holds while the model moves. Otherwise (Box, or no stats) nothing
//! changes and both outputs are zero: the box centre on the glass, exactly the
//! pre-#192 rule. Returns a label for the log line.
template <typename Renderer>
inline const char *
ApplyCentering(const Renderer &renderer,
               Centering mode,
               float fill,
               float center[3],
               float extent[3],
               float outDelta[3],
               float *outScreenPlaneShift)
{
	outDelta[0] = outDelta[1] = outDelta[2] = 0.0f;
	*outScreenPlaneShift = 0.0f;
	if (mode == Centering::Box) {
		return "box (DXR_MODELVIEWER_CENTER=box)";
	}
	float pivot[3], rext[3], zdp = 0.0f;
	if (!renderer.getCenteringFrame(fill, pivot, rext, &zdp)) {
		return "box (no surface stats)";
	}
	for (int i = 0; i < 3; ++i) {
		outDelta[i] = pivot[i] - center[i];
		center[i] = pivot[i];
		extent[i] = rext[i];
	}
	*outScreenPlaneShift = pivot[2] - zdp;
	return "robust";
}

//! Which cap bound the fit, for logging. Mirrors the branch order above.
inline const char *
FitBoundBy(float sweptW, float extentH, float extentD, float aspect, float fill = dxr::kAutoFitDefaultFill)
{
	if (!(aspect > 0.0f)) {
		return "height (no viewport)";
	}
	const float vhH = extentH / fill;
	const float vhW = sweptW / (fill * aspect);
	const float vhD = extentD / kFitDepthLimit;
	if (vhD > vhH && vhD > vhW) {
		return "depth";
	}
	return (vhW > vhH) ? "swept-width" : "height";
}

} // namespace modelviewer

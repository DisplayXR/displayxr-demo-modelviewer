// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 XShape click-through for the transparent model overlay.
 *
 * The Linux analogue of the Windows leg's `dxr::ClickThroughRegion`
 * (displayxr-common/common/vk_clickthrough_region.h, runtime#833/#837), which
 * punches the window through to the desktop with SetWindowRgn. X11's
 * equivalent is an XShape **ShapeInput** region: clicks land on the model and
 * pass through the transparent surround to whatever is underneath.
 *
 * WHERE THE COVERAGE COMES FROM, AND WHY IT IS NOT A SECOND SCENE PASS.
 * The Windows leg derives the region from the frame's OWN rendered view — a
 * downscale blit of the swapchain tile's alpha — and this file does the same.
 * The sibling avatar demo instead re-renders the subject into scratch images
 * (`displayxr-demo-avatar/linux/clickthrough.cpp`), which it can afford
 * because ITS `ModelRenderer::renderEye` sizes its internal MSAA/depth targets
 * to the VIEWPORT with a grow-only policy. modelviewer's renderer does not: it
 * keys `ensureTargets()` on `imageWidth/imageHeight` (model_renderer.cpp:2920),
 * so alternating a 3840x2160 swapchain with a small silhouette raster would
 * destroy and recreate 8x MSAA colour+depth targets EVERY FRAME. Reading the
 * already-rendered alpha costs one blit instead, and it has the further virtue
 * of describing exactly what the user is looking at.
 *
 * That choice also sidesteps the whole class of bug the avatar had to fix in
 * its scratch-image path (avatar #99: the silhouette image needs
 * COLOR_ATTACHMENT usage on top of TRANSFER, and `renderEye` picks its entry
 * layout from the viewport origin) — there is no scratch render target here,
 * only transfer-only coverage images.
 *
 * Everything downstream of the coverage IS the avatar's design, because that
 * part is X11 and it is hardware-verified:
 *
 *   - the raster SCALES with the window (kTexelPxDefault window px per texel,
 *     capped), so precision does not degrade as the window grows;
 *   - the coverage is the UNION of the FIRST and LAST view, from the SAME
 *     frame. The weave shows every view at once with horizontal disparity that
 *     grows with window size, so a single-view mask visibly clips the other
 *     view's content;
 *   - the alpha threshold is LOW (8) and the result is DILATED by one texel.
 *     The error is asymmetric: an over-large region leaves a few transparent
 *     pixels clickable, an under-large one makes content unreachable;
 *   - the readback is PIPELINED behind a fence and HOST_CACHED. A synchronous
 *     vkQueueWaitIdle on a write-combined buffer is what made this pass look
 *     too expensive to run at frame rate (~127 ms vs ~0.86 ms, measured on the
 *     Windows leg);
 *   - rects are one per horizontal run, with identical consecutive rows folded
 *     into a band, so a 4K window does not hand the X server thousands of
 *     rectangles per frame.
 *
 * CHROME RECTS ARE UNIONED IN, NOT PUNCHED OUT. Anything the app draws that
 * is not the model — the client-side title bar — must be added to the region
 * or the shaped window makes it unclickable. (That is the avatar's
 * speech-bubble bug: an un-unioned band is invisible to the pointer.) Chrome
 * rects are already in client px and are appended unscaled.
 *
 * REACHABILITY SAFETY NET. With the title bar unioned in the window is always
 * grabbable, but the shape is also applied when decorations are off, and a
 * silhouette that shrinks to almost nothing (model panned off screen, nothing
 * loaded) would leave a window that cannot be clicked or focused — you could
 * no longer press Ctrl+T to get it back. Below kMinCoveredPx of covered
 * window area the shape is therefore DROPPED rather than applied, and the
 * fall back is logged once per transition.
 *
 * No-op when the window / Display is null (hosted-NULL fallback), and reports
 * once when the server has no XShape extension instead of silently doing
 * nothing. Requires libXext.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <cstdint>

//! Everything one click-through update needs.
struct ClickthroughParams {
	VkDevice dev = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;

	//! The swapchain atlas image the frame's views were just rendered into.
	//! Must be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL on entry; it is
	//! handed back in the same layout, so this may be called any time between
	//! the last renderEye() and xrReleaseSwapchainImage().
	VkImage viewImage = VK_NULL_HANDLE;
	VkFormat viewFormat = VK_FORMAT_UNDEFINED;
	//! Per-view tile size in atlas px (the mode's renderW x renderH).
	uint32_t tileW = 0, tileH = 0;
	//! Atlas offset of the FIRST and LAST view's tile. Equal when mono.
	uint32_t firstTileX = 0, firstTileY = 0;
	uint32_t lastTileX = 0, lastTileY = 0;
	bool twoViews = false;

	Display *dpy = nullptr;
	//! The TOP-LEVEL window: the one the WM and XWayland hit-test, so the one
	//! that carries the input shape. When the content is a child window (the
	//! client-side-decorations layout) its own input region is left at the
	//! default full rect — the server never descends into a child at a point
	//! outside the PARENT's input shape, so shaping the parent alone gates both.
	Window win = 0;
	//! Live CONTENT size in px — the area the view tiles map onto (the bound
	//! content child's XGetGeometry, not a cached create size).
	uint32_t winW = 0;
	uint32_t winH = 0;
	//! Where the content sits inside @ref win (the header bar's height when
	//! one is shown, else 0). Silhouette rects are offset by it; chrome rects
	//! are already in top-level px.
	int32_t contentOffsetX = 0;
	int32_t contentOffsetY = 0;

	//! Ctrl+T state. An opaque frame covers every pixel of the window, so the
	//! whole window must stay interactive.
	bool transparentBg = false;
	//! Decorated windows (DXR_X11_WM_DECORATIONS=1) drop the shape entirely:
	//! the WM frame needs the whole window for move/resize.
	bool decorated = false;

	//! App-drawn chrome in TOP-LEVEL px (the client-side title bar), unioned into
	//! the region so it stays clickable over a punched-through background.
	const XRectangle *chrome = nullptr;
	uint32_t chromeCount = 0;
};

//! Read back the frame's silhouette and set the window's XShape input region
//! from it. Call once per frame, after the eye render, before releasing the
//! swapchain image.
void ClickthroughUpdate(const ClickthroughParams &p);

//! Free the coverage images / readback buffer / command pool / fence. Device
//! must be idle.
void ClickthroughDestroy(VkDevice dev);

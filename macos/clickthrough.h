// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Click-through for the transparent model overlay (macOS).
 *
 * The macOS analogue of linux/clickthrough.{h,cpp} and the Windows leg's
 * `dxr::ClickThroughRegion`. The COVERAGE half is the same recipe, byte for
 * byte in behaviour: a downscale blit of the frame's FIRST and LAST view
 * tiles out of the swapchain atlas (the union, from the same frame, because
 * the weave shows every view at once), fence-pipelined into a HOST_CACHED
 * buffer and consumed one frame later (never a stall), alpha > 8, dilated by
 * one texel. Same env levers as Windows and Linux:
 *   DXR_CLICKTHROUGH_TEXEL_PX  window px per coverage texel (default 4)
 *   DXR_CLICKTHROUGH_DILATE    dilation radius in texels    (default 1)
 *   DXR_CLICKTHROUGH_ALPHA     alpha threshold 1..255       (default 8)
 *
 * WHAT IS DIFFERENT ON macOS: Cocoa has no input-region API (no SetWindowRgn,
 * no XShape ShapeInput). The only per-window switch is
 * NSWindow.ignoresMouseEvents, which is all-or-nothing. So this module does
 * not build a region at all: it publishes the coverage raster and answers
 * "is there content under this window point?", and main.mm drives
 * ignoresMouseEvents from that answer for the CURRENT cursor position. That
 * has to come from a global cursor poll ([NSEvent mouseLocation] each frame),
 * not from window events: once ignoresMouseEvents is YES the window receives
 * no mouse-moved events at all, so it could never turn itself back on.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

//! Everything one coverage update needs.
struct MacClickthroughParams {
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

	//! Content-view size in drawable px (sizes the coverage raster).
	uint32_t winW = 0, winH = 0;

	//! Read back this frame. False (opaque mode, no probe) drops the coverage.
	bool active = false;
};

//! Raw (pre-threshold) alpha of the last consumed readback — what the app
//! actually handed the runtime in the sampled tiles. Diagnostic only.
struct MacAlphaStats {
	bool valid = false;
	uint32_t texels = 0;
	uint32_t zero = 0;   //!< texels with alpha == 0
	uint32_t full = 0;   //!< texels with alpha == 255
	uint8_t minAlpha = 0;
	uint8_t maxAlpha = 0;
};

//! Consume the previous frame's readback and kick this frame's. Call once per
//! frame, after the eye render, before releasing the swapchain image.
void MacClickthroughUpdate(const MacClickthroughParams &p);

//! Hit-test a point in normalised content coordinates (u right, v DOWN, both
//! in [0,1)). Returns false when no coverage is available yet; otherwise
//! writes whether content (dilated) is under the point.
bool MacClickthroughHit(float u, float v, bool *outHit);

//! Covered window area in px for a winW x winH content rect (0 = no coverage).
uint64_t MacClickthroughCoveredPx(uint32_t winW, uint32_t winH);

//! Raw alpha statistics of the last readback; false when none yet.
bool MacClickthroughAlphaStats(MacAlphaStats *out);

//! Free the coverage images / readback buffer / command pool / fence.
void MacClickthroughDestroy(VkDevice dev);

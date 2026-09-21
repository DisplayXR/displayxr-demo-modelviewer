// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Client-side decorations (a GNOME-style header bar) for a weaving X11
 *         window whose drag the app must own.
 *
 * WHY CLIENT-SIDE, AND WHY NOT JUST "UNDECORATED". A windowed 3D app has to
 * keep the woven interlace phase invariant while it moves, so every step of a
 * window drag must go through the display processor's lattice snap
 * (xrWeaveSnapWindowRectDXR). What that requires is that the APP owns the
 * drag — not that the window has no title bar:
 *
 *   - Windows keeps a perfectly ordinary title bar and still snaps, because
 *     during the OS modal move loop it sends a DPI-aware app
 *     WM_WINDOWPOSCHANGING, PROPOSING the next position, and the app (the DP's
 *     hook) may rewrite it before it lands.
 *   - X11 has no equivalent. A server-side (WM-drawn) title bar belongs to the
 *     window manager's frame, the drag runs inside mutter's grab loop, and the
 *     client learns the result only afterwards via ConfigureNotify — it can
 *     only correct after the fact, fighting the WM on every step.
 *   - Wayland is strictly worse: no client position protocol at all.
 *
 * So the title bar is drawn by the app: the drag is ours from the first pixel
 * and runs through the same snapped path as the right-button drag. This is
 * also idiomatic — GNOME is CSD-native and offers no server-side decorations
 * on Wayland at all. Do NOT "simplify" this back to WM decorations;
 * DXR_X11_WM_DECORATIONS=1 is the deliberate, unsnapped escape hatch.
 *
 * GEOMETRY — THE BAR IS OUTSIDE THE WINDOW THE RUNTIME SEES. The runtime
 * derives the window-relative Kooima projection, the canvas and the weave's
 * present origin from the geometry of the window bound through
 * XR_DXR_xlib_window_binding (comp_vk_native_compositor.c,
 * get_window_metrics: xcb_get_geometry for the size, xcb_translate_coordinates
 * to root for the origin; vk_update_present_origin feeds the DP from the same
 * call). So the app builds a PARENT/CHILD pair, the way Windows separates the
 * non-client frame from the client area:
 *
 *   top-level  = bar + content. Owns the bar, receives every input event, is
 *                what the WM stacks/iconifies/fullscreens and what the drag
 *                moves, and carries the click-through XShape.
 *   child      = exactly the content rect below the bar. ITS XID is the one
 *                bound, so the runtime's rect IS the visible 3D area — no strip
 *                of "screen" hidden under the bar, and no runtime change.
 *
 * The bar is not woven, so it is plain 2D: this module rasterizes it on the
 * CPU and XPutImage()s it into the top-level. It is never in the atlas, never
 * in an atlas capture, and never resampled with downscaled view tiles.
 *
 * LIFTABLE BY DESIGN. This file knows nothing about the demo, Vulkan or
 * OpenXR: it takes a Display (for DPI) and paints into a Window.
 * Window moving is NOT done here — hitTest() says "this press is a drag" and
 * the caller routes it into its own snapped drag, which is the one piece that
 * must stay app-owned. That split is what should let it move into the shared
 * test_apps/common/dxr_linux_window helper later.
 */

#pragma once

#include <X11/Xlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dxr_csd {

//! What a point in the bar is.
enum class Hit { Outside, Drag, Minimize, Close };

class TitleBar {
public:
	//! Read the desktop scale (Xft.dpi, or DXR_CSD_SCALE) and load the title
	//! font. Call once, BEFORE creating the windows: the top-level's height is
	//! content + height(). Never fails: without a font the bar has no title.
	void configure(Display *dpy);
	//! Free the GC. The windows themselves belong to the caller.
	void release(Display *dpy);

	//! Bar height in client (device) px — 46 logical px x the desktop scale,
	//! rounded up to even.
	uint32_t height() const { return height_; }
	float scale() const { return scale_; }

	//! The bar's rect in TOP-LEVEL px, for the click-through input region.
	XRectangle rect(uint32_t winW) const;

	//! Classify a top-level-px point. Outside = not in the bar. (Not `None`:
	//! <X11/X.h> #defines None, which would mangle the enumerator.)
	Hit hitTest(int x, int y, uint32_t winW) const;

	// ── Interaction state. Changes mark the bar dirty; paint when dirty().
	void setHover(Hit h);
	void setPressed(Hit h);
	Hit pressed() const { return pressed_; }
	void setFocused(bool f);
	void setTitle(const std::string &t) { if (t != title_) { title_ = t; dirty_ = true; } }
	//! Force a repaint (Expose: the server discarded our pixels).
	void invalidate() { dirty_ = true; }
	bool dirty() const { return dirty_; }

	//! Paint the bar into the top of @p win (the TOP-LEVEL), @p winW wide.
	//! @p visual must be @p win's visual; the pixels are packed through its
	//! channel masks, so ARGB and root visuals both work.
	void paint(Display *dpy, Window win, const Visual *visual, uint32_t winW);

private:
	bool rasterize(uint32_t w, const std::string &title);
	void loadFont();

	std::vector<uint8_t> pixels_;  //!< RGBA8, sRGB, rasterW_ x height_
	std::vector<uint32_t> ximage_; //!< pixels_ packed for the window visual
	uint32_t rasterW_ = 0;
	void *gc_ = nullptr;           //!< GC (opaque here to keep Xutil out of the header)
	Window gcWin_ = 0;

	std::vector<uint8_t> fontData_;
	bool fontOk_ = false;
	std::string title_;
	std::string rasterTitle_;
	bool dirty_ = true;

	float scale_ = 1.0f;
	uint32_t height_ = 46;
	Hit hover_ = Hit::Outside;
	Hit pressed_ = Hit::Outside;
	bool focused_ = true;
};

} // namespace dxr_csd

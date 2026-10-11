// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The viewer's adoption of the depth-aware cursor (XR_DXR_cursor_depth).
 *
 * The runtime DRAWS the cursor (ADR-046 Amendment 2): an arrow lifted just in
 * front of whatever pops out of the glass under the pointer, and nothing at
 * all over content on or behind the glass (the OS cursor stays the cursor
 * there). The app owns only the depth under the pointer. displayxr-common's
 * `dxr::CursorDepth` does the bookkeeping; this header is the part every leg
 * of THIS viewer shares on top of it:
 *
 *   - the depth source: `ModelRenderer::raycast` (per-cluster BVH over the
 *     loaded model, following its animated pose — model_raycast.h). The model
 *     is the only thing in the scene that can stick out of the glass: the
 *     skybox sits at infinity and every UI surface (HUD, toasts, buttons) is
 *     2D, so the model IS "everything that sticks out".
 *   - `DISPLAYXR_CURSOR_DEPTH=0` opts out (the extension is then not enabled
 *     at all, so the runtime does no cursor work), as in the runtime's cube apps.
 *   - `DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the pointer (canvas UV, origin
 *     top-left) for headless verification; the leg then ignores real motion.
 *   - a throttled cost line: the ray cast's wall time per frame.
 *
 * Per leg (see macos/main.mm, linux/main.cpp, windows/main.cpp):
 *
 *     // instance: enable XR_DXR_cursor_depth if present && CursorDepthWanted()
 *     modelviewer::CursorDepthAdoption cursorAdopt;
 *     cursorAdopt.setup(cursor, g_modelRenderer, specVersionOr0);
 *     cursor.setWindowCocoa(view);          // or setWindowWin32 / the manual path
 *     // per frame, around xrLocateViews:
 *     cursor.chain(locateInfo, viewState);
 *     xrLocateViews(...);
 *     cursor.afterLocate(views, activeViewCount);
 *     if (cursorAdopt.endFrame(cursor)) LOG_INFO("%s", cursorAdopt.summary());
 */

#pragma once

#include "cursor_depth.h"   // displayxr-common v2.29.0+
#include "model_renderer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace modelviewer {

//! False when DISPLAYXR_CURSOR_DEPTH=0: do not enable the extension.
inline bool CursorDepthWanted() {
    const char* e = std::getenv("DISPLAYXR_CURSOR_DEPTH");
    return !(e != nullptr && std::strcmp(e, "0") == 0);
}

class CursorDepthAdoption {
public:
    //! enable(specVersion) (0 = extension not enabled: the helper stays inert),
    //! the model ray cast, and the scripted pointer. The window adapter stays
    //! with the leg (it is the only platform-specific part).
    void setup(dxr::CursorDepth& cursor, const ModelRenderer& renderer, uint32_t specVersion) {
        cursor.enable(specVersion);
        const ModelRenderer* r = &renderer;
        Stats* st = &stats_;
        cursor.setRayCast([r, st](const dxr::CursorRay& ray) -> std::optional<float> {
            const auto t0 = std::chrono::steady_clock::now();
            const float o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
            const float d[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
            float t = 0.0f;
            const bool hit = r->raycast(o, d, t);
            st->frameUs += std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - t0).count();
            st->frameRays++;
            return hit ? std::optional<float>(t) : std::nullopt;
        });
        scripted_ = false;
        if (const char* uv = std::getenv("DISPLAYXR_CURSOR_DEPTH_UV")) {
            float u = 0.0f, v = 0.0f;
            if (std::sscanf(uv, "%f,%f", &u, &v) == 2) {
                cursor.setPointerUV(u, v);
                cursor.setWantPlacement(true);   // the placement goes into the summary
                scripted_ = true;
            }
        }
    }

    //! The pointer is scripted (DISPLAYXR_CURSOR_DEPTH_UV): ignore real motion.
    bool scripted() const { return scripted_; }

    //! Call once per frame after afterLocate(). True when a cost summary is
    //! ready (every 600 frames that ran the ray cast); read it with summary().
    bool endFrame(const dxr::CursorDepth& cursor) {
        if (stats_.frameRays > 0) {
            stats_.frames++;
            stats_.sumUs += stats_.frameUs;
            stats_.sumRays += stats_.frameRays;
            if (stats_.frameUs > stats_.maxUs) stats_.maxUs = stats_.frameUs;
            if (cursor.hint().hasContent) stats_.hitFrames++;
            if (cursor.drawState().willDraw) stats_.drawFrames++;
        }
        stats_.frameUs = 0.0;
        stats_.frameRays = 0;
        if (stats_.frames < kSummaryFrames) return false;
        std::snprintf(summary_, sizeof(summary_),
                      "cursor depth: ray cast %.1f us/frame mean, %.1f us max, %.1f rays/frame "
                      "over %u frames (hit %u, runtime drew %u)",
                      stats_.sumUs / stats_.frames, stats_.maxUs, (double)stats_.sumRays / stats_.frames,
                      stats_.frames, stats_.hitFrames, stats_.drawFrames);
        if (scripted_) {
            const XrCursorDepthPlacementDXR& pl = cursor.placement();
            const size_t n = std::strlen(summary_);
            std::snprintf(summary_ + n, sizeof(summary_) - n,
                          "; placement active=%d disparity=%.3f target=%.3f pos=(%.4f, %.4f, %.4f) h=%.4f",
                          (int)pl.isActive, pl.disparity, pl.targetDisparity, pl.pose.position.x,
                          pl.pose.position.y, pl.pose.position.z, pl.height);
        }
        stats_ = Stats{};
        return true;
    }
    const char* summary() const { return summary_; }

private:
    static constexpr uint32_t kSummaryFrames = 600;
    struct Stats {
        double frameUs = 0.0;
        uint32_t frameRays = 0;
        uint32_t frames = 0, hitFrames = 0, drawFrames = 0;
        uint64_t sumRays = 0;
        double sumUs = 0.0, maxUs = 0.0;
    };
    Stats stats_;
    bool scripted_ = false;
    char summary_[512] = {};
};

} // namespace modelviewer

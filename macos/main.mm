// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS Vulkan OpenXR glTF 2.0 PBR model viewer with external window binding
 *
 * Renders glTF 2.0 models on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk_macos with the cube/grid renderer replaced by
 * the model_common/ModelRenderer PBR pipeline.  Features a "Load…" button overlay.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_DXR_cocoa_window_binding)
 * - Mouse drag camera, WASD/QE movement, scroll zoom
 * - XR_DXR_display_info: Kooima projection, display metrics
 * - V key cycles rendering modes via xrRequestDisplayRenderingModeDXR
 * - 0-3 keys select rendering mode directly
 * - Ctrl+O or button click: NSOpenPanel to load .glb/.gltf models
 * - Ctrl+T / Cmd+T: transparent background (desktop shows through, clicks pass
 *   through the transparent surround, window floats above others) — #181.
 *   MODEL_TRANSPARENT=1 or --transparent starts transparent; MODEL_TRANSPARENT=0
 *   turns the capability off for the session. (Plain T = eye-tracking mode.)
 * - Tab: toggle HUD overlay, Space: reset camera, ESC: quit
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_cocoa_window_binding.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_atlas_capture.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_mcp_tools.h>
#include <openxr/XR_DXR_depth_budget.h>  // advisory rear depth budget (ADR-040)

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <array>
#include <cstdlib>   // getenv (deterministic-capture switch)
#include <chrono>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

#include "view_params.h"
#include "dxr_view_config.h" // displayxr-common: DxrSelectViewConfigType (#1486/#1500) + DxrAliasInactiveViews (ADR-041)
#include "mode_switch.h" // dxr::ModeSwitch — smooth 2D<->3D disparity ramp (inline on macOS)
#include "display3d_view.h"
#include "camera3d_view.h"
#include "projection_depth.h"
#include "model_renderer.h"
#include "atlas_capture.h"
#include "auto_fit.h"    // dxr::AutoFitVHeight — shared width-aware load-time framing
#include "model_fit.h"   // modelviewer::FitVHeight — the page-parity framing rule
#include "model_cursor_depth.h" // depth-aware cursor (XR_DXR_cursor_depth): runtime-drawn, model ray cast
#include "hand_tracking.h"      // XR_EXT_hand_tracking consumer + display->world joint map
#include "hand_gestures.h"      // pinch gestures -> rig deltas (pure math, unit-tested)
#include "hand_renderer.h"      // translucent hand mesh via ModelRenderer's overlay hook
#include "model_orbit.h"        // turntable: orbit state -> model transform, rig stays fixed
#include "launch_args.h"        // dxr::ParseLaunchArgs — the shared launch contract (--transparent, path, --vh)
// XR_DXR_depth_budget (ADR-040) — the Windows leg's #116 port, as on Linux.
// clip_policy.h is the ONE place the runtime's advisory rear budget becomes
// near/far/clipFar; content_bounds.h (v2) and content_mask.h (v3) describe
// WHERE the content is so the runtime measures the background only behind it.
#include "clip_policy.h"
#include "content_bounds.h"
#include "content_mask.h"
#include "clickthrough.h"       // alpha-coverage readback -> ignoresMouseEvents (Ctrl/Cmd+T)
#include <algorithm>
#include <cstdarg>

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Input state
// ============================================================================

struct InputState {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;   // rig roll about the view axis; only the two-hand pinch twist sets it
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    ViewParams viewParams;
    bool hudVisible = false;  // Hidden by default; toggle with Tab.
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
    bool eyeTrackingModeToggleRequested = false;
    bool transparentBgToggleRequested = false;   // Ctrl+T / Cmd+T
    bool loadRequested = false;
    bool teleportRequested = false;
    float teleportMouseX = 0.0f, teleportMouseY = 0.0f; // logical points

    // Smooth display-pose transition (double-click focus)
    bool transitioning = false;
    XrPosef transitionFrom = {{0,0,0,1}, {0,0,0}};
    XrPosef transitionTo   = {{0,0,0,1}, {0,0,0}};
    float transitionT = 0.0f;
    float transitionDuration = 0.45f;

    // Auto-orbit (turntable) mode
    bool animateEnabled = true;  // Always on; auto-orbit kicks in after 10 s idle.
    double lastInputTimeSec = 0.0;
    bool animationActive = false;
    bool animateToggleRequested = false;     // set by UI button

    // glTF clip playback (Phase 4): one-shot, set by 'N' / 'K', cleared by the
    // render loop after it drives ModelRenderer::cycleAnimation()/togglePaused().
    bool cycleClipRequested = false;         // 'N' → next animation clip
    bool playPauseRequested = false;         // 'K' → toggle play/pause

    // Drag-and-drop / pending file load
    std::string pendingLoadPath;

    // 'I' key: capture the rendered atlas region (cols × rows × renderW × renderH)
    // of the swapchain to <scene>_<cols>x<rows>.png in the working directory.
    // Skipped for 1×1 (mono) layouts. Useful for grabbing the SBS image
    // intended for shell launcher icons / 3D thumbnails.
    bool captureAtlasRequested = false;

    // Unified rendering mode (V key cycles, 0-8 keys select directly)
    uint32_t currentRenderingMode = 1;   // Default: mode 1 (first 3D mode)
    uint32_t renderingModeCount = 0;     // Set from xrEnumerateDisplayRenderingModesDXR
    bool renderingModeChangeRequested = false;
};

// Fallback virtual-display height in meters when no scene is loaded
// (or auto-fit fails). On scene load we replace this with a robust
// percentile-based extent — see ApplyAutoFitForLoadedScene().
static constexpr float kDefaultVirtualDisplayHeightM = 1.5f;

// Load-time framing is the page's rule (common/model_fit.h), which composes
// the shared width-aware rule from displayxr-common (dxr::AutoFitVHeight,
// default dxr::kAutoFitDefaultFill = 80%): the model spans at most 80% of the
// viewport in BOTH axes, with the SWEPT horizontal extent hypot(W, D) as the
// width so a deep asset still fits once the turntable turns it, plus a depth
// backstop. There is no separate vertical comfort multiplier — the fill
// fraction IS the headroom. One rule across every leg: see common/model_fit.h
// for why it has to equal the web SDK's SceneViewer.fitTo.

// Cached auto-fit result for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kDefaultVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static bool  g_fitValid     = false;
static float g_displayHeightM = 0.0f;   // physical panel height (for ApplySceneCamera)
// Full-model extents behind g_fitCenter, for the XR_DXR_depth_budget v2
// content-bounds ROI (Windows: g_fitExtentW/H/D). W is the SWEPT horizontal
// extent hypot(W, D), as on Windows/Linux — conservative under the turntable.
static float g_fitExtentW = 0.0f;
static float g_fitExtentH = 0.0f;
static float g_fitExtentD = 0.0f;

// XR_DXR_depth_budget v3: the retained content-occupancy mask, reduced from the
// renderer's UNCLIPPED coverage pass once per rendered frame and re-chained on
// frames that skip rendering. Must outlive xrEndFrame (XrContentMaskDXR::cells
// is a pointer), hence file scope. Same as linux/main.cpp.
static std::vector<uint8_t> g_contentMaskCells;
static uint32_t g_contentMaskW = 0, g_contentMaskH = 0;
static bool g_contentMaskChaining = false;  // one-time log on start/stop, never per frame

// --vh=<metres>: pins the virtual display height over the auto-fit result.
static float g_vhOverride = 0.0f;

// Transparent background (Ctrl+T / Cmd+T, #181) — the Windows/Linux legs'
// g_transparentBg. The runtime fixes the layer's opacity (CAMetalLayer.opaque,
// the swapchain compositeAlpha) at xrCreateSession, so the session ALWAYS asks
// for transparentBackgroundEnabled = XR_TRUE (g_transparentCapable) and the
// toggle only changes what the app DRAWS: an alpha-0 clear and no skybox. With
// alpha 1 everywhere an opaque frame is indistinguishable from an opaque
// session (and a runtime that probes the atlas alpha per frame sees nothing to
// composite). MODEL_TRANSPARENT=0 turns the capability off for the session —
// opaque layer, XR_FALSE — and Ctrl+T then refuses with a log line instead of
// drawing black.
static bool g_transparentCapable = true;
static bool g_transparentBg = false;
// Click-through state (driven by UpdateClickThrough each frame).
static bool g_ignoringMouse = false;
// Last click-through decision inputs, for the alpha-probe log line.
static struct { bool inContent, haveCov, hit, overChrome, reachable; float u, v; } g_ctLast = {};
// DXR_MODELVIEWER_ALPHA_PROBE=1: run the click-through readback in opaque mode
// too and log the raw atlas alpha the app submits (min/max/zero/full) every
// ~2 s — the pixel-level check that opaque frames are alpha 255 everywhere and
// transparent ones alpha 0 around the model.
static bool g_alphaProbe = false;
// Not under a workspace controller this frame (IsStandaloneSession, refreshed
// in the render loop). Gates the transparent half of the auto-orbit hold and
// the no-budget ZDP clip fallback, exactly as on Windows.
static bool g_standaloneSession = true;

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;
static InputState g_input;
// Smooth 2D<->3D disparity ramp, driven inline (macOS app-owned mode model).
// g_msLastMode tracks the runtime-confirmed mode so the sequencer knows the
// ramp direction; it is synced on fire and on the RenderingModeChanged event.
static dxr::ModeSwitch g_modeSwitch;
static bool g_modeSwitchConfigured = false;
static uint32_t g_msLastMode = 1; // matches g_input.currentRenderingMode default
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f;

typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

static bool g_fullscreen = false;
// Fullscreen saves the CONTENT rect (not the frame): the style it returns to
// depends on g_transparentBg at exit time (titled vs borderless), and only the
// content rect is the same in both.
static NSRect g_savedContentRect = {};

// The normal (opaque-mode) window style. Transparent mode (Ctrl+T) swaps the
// window to NSWindowStyleMaskBorderless — the Windows leg's WS_POPUP, the
// Linux leg's "no header bar while transparent" — so no title bar or frame
// shows around the floating model. The content rect is preserved across the
// swap; the window, the view and its CAMetalLayer are never recreated (the
// runtime holds the view via XR_DXR_cocoa_window_binding).
static const NSUInteger kTitledWindowStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                             NSWindowStyleMaskResizable |
                                             NSWindowStyleMaskMiniaturizable;
// RMB window drag (transparent / borderless mode only — dxr::RmbWindowDrag,
// the Windows borderless-overlay + Unity desktop-avatar convention). State in
// AppKit global points (bottom-up).
static bool g_rmbDragging = false;
static NSPoint g_rmbMouseStart = {};
static NSPoint g_rmbFrameStart = {};

// Model-viewer state
static ModelRenderer g_modelRenderer;

// Hand tracking (XR_EXT_hand_tracking): joints -> pinch gestures -> rig
// deltas, plus a translucent hand mesh drawn through the renderer's overlay
// hook. All of it is inert when the runtime has no hand-tracking provider.
static modelviewer::HandTracking          g_handTracking;
static modelviewer::HandGestureController g_handGestures;
static modelviewer::HandRenderer          g_handRenderer;
static bool   g_handsEverSeen = false;   // first detection -> on-screen hint
static double g_handHintUntilSec = 0.0;  // hint visible while NowSec() < this
static const float kHandColorLeft[3]  = {0.30f, 0.75f, 1.00f};   // cyan
static const float kHandColorRight[3] = {1.00f, 0.62f, 0.28f};   // amber
static std::string g_loadedFileName;

static double g_avgFrameTime = 0.0;
static uint64_t g_frameCount = 0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_renderW = 0, g_renderH = 0;
static uint32_t g_windowW = 0, g_windowH = 0;

// Atlas capture helpers (filename / Pictures dir / flash overlay / Vulkan
// readback) live in test_apps/common/atlas_capture* — see dxr_capture::*.

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];
    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

//! q = a * b (apply b, then a).
static XrQuaternionf quat_mul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

//! Rig orientation = yaw/pitch (mouse, WASD) then a roll about the rig's own
//! view axis (two-hand pinch twist). Roll last so it never leaks into the
//! yaw/pitch the transitions and HUD read back.
static void quat_from_yaw_pitch_roll(float yaw, float pitch, float roll, XrQuaternionf* out) {
    XrQuaternionf yp;
    quat_from_yaw_pitch(yaw, pitch, &yp);
    XrQuaternionf r = {0.0f, 0.0f, sinf(roll / 2.0f), cosf(roll / 2.0f)};
    *out = quat_mul(yp, r);
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye distance for the ZDP-anchored clip (#396 W7 consume path):
// z of (rigPose^-1 * eyeWorld). Equals the old eye_display.z so near = ez - vH /
// far = ez + far_offset stays identical. fov is clip-independent — this is all
// the app keeps of the old per-eye Kooima math.
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// ============================================================================
// Forward decls for top-bar UI helpers (defined after CreateMacOSWindow)
// ============================================================================

struct AppXrSession;
static void UpdateTopBarButtonTitles(AppXrSession& xr);
static void ApplyAutoFitForLoadedScene();
static void UpdateMcpAnimationTools();

// ============================================================================
// Input timestamp helper
// ============================================================================

static double NowSec(void) {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

static void MarkUserInput(InputState& input) {
    input.lastInputTimeSec = NowSec();
    input.animationActive = false;
}

// Extract yaw/pitch from a quaternion (XYZ order, matches quat_from_yaw_pitch).
// Only used after a smooth pose transition completes so subsequent drag rotation
// feels natural. Ambiguous near the poles — acceptable for this demo.
static void yaw_pitch_from_quat(XrQuaternionf q, float* yaw, float* pitch) {
    // Forward vector in local is (0, 0, -1); rotate it by q to get world forward.
    float fx = 2.0f * (q.x * q.z + q.y * q.w) * -1.0f + 0.0f;
    // Reuse the cross-product form for clarity.
    float vx = 0, vy = 0, vz = -1.0f;
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    float fwdX = vx + q.w * tx + (q.y * tz - q.z * ty);
    float fwdY = vy + q.w * ty + (q.z * tx - q.x * tz);
    float fwdZ = vz + q.w * tz + (q.x * ty - q.y * tx);
    (void)fx;
    // quat_from_yaw_pitch(y,p) rotates (0,0,-1) to (-cos(p)sin(y), sin(p), -cos(p)cos(y)).
    // Inverting:  p = asin(fwdY),  y = atan2(-fwdX, -fwdZ).
    *yaw = atan2f(-fwdX, -fwdZ);
    float clampedY = fwdY;
    if (clampedY > 1.0f) clampedY = 1.0f;
    if (clampedY < -1.0f) clampedY = -1.0f;
    *pitch = asinf(clampedY);
}

// ============================================================================
// Camera movement (ported from common/input_handler)
// ============================================================================

static void UpdateCameraMovement(InputState& input, float dt, float displayHeightM) {
    if (input.resetViewRequested) {
        input.pitch = 0;
        input.roll = 0;
        input.viewParams = ViewParams();
        if (g_fitValid) {
            input.cameraPosX = g_fitCenter[0];
            input.cameraPosY = g_fitCenter[1];
            input.cameraPosZ = g_fitCenter[2];
            input.yaw = g_fitYaw;
            input.viewParams.virtualDisplayHeight = g_fitVHeight;
        } else {
            input.cameraPosX = input.cameraPosY = input.cameraPosZ = 0;
            input.yaw = 0;
            input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
        }
        input.resetViewRequested = false;
        input.transitioning = false;
        // Auto-orbit always on; resetting just resets the idle timer below.
        input.animationActive = false;
        input.lastInputTimeSec = NowSec();
        return;
    }

    // Smooth pose transition (double-click focus). Overrides WASD while active.
    if (input.transitioning) {
        input.transitionT += dt;
        float u = input.transitionT / input.transitionDuration;
        if (u >= 1.0f) u = 1.0f;
        // Ease-out cubic
        float invU = 1.0f - u;
        float eased = 1.0f - invU * invU * invU;
        XrPosef cur;
        display3d_pose_slerp(&input.transitionFrom, &input.transitionTo, eased, &cur);
        input.cameraPosX = cur.position.x;
        input.cameraPosY = cur.position.y;
        input.cameraPosZ = cur.position.z;
        float yaw, pitch;
        yaw_pitch_from_quat(cur.orientation, &yaw, &pitch);
        input.yaw = yaw;
        input.pitch = pitch;
        if (u >= 1.0f) input.transitioning = false;
        return;
    }

    float speed = 0.15f;
    if (displayHeightM > 0.0f) speed *= displayHeightM / 0.1f;

    // Build orientation quaternion and derive basis vectors
    XrQuaternionf ori;
    quat_from_yaw_pitch(input.yaw, input.pitch, &ori);

    float fwdX, fwdY, fwdZ, rtX, rtY, rtZ, upX, upY, upZ;
    quat_rotate_vec3(ori, 0, 0, -1, &fwdX, &fwdY, &fwdZ);
    quat_rotate_vec3(ori, 1, 0, 0, &rtX, &rtY, &rtZ);
    quat_rotate_vec3(ori, 0, 1, 0, &upX, &upY, &upZ);

    float d = speed * dt;
    if (input.keyW) { input.cameraPosX += fwdX*d; input.cameraPosY += fwdY*d; input.cameraPosZ += fwdZ*d; }
    if (input.keyS) { input.cameraPosX -= fwdX*d; input.cameraPosY -= fwdY*d; input.cameraPosZ -= fwdZ*d; }
    if (input.keyD) { input.cameraPosX += rtX*d;  input.cameraPosY += rtY*d;  input.cameraPosZ += rtZ*d; }
    if (input.keyA) { input.cameraPosX -= rtX*d;  input.cameraPosY -= rtY*d;  input.cameraPosZ -= rtZ*d; }
    if (input.keyE) { input.cameraPosX += upX*d;  input.cameraPosY += upY*d;  input.cameraPosZ += upZ*d; }
    if (input.keyQ) { input.cameraPosX -= upX*d;  input.cameraPosY -= upY*d;  input.cameraPosZ -= upZ*d; }

    // Auto-orbit: if enabled and user has been idle > 10s, slowly yaw the display.
    // Held while a clip plays — the asset already carries its own motion, and
    // orbiting on top of it double-animates the scene and fights whatever
    // framing the clip was authored for. Holding the idle clock at "now" (rather
    // than just skipping the yaw) restarts the 10 s countdown when playback
    // pauses, so the turntable doesn't snap on the instant the user hits K.
    // Also held while the background is TRANSPARENT in a standalone session
    // (the Windows leg's AutoOrbitSuppressed): the model is a floating object
    // on the desktop, and one that spins by itself reads as a glitch. Under a
    // workspace controller the app is a framed tile, so it keeps orbiting.
    if ((g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused()) ||
        (g_transparentBg && g_standaloneSession)) {
        input.animationActive = false;
        input.lastInputTimeSec = NowSec();
    } else {
        double idleFor = NowSec() - input.lastInputTimeSec;
        input.animationActive = (input.animateEnabled && idleFor > 10.0);
        if (input.animationActive) {
            float rate = 6.2831853f / 20.0f; // one revolution per 20 seconds
            input.yaw += rate * dt;
        }
    }
}

// ============================================================================
// HUD overlay (simple NSView with CoreText)
// ============================================================================

@interface HudOverlayView : NSView
@property (nonatomic, strong) NSString *hudText;
@end

@implementation HudOverlayView

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    if (!_hudText) return;
    // No backdrop fill — the enclosing NSVisualEffectView provides frosted
    // vibrancy that auto-adapts to whatever is behind the window.
    NSDictionary *attrs = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor labelColor]
    };
    NSRect textRect = NSInsetRect(self.bounds, 10, 8);
    [_hudText drawInRect:textRect withAttributes:attrs];
}

@end

static HudOverlayView   *g_hudView = nil;      // text view
static NSVisualEffectView *g_hudBackdrop = nil;  // frosted wrapper sized to hudView

// ============================================================================
// Top-bar overlay (Open / Auto-Orbit / Mode buttons)
// ============================================================================

static NSView   *g_topBar = nil;
static NSButton *g_openButton = nil;
static NSButton *g_modeButton = nil;
static NSButton *g_animButton = nil;   // right-justified clip button; hidden w/o clips
static NSView   *g_animButtonBackdrop = nil;

// Translucent dark background view used behind the top bar.
@interface TopBarBackdropView : NSView
@end
@implementation TopBarBackdropView
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)r {
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.55] set];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.08] set];
    NSRect hr = NSMakeRect(0, 0, self.bounds.size.width, 1);
    NSRectFill(hr);
}
@end

static void OpenLoadDialog() {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Load 3D Model"];
        [panel setMessage:@"Select a 3D model (glTF / STL / OBJ / FBX / USD)"];

        if (@available(macOS 11.0, *)) {
            NSMutableArray<UTType *> *types = [NSMutableArray array];
            for (NSString *ext in @[@"glb", @"gltf", @"stl", @"obj", @"fbx",
                                    @"usdz", @"usd", @"usda", @"usdc"]) {
                UTType *t = [UTType typeWithFilenameExtension:ext];
                if (t) [types addObject:t];
            }
            if (types.count > 0) {
                [panel setAllowedContentTypes:types];
            }
        }

        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) {
                const char *path = [[url path] UTF8String];
                std::string pathStr(path);
                if (model_validate_file(pathStr)) {
                    LOG_INFO("Loading model: %s", path);
                    if (g_modelRenderer.loadModel(path)) {
                        g_loadedFileName = model_basename(pathStr);
                        LOG_INFO("Model loaded: %s (%s)", g_loadedFileName.c_str(),
                            model_filesize_str(pathStr).c_str());
                        ApplyAutoFitForLoadedScene();
                    } else {
                        LOG_ERROR("Failed to load model: %s", path);
                        NSAlert *alert = [[NSAlert alloc] init];
                        [alert setMessageText:@"Failed to load model file"];
                        [alert setInformativeText:@"The file may be corrupt or unsupported."];
                        [alert runModal];
                    }
                }
            }
        }
    }
}

// ============================================================================
// macOS Window + Metal Layer
// ============================================================================

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n { (void)n; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)windowWillClose:(NSNotification *)n { (void)n; g_running = false; }
@end

// Borderless NSWindows refuse key/main status by default, which would kill
// keyboard input (Ctrl+T could not turn transparency back off). This window
// stays eligible in every style.
@interface ModelViewerWindow : NSWindow
@end

@implementation ModelViewerWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@interface MetalView : NSView
@end

@implementation MetalView
// The layer must be 1:1 with the window's backing store: a CAMetalLayer left
// at contentsScale 1 on a 2x display is RESAMPLED by WindowServer, which turns
// a lenticular weave (computed per physical subpixel) into colour bands. A
// layer the app creates is not managed by AppKit, so keep contentsScale =
// backingScaleFactor here and on every backing change.
- (CALayer*)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    CGFloat scale = self.window != nil ? self.window.backingScaleFactor
                                       : [NSScreen mainScreen].backingScaleFactor;
    layer.contentsScale = scale > 0 ? scale : 1.0;
    // A transparent-capable session needs a non-opaque layer from the start
    // (the runtime enforces it again at xrCreateSession). Opaque frames are
    // alpha 1 everywhere, so this costs nothing visible.
    layer.opaque = g_transparentCapable ? NO : YES;
    return layer;
}
- (void)syncLayerScale {
    if (self.window != nil && self.layer != nil && self.window.backingScaleFactor > 0) {
        self.layer.contentsScale = self.window.backingScaleFactor;
    }
}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self syncLayerScale];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self syncLayerScale];
}
- (BOOL)wantsLayer { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent *)event {
    MarkUserInput(g_input);
    if ([event clickCount] >= 2) {
        NSPoint loc = [event locationInWindow];
        g_input.teleportRequested = true;
        g_input.teleportMouseX = (float)loc.x;
        g_input.teleportMouseY = (float)loc.y;
    }
    [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent *)event {
    MarkUserInput(g_input);
    g_input.yaw -= (float)[event deltaX] * 0.005f;
    // pitch -= deltaY: since W7 (#396) model_renderer flips Vulkan-Y at the
    // RASTER stage (negative VkViewport.height), NOT by reflecting the view
    // matrix. A view-stage reflection would invert rotational handedness
    // (needing += here); the raster flip does not, so we use the same -=
    // convention as the cube_handle apps.
    g_input.pitch -= (float)[event deltaY] * 0.005f;
    float maxPitch = 1.5f;
    if (g_input.pitch > maxPitch) g_input.pitch = maxPitch;
    if (g_input.pitch < -maxPitch) g_input.pitch = -maxPitch;
}

// RMB drag moves the window while it is borderless (transparent mode), the
// Windows leg's dxr::RmbWindowDrag: with no title bar there is no other way
// to move it. Opaque mode leaves RMB alone (the title bar moves the window),
// exactly as on Windows. Gating to "the model, not the surround" is free:
// UpdateClickThrough makes the window ignore the mouse over the transparent
// surround, so a right-press there goes to whatever is underneath.
//
// This is a FALLBACK and must never fight the runtime. When the runtime owns
// window placement (ADR-050 comp_metal_placement — active whenever the session
// lands on comp_metal, including this Vulkan app routed VK -> comp_vk_client ->
// comp_metal for a Metal-only DP such as Leia), it makes the window
// movable = NO and takes a borderless window's right-button drag itself
// (phase-snapped, atomic with the present); its local event monitor swallows
// those events, so they never reach this view. If one does get here anyway
// (an older runtime that owns placement but not the RMB drag), !isMovable
// still says the runtime owns placement, and an app setFrameOrigin would move
// the window behind its back (stale woven frame, no snap) — so the fallback
// only runs while the window is movable, i.e. no runtime placement (an
// in-process vk_native session, where the present reads the window position
// every frame and a plain setFrameOrigin is what the title-bar drag does too).
// The app never flips movable back.
static bool AppOwnsRmbWindowMove(NSWindow *w) {
    return w != nil && w.isMovable && g_transparentBg && !g_fullscreen;
}

// Never let AppKit start a window drag from a click in the content view
// (mouseDownCanMoveWindow defaults to YES for a non-opaque view, which this
// layer-backed transparent view is) — window moves belong to the runtime's
// placement or to the RMB fallback above.
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)rightMouseDown:(NSEvent *)event {
    if (AppOwnsRmbWindowMove(self.window)) {
        MarkUserInput(g_input);
        g_rmbDragging = true;
        g_rmbMouseStart = [NSEvent mouseLocation];
        g_rmbFrameStart = self.window.frame.origin;
        return;
    }
    [super rightMouseDown:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
    if (g_rmbDragging && self.window != nil && self.window.isMovable) {
        MarkUserInput(g_input);
        // The live cursor, not the event deltas: deltas are lost when events
        // coalesce, and the window moving under the cursor skews them.
        const NSPoint m = [NSEvent mouseLocation];
        const NSPoint o = NSMakePoint(round(g_rmbFrameStart.x + (m.x - g_rmbMouseStart.x)),
                                      round(g_rmbFrameStart.y + (m.y - g_rmbMouseStart.y)));
        if (!NSEqualPoints(o, self.window.frame.origin)) [self.window setFrameOrigin:o];
        return;
    }
    [super rightMouseDragged:event];
}

- (void)rightMouseUp:(NSEvent *)event {
    if (g_rmbDragging) {
        g_rmbDragging = false;
        return;
    }
    [super rightMouseUp:event];
}

- (void)scrollWheel:(NSEvent *)event {
    MarkUserInput(g_input);
    float delta = (float)[event scrollingDeltaY] * 0.02f;
    g_input.viewParams.scaleFactor += delta * 0.5f;
    if (g_input.viewParams.scaleFactor < 0.1f) g_input.viewParams.scaleFactor = 0.1f;
}

- (void)keyDown:(NSEvent *)event {
    MarkUserInput(g_input);
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    // Ctrl+O = open a model. Literal Ctrl (NOT Cmd) on every platform — uniform
    // with the Win/Linux demos AND the mediaplayer (SDL SDL_KMOD_CTRL). Strict:
    // Ctrl must be held. (Was bare L — retired for the uniform chord.)
    if ((ch == 'o' || ch == 'O') && ([event modifierFlags] & NSEventModifierFlagControl)) {
        g_input.loadRequested = true;
        return;
    }
    // Ctrl+T (the Windows/Linux chord) or Cmd+T (the Mac habit) = transparent
    // background. Checked before the switch so plain T stays eye-tracking.
    if ((ch == 't' || ch == 'T') &&
        ([event modifierFlags] & (NSEventModifierFlagControl | NSEventModifierFlagCommand))) {
        g_input.transparentBgToggleRequested = true;
        return;
    }
    switch (ch) {
        case 'w': case 'W': g_input.keyW = true; break;
        case 'a': case 'A': g_input.keyA = true; break;
        case 's': case 'S': g_input.keyS = true; break;
        case 'd': case 'D': g_input.keyD = true; break;
        case 'e': case 'E': g_input.keyE = true; break;
        case 'q': case 'Q': g_input.keyQ = true; break;
        case 'v': case 'V':
            // Cycle through all rendering modes
            if (g_input.renderingModeCount > 0) {
                g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
            }
            g_input.renderingModeChangeRequested = true;
            break;
        case 'm': case 'M':
            g_input.animateToggleRequested = true;
            break;
        case 'n': case 'N':   // next glTF animation clip
            g_input.cycleClipRequested = true;
            break;
        case 'k': case 'K':   // play/pause the active clip
            g_input.playPauseRequested = true;
            break;
        case 'c': case 'C':
            g_input.cameraMode = !g_input.cameraMode;
            break;
        case 'i': case 'I':
            g_input.captureAtlasRequested = true;
            break;
        case 't': case 'T':
            g_input.eyeTrackingModeToggleRequested = true;
            break;
        // (model open moved to Ctrl+O — see the modifier check above the switch)
        case '-': case '_': {
            // Edit steadyIpdFactor (the ModeSwitch ramp target); seed ipdFactor
            // in lockstep for the idle/non-ramp render path.
            float v = g_input.viewParams.steadyIpdFactor - 0.1f;
            if (v < 0.1f) v = 0.1f;
            g_input.viewParams.steadyIpdFactor = v;
            g_input.viewParams.ipdFactor = v;
            g_input.viewParams.parallaxFactor = v;
            break;
        }
        case '=': case '+': {
            float v = g_input.viewParams.steadyIpdFactor + 0.1f;
            if (v > 1.0f) v = 1.0f;
            g_input.viewParams.steadyIpdFactor = v;
            g_input.viewParams.ipdFactor = v;
            g_input.viewParams.parallaxFactor = v;
            break;
        }
        case ' ':
            g_input.resetViewRequested = true;
            break;
        case '0':
            g_input.currentRenderingMode = 0;
            g_input.renderingModeChangeRequested = true;
            break;
        case '1':
            if (g_input.renderingModeCount > 1) g_input.currentRenderingMode = 1;
            g_input.renderingModeChangeRequested = true;
            break;
        case '2':
            if (g_input.renderingModeCount > 2) g_input.currentRenderingMode = 2;
            g_input.renderingModeChangeRequested = true;
            break;
        case '3':
            if (g_input.renderingModeCount > 3) g_input.currentRenderingMode = 3;
            g_input.renderingModeChangeRequested = true;
            break;
        // Viewing conditions (issue #70): exposure in quarter stops, and the
        // tone curve. Both are reported in the HUD so a screen capture records
        // the grading it was taken under.
        case '[':
            g_modelRenderer.setExposureEV(g_modelRenderer.exposureEV() - 0.25f);
            LOG_INFO("Exposure: %+.2f EV", g_modelRenderer.exposureEV());
            break;
        case ']':
            g_modelRenderer.setExposureEV(g_modelRenderer.exposureEV() + 0.25f);
            LOG_INFO("Exposure: %+.2f EV", g_modelRenderer.exposureEV());
            break;
        case 'g': case 'G':   // grading: cycle the tone curve (T is eye-tracking)
            g_modelRenderer.cycleToneCurve();
            LOG_INFO("Tone curve: %s", g_modelRenderer.toneCurveName());
            break;
        case '\t':
            g_input.hudVisible = !g_input.hudVisible;
            break;
        case 27: // ESC
            g_running = false;
            break;
    }
}

- (void)keyUp:(NSEvent *)event {
    MarkUserInput(g_input);
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 'w': case 'W': g_input.keyW = false; break;
        case 'a': case 'A': g_input.keyA = false; break;
        case 's': case 'S': g_input.keyS = false; break;
        case 'd': case 'D': g_input.keyD = false; break;
        case 'e': case 'E': g_input.keyE = false; break;
        case 'q': case 'Q': g_input.keyQ = false; break;
    }
}

// Drag-and-drop: accept .glb and .gltf files
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([[pb types] containsObject:NSPasteboardTypeFileURL]) {
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]] options:nil];
    for (NSURL *url in urls) {
        if (![url isFileURL]) continue;
        NSString *path = [url path];
        NSString *ext = [[path pathExtension] lowercaseString];
        if ([ext isEqualToString:@"glb"] || [ext isEqualToString:@"gltf"]) {
            g_input.pendingLoadPath = std::string([path UTF8String]);
            return YES;
        }
    }
    return NO;
}

- (void)flagsChanged:(NSEvent *)event {
    // Cmd+Ctrl+F = fullscreen toggle
    NSUInteger flags = [event modifierFlags];
    (void)flags;
}
@end

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// SIGUSR1 = Ctrl+T, for scripted runs (no TCC needed to deliver a keystroke):
// `kill -USR1 <pid>`. Only sets the same request flag the key does.
static void ToggleSignalHandler(int sig) {
    (void)sig;
    g_input.transparentBgToggleRequested = true;
}

// ---------------------------------------------------------------------------
// Toast — a transient confirmation chip, bottom-centre (the Windows leg's
// dxr::ToastState, as an AppKit view: the macOS HUD is AppKit already).
// ---------------------------------------------------------------------------
static NSVisualEffectView *g_toastBackdrop = nil;
static NSTextField *g_toastLabel = nil;
static double g_toastUntilSec = 0.0;

static void ShowToast(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_toastBackdrop == nil || g_toastLabel == nil || g_metalView == nil) return;
    [g_toastLabel setStringValue:[NSString stringWithUTF8String:buf]];
    [g_toastLabel sizeToFit];
    const NSSize ts = g_toastLabel.frame.size;
    const CGFloat padX = 18.0, padY = 10.0;
    const NSSize vs = g_metalView.bounds.size;
    const CGFloat w = ceil(ts.width + 2 * padX), h = ceil(ts.height + 2 * padY);
    [g_toastBackdrop setFrame:NSMakeRect(floor((vs.width - w) * 0.5), floor(vs.height * 0.10), w, h)];
    [g_toastLabel setFrameOrigin:NSMakePoint(padX, padY)];
    [g_toastBackdrop setHidden:NO];
    g_toastUntilSec = NowSec() + 1.6;
}

static void UpdateToast() {
    if (g_toastBackdrop != nil && !g_toastBackdrop.hidden && NowSec() >= g_toastUntilSec)
        [g_toastBackdrop setHidden:YES];
}

static bool CreateMacOSWindow(uint32_t width, uint32_t height,
                              int32_t screenLeft, int32_t screenTop) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    // INV-1.3: open on the 3D panel. (screenLeft, screenTop) is the panel
    // top-left in top-down global coordinates (origin = primary top-left,
    // XrDisplayDesktopPositionDXR, runtime#715); flip into AppKit's bottom-up
    // space.
    //
    // Resolve the NSScreen that holds the panel's top-left and put the WHOLE
    // window (title bar included) inside its visibleFrame. Placing the content
    // top AT the panel top pushes the title bar off-screen, and AppKit then
    // re-constrains the window onto [NSScreen mainScreen] — whichever screen
    // has focus at launch, i.e. often the laptop, not the 3D panel. Same fix
    // as the runtime's cube_handle_{metal,vk}_macos.
    NSUInteger style = kTitledWindowStyle;
    NSRect frame = NSMakeRect(100, 100, width, height);
    NSScreen *target = nil;
    NSScreen *primary = [NSScreen screens].firstObject;
    if (primary != nil) {
        const NSPoint p = NSMakePoint((CGFloat)screenLeft + 1.0,
                                      primary.frame.size.height - (CGFloat)screenTop - 1.0);
        for (NSScreen *s in [NSScreen screens]) {
            if (NSPointInRect(p, s.frame)) { target = s; break; }
        }
    }
    if (target != nil) {
        const NSRect vf = target.visibleFrame; // below that screen's menu bar
        const NSRect wf = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, width, height) styleMask:style];
        const CGFloat titleH = wf.size.height - (CGFloat)height;
        frame = NSMakeRect(vf.origin.x, NSMaxY(vf) - titleH - (CGFloat)height, width, height);
    }
    g_window = [[ModelViewerWindow alloc] initWithContentRect:frame
        styleMask:style backing:NSBackingStoreBuffered defer:NO];
    if (target != nil) {
        // Global coordinates, title bar on-screen: no re-constrain.
        [g_window setFrame:[g_window frameRectForContentRect:frame] display:NO];
    }
    [g_window setTitle:@"DisplayXR 3D Model Viewer"];
    // Nothing in the app moves the window on a background / content drag:
    // the runtime's placement owns moves (it sets movable = NO itself; the app
    // never touches movable), and RMB-drag is the runtime's (or the fallback's).
    [g_window setMovableByWindowBackground:NO];
    [g_window setDelegate:delegate];
    // Transparent-capable session (#181): the window itself must not paint an
    // opaque background behind the CAMetalLayer, or alpha 0 shows the window
    // colour instead of the desktop. App-owned window, so this is the app's
    // job (the runtime repeats it for the external view at xrCreateSession).
    // Opaque frames are alpha 1 everywhere, so nothing changes until Ctrl+T.
    if (g_transparentCapable) {
        [g_window setOpaque:NO];
        [g_window setBackgroundColor:[NSColor clearColor]];
    }

    g_metalView = [[MetalView alloc] initWithFrame:frame];
    [g_window setContentView:g_metalView];
    // 1:1 with the backing store (see MetalView::makeBackingLayer). Create the
    // layer NOW (AppKit otherwise makes it lazily, after the runtime adopts it)
    // and set its scale.
    [g_metalView setWantsLayer:YES];
    g_metalView.layer.contentsScale = g_window.backingScaleFactor;
    [g_window makeKeyAndOrderFront:nil];
    [g_window makeFirstResponder:g_metalView];

    // Accept drag-and-drop of .glb / .gltf files
    [g_metalView registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    // Add HUD overlay — frosted backdrop + text view inside
    NSRect hudFrame = NSMakeRect(8, 8, 320, 520);
    g_hudBackdrop = [[NSVisualEffectView alloc] initWithFrame:hudFrame];
    [g_hudBackdrop setMaterial:NSVisualEffectMaterialHUDWindow];
    [g_hudBackdrop setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [g_hudBackdrop setState:NSVisualEffectStateActive];
    [g_hudBackdrop setWantsLayer:YES];
    g_hudBackdrop.layer.cornerRadius = 8.0;
    g_hudBackdrop.layer.masksToBounds = YES;
    [g_metalView addSubview:g_hudBackdrop];

    g_hudView = [[HudOverlayView alloc] initWithFrame:g_hudBackdrop.bounds];
    [g_hudView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [g_hudBackdrop addSubview:g_hudView];

    // --- Top bar (Open / Mode) — transparent so the buttons sit directly
    // over the rendered content (no frosted panel hiding the top of the scene).
    const CGFloat barH = 48.0;
    NSRect barFrame = NSMakeRect(0, height - barH, width, barH);
    NSView *topBar = [[NSView alloc] initWithFrame:barFrame];
    [topBar setWantsLayer:YES];
    [[topBar layer] setBackgroundColor:[[NSColor clearColor] CGColor]];
    [topBar setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    g_topBar = topBar;
    [g_metalView addSubview:g_topBar];

    const CGFloat btnH = 32.0, btnY = (barH - btnH) * 0.5f;
    const CGFloat gap = 10.0;
    CGFloat x = 12.0;
    CGFloat openW = 96.0, modeW = 220.0;

    // Helper: wrap a button in a glassy NSVisualEffectView backdrop matching
    // the HUD's HUDWindow material so the controls have the same look.
    NSView * (^makeGlassyButton)(NSRect, NSString*, SEL, NSButton **) =
        ^NSView *(NSRect frame, NSString *title, SEL act, NSButton **outBtn) {
        NSVisualEffectView *bd = [[NSVisualEffectView alloc] initWithFrame:frame];
        [bd setMaterial:NSVisualEffectMaterialHUDWindow];
        [bd setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
        [bd setState:NSVisualEffectStateActive];
        [bd setWantsLayer:YES];
        bd.layer.cornerRadius = 6.0;
        bd.layer.masksToBounds = YES;
        NSButton *b = [[NSButton alloc] initWithFrame:bd.bounds];
        [b setTitle:title];
        [b setBezelStyle:NSBezelStyleInline];
        [b setBordered:NO];
        [b setTarget:nil];
        [b setAction:act];
        [b setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [bd addSubview:b];
        if (outBtn) *outBtn = b;
        return bd;
    };

    NSView *openBd = makeGlassyButton(NSMakeRect(x, btnY, openW, btnH),
                                       @"Open…", @selector(openButtonClicked:),
                                       &g_openButton);
    [g_topBar addSubview:openBd];
    x += openW + gap;

    NSView *modeBd = makeGlassyButton(NSMakeRect(x, btnY, modeW, btnH),
                                       @"Mode: —", @selector(modeButtonClicked:),
                                       &g_modeButton);
    [g_topBar addSubview:modeBd];

    // Animation button — right-justified (pinned to the right edge via
    // NSViewMinXMargin so it stays put on resize). Hidden until a model with
    // clips loads (see UpdateTopBarButtonTitles). Click = next clip (N-key).
    const CGFloat animW = 160.0;
    NSView *animBd = makeGlassyButton(NSMakeRect(width - animW - 12.0, btnY, animW, btnH),
                                       @"", @selector(animationButtonClicked:),
                                       &g_animButton);
    [animBd setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [animBd setHidden:YES];
    g_animButtonBackdrop = animBd;
    [g_topBar addSubview:animBd];

    // Toast chip (hidden until ShowToast).
    g_toastBackdrop = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    [g_toastBackdrop setMaterial:NSVisualEffectMaterialHUDWindow];
    [g_toastBackdrop setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [g_toastBackdrop setState:NSVisualEffectStateActive];
    [g_toastBackdrop setWantsLayer:YES];
    g_toastBackdrop.layer.cornerRadius = 10.0;
    g_toastBackdrop.layer.masksToBounds = YES;
    [g_toastBackdrop setHidden:YES];
    g_toastLabel = [NSTextField labelWithString:@""];
    [g_toastLabel setFont:[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold]];
    [g_toastLabel setTextColor:[NSColor labelColor]];
    [g_toastBackdrop addSubview:g_toastLabel];
    [g_metalView addSubview:g_toastBackdrop];

    // A/B knob (diagnostic): DISPLAYXR_MV_OVERLAYS=0 removes every AppKit view
    // stacked over the CAMetalLayer (frosted HUD + top-bar NSVisualEffectViews).
    // Overlapping AppKit/vibrancy views make WindowServer composite the Metal
    // layer instead of presenting it directly, which can resample a lenticular
    // weave. Controls stay on the keyboard.
    {
        const char *ov = getenv("DISPLAYXR_MV_OVERLAYS");
        if (ov != nullptr && strcmp(ov, "0") == 0) {
            [g_hudBackdrop removeFromSuperview];
            [g_topBar removeFromSuperview];
            g_hudBackdrop = nil;
            g_hudView = nil;
            g_topBar = nil;
            g_openButton = nil;
            g_modeButton = nil;
            g_animButton = nil;
            g_animButtonBackdrop = nil;
            [g_toastBackdrop removeFromSuperview];
            g_toastBackdrop = nil;
            g_toastLabel = nil;
            LOG_INFO("DISPLAYXR_MV_OVERLAYS=0: HUD and top bar removed (nothing over the Metal layer)");
        }
    }

    [NSApp activateIgnoringOtherApps:YES];
    LOG_INFO("macOS window created (%ux%u)", width, height);
    return true;
}

// Button action handlers (added as category on NSApplication)
@interface NSApplication (TopBarActions)
- (void)openButtonClicked:(id)sender;
- (void)modeButtonClicked:(id)sender;
- (void)animationButtonClicked:(id)sender;
@end

@implementation NSApplication (TopBarActions)
- (void)openButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    g_input.loadRequested = true;
}
- (void)modeButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    if (g_input.renderingModeCount > 0) {
        g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
    }
    g_input.renderingModeChangeRequested = true;
}
- (void)animationButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    g_input.cycleClipRequested = true;   // next clip (N-key equivalent)
}
@end

// NOTE: UpdateTopBarButtonTitles() body lives after the AppXrSession struct
// definition further below, since it accesses its members.

static void PumpMacOSEvents() {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

// Swap the window's style to/from borderless while keeping its CONTENT rect
// (screen points, position + size) identical, the window key and the content
// view first responder. setStyleMask alone keeps the FRAME, so the content
// rect would grow/shrink by the title bar; the frame is recomputed from the
// saved content rect right after. Never setFrame:display:YES (the view's
// CAMetalLayer is the runtime's present target; it presents on its own).
static void SetWindowStylePreservingContent(NSUInteger style, NSRect content) {
    if (g_window == nil) return;
    const bool wasKey = g_window.isKeyWindow;
    if (g_window.styleMask != style) [g_window setStyleMask:style];
    const NSRect want = [g_window frameRectForContentRect:content];
    if (!NSEqualRects(want, g_window.frame)) [g_window setFrame:want display:NO animate:NO];
    // A style change can drop key status / the first responder.
    if (wasKey && !g_window.isKeyWindow) [g_window makeKeyWindow];
    if (g_window.firstResponder != g_metalView) [g_window makeFirstResponder:g_metalView];
}

static NSUInteger WindowedStyle() {
    return g_transparentBg ? NSWindowStyleMaskBorderless : kTitledWindowStyle;
}

static void ToggleBorderlessFullscreen() {
    if (g_fullscreen) {
        // Back to the windowed style the CURRENT background wants (Ctrl+T may
        // have flipped while fullscreen) and the windowed z-band: floating
        // while transparent (#181).
        g_fullscreen = false;
        SetWindowStylePreservingContent(WindowedStyle(), g_savedContentRect);
        [g_window setLevel:g_transparentBg ? NSFloatingWindowLevel : NSNormalWindowLevel];
        LOG_INFO("Exited fullscreen");
    } else {
        g_savedContentRect = [g_window contentRectForFrameRect:[g_window frame]];
        NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
        SetWindowStylePreservingContent(NSWindowStyleMaskBorderless, [screen frame]);
        [g_window setLevel:NSStatusWindowLevel];
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen");
    }
}

// Window side of Ctrl+T (#181). Keep-above while transparent — the Windows
// leg's HWND_TOPMOST, Linux's _NET_WM_STATE_ABOVE — and no window shadow: the
// shadow AppKit derives from a non-opaque window's content would outline the
// floating model with a grey halo against the desktop. Opaque restores both
// and makes the whole window clickable again. Fullscreen keeps its own level.
static void ApplyTransparentWindowState() {
    if (g_window == nil) return;
    if (!g_fullscreen) {
        // No decorations while transparent (borderless; RMB-drag moves it),
        // the normal titled window when opaque. Fullscreen is borderless
        // already and picks the style on exit.
        SetWindowStylePreservingContent(WindowedStyle(),
                                        [g_window contentRectForFrameRect:[g_window frame]]);
        [g_window setLevel:g_transparentBg ? NSFloatingWindowLevel : NSNormalWindowLevel];
    }
    if (!g_transparentBg) g_rmbDragging = false;
    // On-window chrome, exactly as the Windows leg (#833, "chrome is hidden in
    // transparent/punch-through mode by design"): the top button bar (Open /
    // Mode / Animation) is dropped while transparent and comes back on opaque.
    // The Tab HUD panel and the toast chip are NOT touched — Windows keeps
    // both (the toast is unioned into its click region). A hidden view is also
    // out of UpdateClickThrough's chrome hit-test, so the bar's area passes
    // clicks through like any other transparent surround.
    if (g_topBar != nil) [g_topBar setHidden:g_transparentBg ? YES : NO];
    [g_window setHasShadow:g_transparentBg ? NO : YES];
    if (!g_transparentBg && g_ignoringMouse) {
        [g_window setIgnoresMouseEvents:NO];
        g_ignoringMouse = false;
    }
    [g_window invalidateShadow];
}

// Click-through, per frame (#181). Cocoa has no input region, only the
// all-or-nothing NSWindow.ignoresMouseEvents, so the decision is made for the
// CURRENT cursor position from a global poll ([NSEvent mouseLocation]) — once
// the window ignores the mouse it receives no mouse-moved events, so window
// events could never turn it back on. Content under the cursor (the frame's
// own alpha, clickthrough.cpp) or one of the app's AppKit controls = clickable;
// transparent surround = pass through to whatever is underneath.
static void UpdateClickThrough() {
    if (g_window == nil || g_metalView == nil) return;
    bool wantIgnore = false;
    if (g_transparentBg) {
        // Never flip mid-gesture: a drag that leaves the silhouette must keep
        // its mouseDragged stream (and its mouseUp).
        if ([NSEvent pressedMouseButtons] != 0) return;
        const NSRect frame = [g_window frame];
        const NSRect content = [g_window contentRectForFrameRect:frame];
        const NSPoint p = [NSEvent mouseLocation];   // global, bottom-up
        if (NSPointInRect(p, content) && content.size.width > 0 && content.size.height > 0) {
            // Chrome first: the app's own AppKit controls (top-bar buttons,
            // a visible HUD/toast) stay clickable — the Linux helper's "union
            // the header bar in". The transparent top-bar container itself
            // does not count.
            const NSPoint inWin = [g_window convertPointFromScreen:p];
            NSView *hv = [g_metalView hitTest:[g_metalView.superview convertPoint:inWin fromView:nil]];
            const bool overChrome = hv != nil && hv != g_metalView && hv != g_topBar;
            bool hit = false;
            const float u = (float)((p.x - content.origin.x) / content.size.width);
            const float v = 1.0f - (float)((p.y - content.origin.y) / content.size.height);
            const bool haveCov = MacClickthroughHit(u, v, &hit);
            // Reachability net (Linux/Windows): a silhouette below 64x64 px of
            // window area (model panned away, nothing loaded) keeps the whole
            // window clickable so Ctrl+T can still bring it back.
            const CGFloat bs = g_window.backingScaleFactor > 0 ? g_window.backingScaleFactor : 1.0;
            const uint64_t coveredPt = MacClickthroughCoveredPx((uint32_t)content.size.width,
                                                                (uint32_t)content.size.height);
            const bool reachable = (uint64_t)((double)coveredPt * bs * bs) >= 64ull * 64ull;
            static int s_lastReach = -1;
            if (haveCov && (reachable ? 1 : 0) != s_lastReach) {
                s_lastReach = reachable ? 1 : 0;
                if (!reachable)
                    LOG_WARN("clickthrough: silhouette covers only ~%llu px — keeping the window "
                             "clickable (Ctrl+T still works)", (unsigned long long)(coveredPt * bs * bs));
            }
            wantIgnore = haveCov && reachable && !overChrome && !hit;
            g_ctLast = {true, haveCov, hit, overChrome, reachable, u, v};
        } else {
            g_ctLast.inContent = false;
        }
    }
    if (wantIgnore != g_ignoringMouse) {
        [g_window setIgnoresMouseEvents:wantIgnore ? YES : NO];
        g_ignoringMouse = wantIgnore;
        static bool s_logged = false;
        if (wantIgnore && !s_logged) {
            s_logged = true;
            LOG_INFO("clickthrough: cursor over the transparent surround — clicks pass through "
                     "(logged once)");
        }
    }
}

// ============================================================================
// OpenXR Session (ported from cube_handle_vk_macos)
// ============================================================================

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    char systemName[256] = {};

    // Swapchain
    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    // Display info from XR_DXR_display_info
    bool hasDisplayInfoExt = false;
    bool hasCocoaWindowBinding = false;
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerX = 0, nominalViewerY = 0, nominalViewerZ = 0.5f;
    float recommendedViewScaleX = 0.5f, recommendedViewScaleY = 1.0f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
    // INV-1.3 (v16, runtime#715): 3D panel top-left in top-down virtual-desktop
    // pixels (XrDisplayDesktopPositionDXR). (0,0) = primary/unknown — the safe
    // default, which zero-init also yields on pre-v16 runtimes.
    int32_t displayScreenLeft = 0, displayScreenTop = 0;

    // Eye tracking
    float eyePositions[8][3] = {};  // [view][x,y,z] — raw per-eye positions in display space
    bool eyeTrackingActive = false;
    bool isEyeTracking = false;
    uint32_t activeEyeTrackingMode = 0;
    uint32_t supportedEyeTrackingModes = 0;

    // Function pointers
    PFN_xrRequestDisplayModeDXR pfnRequestDisplayModeEXT = nullptr;
    PFN_xrRequestEyeTrackingModeDXR pfnRequestEyeTrackingModeEXT = nullptr;
    PFN_xrRequestDisplayRenderingModeDXR pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateDisplayRenderingModesEXT = nullptr;

    // XR_DXR_atlas_capture (W6 of #396): runtime-owned 'I' key capture.
    bool hasAtlasCaptureExt = false;
    PFN_xrCaptureAtlasDXR pfnCaptureAtlasEXT = nullptr;

    // XR_DXR_view_rig (W7 of #396): runtime owns the off-axis Kooima and returns
    // render-ready XrView{pose, fov}; the app deletes its own.
    bool hasViewRigExt = false;
    uint32_t viewRigSpecVersion = 0;

    // XR_EXT_hand_tracking: advertised by the runtime; whether a hand-tracking
    // provider is actually present is a SYSTEM property (HandTracking::init).
    bool hasHandTrackingExt = false;

    // XR_DXR_cursor_depth (runtime ADR-046): the runtime draws a depth cursor
    // in front of content that pops out under the pointer. 0 = not enabled.
    uint32_t cursorDepthSpecVersion = 0;

    // XR_DXR_depth_budget (ADR-040). Optional — an older runtime leaves it
    // off and ResolveClipPlanes falls back to the pre-extension rule.
    bool hasDepthBudgetExt = false;
    uint32_t depthBudgetExtVersion = 0;

    // XR_DXR_mcp_tools (#22): app-defined agent tools on the runtime-hosted
    // per-process MCP server. The whole path is inert when the extension or
    // the MCP capability gate is absent — never load-bearing.
    bool hasMcpToolsExt = false;
    PFN_xrSetMCPAppInfoDXR pfnSetMCPAppInfo = nullptr;
    PFN_xrRegisterMCPToolDXR pfnRegisterMCPTool = nullptr;
    PFN_xrUnregisterMCPToolDXR pfnUnregisterMCPTool = nullptr;
    PFN_xrGetMCPToolCallArgsDXR pfnGetMCPToolCallArgs = nullptr;
    PFN_xrSubmitMCPToolResultDXR pfnSubmitMCPToolResult = nullptr;
    bool mcpToolsReady = false;           // appId declared + base tools registered
    bool mcpAnimToolsRegistered = false;  // list/play/stop_animation currently live

    // Enumerated rendering mode info
    uint32_t renderingModeCount = 0;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE] = {};
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    bool renderingModeIsRequestable[8] = {};    // false = a workspace controller owns the mode
    uint32_t renderingModeTileColumns[8] = {};  // atlas tile layout (v12)
    uint32_t renderingModeTileRows[8] = {};
    //! Array index of the mode the runtime reports isActive at enumeration
    //! (-1 = none) — the app renders whatever mode the display is in.
    int32_t activeRenderingMode = -1;

    // Max views the runtime may return from xrLocateViews, taken from
    // xrEnumerateViewConfigurationViews at session init. Some runtimes (e.g.
    // sim_display on macOS) report the union across all rendering modes, so
    // this is >= 2 even for PRIMARY_STEREO.
    uint32_t maxViewCount = 2;

    void* windowHandle = nullptr;  // unused on macOS, kept for compatibility
};

// Session reachable from the model-load paths (they all funnel through
// ApplyAutoFitForLoadedScene, which is xr-free) so they can flip the
// animation-tool registration. Set in main() right after CreateSession.
static AppXrSession* g_xrForMcp = nullptr;

static void UpdateTopBarButtonTitles(AppXrSession& xr) {
    if (g_modeButton) {
        const char *name = "Unknown";
        if (xr.renderingModeCount > 0 &&
            g_input.currentRenderingMode < xr.renderingModeCount &&
            xr.renderingModeNames[g_input.currentRenderingMode][0] != '\0') {
            name = xr.renderingModeNames[g_input.currentRenderingMode];
        }
        [g_modeButton setTitle:[NSString stringWithFormat:@"Mode: %s", name]];
    }
}

// Refresh the right-justified animation button: shown only when the model has
// clips; label = current clip name, or "Paused". Called on load + N/K events.
// xr-free so the load hook (ApplyAutoFitForLoadedScene) can call it.
static void UpdateAnimButton() {
    if (!g_animButton || !g_animButtonBackdrop) return;
    std::string clip; int ci, cn; float ct, cd; bool playing;
    if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing)) {
        [g_animButtonBackdrop setHidden:NO];
        [g_animButton setTitle:playing ? [NSString stringWithUTF8String:clip.c_str()] : @"Paused"];
    } else {
        [g_animButtonBackdrop setHidden:YES];
    }
}

// ★ XR_DXR_mcp_tools late registration (#22): the animation tools exist only
// while a model with clips is loaded; they are unregistered when the model is
// replaced by one without. Each transition makes the runtime broadcast the MCP
// tools/list_changed notification, so agents connected BEFORE a load see the
// tools appear/disappear live. Called from every successful load path (they
// all funnel through ApplyAutoFitForLoadedScene).
static void UpdateMcpAnimationTools() {
    AppXrSession* xr = g_xrForMcp;
    if (!xr || !xr->mcpToolsReady || !xr->pfnRegisterMCPTool || !xr->pfnUnregisterMCPTool)
        return;
    const bool want = g_modelRenderer.hasAnimations();
    if (want == xr->mcpAnimToolsRegistered) return;

    if (want) {
        XrMCPToolInfoDXR listTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        listTool.name = "list_animations";
        listTool.description =
            "List the loaded model's animation clips: index, name and duration in "
            "seconds, plus the active clip index and whether playback is running. "
            "Only available while a model with animation clips is loaded.";
        listTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r1 = xr->pfnRegisterMCPTool(xr->session, &listTool);

        XrMCPToolInfoDXR playTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        playTool.name = "play_animation";
        playTool.description =
            "Play an animation clip, selected by 'index' or 'name' (see "
            "list_animations). Omit both to resume the active clip. Selecting a "
            "different clip restarts it from t=0. Returns the now-playing clip; "
            "verify visually with capture_frame.";
        playTool.inputSchemaJson =
            "{\"type\":\"object\",\"properties\":{"
            "\"index\":{\"type\":\"integer\",\"description\":\"Clip index from list_animations.\"},"
            "\"name\":{\"type\":\"string\",\"description\":\"Clip name from list_animations.\"}}}";
        XrResult r2 = xr->pfnRegisterMCPTool(xr->session, &playTool);

        XrMCPToolInfoDXR stopTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        stopTool.name = "stop_animation";
        stopTool.description =
            "Pause animation playback, freezing the model at its current pose. "
            "Resume with play_animation.";
        stopTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r3 = xr->pfnRegisterMCPTool(xr->session, &stopTool);

        xr->mcpAnimToolsRegistered = XR_SUCCEEDED(r1) || XR_SUCCEEDED(r2) || XR_SUCCEEDED(r3);
        LOG_INFO("XR_DXR_mcp_tools: animation tools registered (%d clip(s)) [%d %d %d]",
                 g_modelRenderer.animationCount(), r1, r2, r3);
    } else {
        xr->pfnUnregisterMCPTool(xr->session, "list_animations");
        xr->pfnUnregisterMCPTool(xr->session, "play_animation");
        xr->pfnUnregisterMCPTool(xr->session, "stop_animation");
        xr->mcpAnimToolsRegistered = false;
        LOG_INFO("XR_DXR_mcp_tools: animation tools unregistered (model has no clips)");
    }
}

// Forward declarations for OpenXR functions (same as cube_handle_vk_macos)
static bool InitializeOpenXR(AppXrSession& xr);
static bool GetVulkanGraphicsRequirements(AppXrSession& xr);
static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance);
static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);
static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice);
static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);
static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& exts, VkDevice& device, VkQueue& queue);
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex);
static bool CreateSpaces(AppXrSession& xr);
static bool CreateSwapchains(AppXrSession& xr);
static void PollEvents(AppXrSession& xr);
static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState);
static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex);
static void ReleaseSwapchainImage(AppXrSession& xr);
static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount,
    const void* frameEndNext = nullptr);
static void CleanupOpenXR(AppXrSession& xr);

// ============================================================================
// OpenXR implementation (abbreviated — same logic as cube_handle_vk_macos)
// ============================================================================

static bool InitializeOpenXR(AppXrSession& xr) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& ext : exts) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasCocoaWindowBinding = true;
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(ext.extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0) xr.hasAtlasCaptureExt = true;
        if (strcmp(ext.extensionName, XR_DXR_MCP_TOOLS_EXTENSION_NAME) == 0) xr.hasMcpToolsExt = true;
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) {
            xr.hasViewRigExt = true;
            xr.viewRigSpecVersion = ext.extensionVersion;   // rig-scoped hand joints need >= 4
        }
        if (strcmp(ext.extensionName, modelviewer::HandTracking::kExtensionName) == 0) xr.hasHandTrackingExt = true;
        if (strcmp(ext.extensionName, XR_DXR_CURSOR_DEPTH_EXTENSION_NAME) == 0 &&
            modelviewer::CursorDepthWanted())
            xr.cursorDepthSpecVersion = ext.extensionVersion;
        if (strcmp(ext.extensionName, XR_DXR_DEPTH_BUDGET_EXTENSION_NAME) == 0) {
            xr.hasDepthBudgetExt = true;
            xr.depthBudgetExtVersion = ext.extensionVersion;
        }
    }

    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasCocoaWindowBinding) enabled.push_back(XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasAtlasCaptureExt) enabled.push_back(XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME);
    if (xr.hasMcpToolsExt) enabled.push_back(XR_DXR_MCP_TOOLS_EXTENSION_NAME);
    if (xr.hasViewRigExt) enabled.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    LOG_INFO("XR_DXR_view_rig: %s", xr.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");
    if (xr.hasHandTrackingExt) enabled.push_back(modelviewer::HandTracking::kExtensionName);
    LOG_INFO("XR_EXT_hand_tracking: %s", xr.hasHandTrackingExt ? "AVAILABLE" : "NOT FOUND");
    if (xr.cursorDepthSpecVersion > 0) enabled.push_back(XR_DXR_CURSOR_DEPTH_EXTENSION_NAME);
    LOG_INFO("XR_DXR_cursor_depth: %s (spec v%u)", xr.cursorDepthSpecVersion ? "ENABLED" : "off",
             xr.cursorDepthSpecVersion);
    // XR_DXR_depth_budget (ADR-040): opt in so the runtime publishes its rear
    // depth budget on xrLocateViews (consumed in transparent mode only).
    if (xr.hasDepthBudgetExt) enabled.push_back(XR_DXR_DEPTH_BUDGET_EXTENSION_NAME);
    LOG_INFO("XR_DXR_depth_budget: %s (v%u)", xr.hasDepthBudgetExt ? "AVAILABLE" : "NOT FOUND",
             xr.depthBudgetExtVersion);

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "DisplayXRModelViewerMacOS", sizeof(ci.applicationInfo.applicationName));
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName));
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &xr.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

    // runtime #1486/#1500: this app's per-frame view count comes from the ACTIVE
    // DXR rendering mode (sim_display's Quad mode = 4 views), so it must begin its
    // session with PRIMARY_MULTIVIEW_DXR — PRIMARY_STEREO now reports EXACTLY 2 and
    // rejects an xrEndFrame projection layer carrying more. One call here, before
    // the first xrEnumerateViewConfigurationViews (CreateSwapchains); the same
    // xr.viewConfigType feeds XrSessionBeginInfo + XrViewLocateInfo below.
    // Degrades to PRIMARY_STEREO on an older runtime.
    xr.viewConfigType = DxrSelectViewConfigType(xr.instance, xr.systemId);
    LOG_INFO("View configuration: %s", DxrViewConfigTypeName(xr.viewConfigType));

    { XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
      xrGetSystemProperties(xr.instance, xr.systemId, &sp);
      memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName)); }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
        XrEyeTrackingModeCapabilitiesDXR ec = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_DXR};
        // INV-1.3 (v16, runtime#715): panel desktop position, so the window
        // opens on the 3D panel instead of the primary monitor.
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        desktopPos.next = &ec;
        di.next = &desktopPos; sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.recommendedViewScaleX = di.recommendedViewScaleX;
            xr.recommendedViewScaleY = di.recommendedViewScaleY;
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerX = di.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = di.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)ec.supportedModes;
            xr.displayScreenLeft = desktopPos.left;
            xr.displayScreenTop = desktopPos.top;
            LOG_INFO("Display desktop position: (%d, %d)",
                xr.displayScreenLeft, xr.displayScreenTop);
        }
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeDXR", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
        if (xr.supportedEyeTrackingModes != 0)
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeDXR", (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR", (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    // XR_DXR_atlas_capture (W6 of #396): resolve the runtime-owned capture entry.
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasDXR", (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
        LOG_INFO("xrCaptureAtlasDXR: %s", xr.pfnCaptureAtlasEXT ? "resolved" : "NULL");
    }

    // XR_DXR_mcp_tools (#22): resolve the agent-tool entry points. Tools are
    // registered after session create (CreateSession) and dispatched from
    // PollEvents.
    if (xr.hasMcpToolsExt) {
        xrGetInstanceProcAddr(xr.instance, "xrSetMCPAppInfoDXR", (PFN_xrVoidFunction*)&xr.pfnSetMCPAppInfo);
        xrGetInstanceProcAddr(xr.instance, "xrRegisterMCPToolDXR", (PFN_xrVoidFunction*)&xr.pfnRegisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrUnregisterMCPToolDXR", (PFN_xrVoidFunction*)&xr.pfnUnregisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrGetMCPToolCallArgsDXR", (PFN_xrVoidFunction*)&xr.pfnGetMCPToolCallArgs);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitMCPToolResultDXR", (PFN_xrVoidFunction*)&xr.pfnSubmitMCPToolResult);
        const bool resolved = xr.pfnSetMCPAppInfo && xr.pfnRegisterMCPTool &&
            xr.pfnUnregisterMCPTool && xr.pfnGetMCPToolCallArgs && xr.pfnSubmitMCPToolResult;
        LOG_INFO("XR_DXR_mcp_tools entry points: %s", resolved ? "resolved" : "NULL");
    } else {
        LOG_INFO("XR_DXR_mcp_tools: not advertised by runtime");
    }

    LOG_INFO("OpenXR initialized: %s", xr.systemName);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames;
    std::vector<const char*> extPtrs;
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') extNames.push_back(n);
        s = e + 1;
    }}
    // Add portability enumeration for MoltenVK
    extNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "DisplayXRModelViewerMacOS";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)extPtrs.size();
    ci.ppEnabledExtensionNames = extPtrs.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& pd) {
    PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    LOG_INFO("GPU: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') storage.push_back(n);
        s = e + 1;
    }}
    // Add portability subset for MoltenVK
    storage.push_back("VK_KHR_portability_subset");
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
    }
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
    const std::vector<const char*>& exts, VkDevice& dev, VkQueue& queue) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = qfi; qi.queueCount = 1; qi.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures features = {};
    features.shaderInt64 = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
    VkDevice dev, uint32_t qfi) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    XrCocoaWindowBindingCreateInfoDXR macBinding = {(XrStructureType)XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR};
    macBinding.viewHandle = (__bridge void*)g_metalView;
    // #181: ALWAYS request a transparent-capable layer (Windows/Linux parity).
    // The runtime fixes CAMetalLayer.opaque + the compositeAlpha here, once;
    // Ctrl+T later changes only what the app draws (alpha-0 clear, no skybox).
    // MODEL_TRANSPARENT=0 is the one opt-out.
    macBinding.transparentBackgroundEnabled = g_transparentCapable ? XR_TRUE : XR_FALSE;
    if (xr.hasCocoaWindowBinding && g_metalView) {
        vkBinding.next = &macBinding;
        LOG_INFO("Using XR_DXR_cocoa_window_binding (transparentBackgroundEnabled=%s%s)",
                 g_transparentCapable ? "XR_TRUE" : "XR_FALSE",
                 g_transparentCapable ? (g_transparentBg ? ", starting TRANSPARENT" : ", starting opaque") : "");
    } else if (g_transparentCapable) {
        // Hosted-NULL fallback: no app window to make see-through.
        g_transparentCapable = false;
        g_transparentBg = false;
        LOG_INFO("No cocoa window binding — transparent background unavailable");
    }

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = &vkBinding; si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));

    // XR_DXR_mcp_tools (#22): declare identity + register the base agent
    // tools. The appId MUST match `id` in
    // displayxr/model_viewer_handle_vk_macos.displayxr.json (INV-10.1).
    // Failure is non-fatal by design — the MCP capability gate may simply be
    // off on this machine; the viewer runs identically without an agent
    // surface. The animation tools are NOT registered here: they appear only
    // once a model with clips loads (UpdateMcpAnimationTools).
    if (xr.hasMcpToolsExt && xr.pfnSetMCPAppInfo && xr.pfnRegisterMCPTool) {
        XrMCPAppInfoDXR mcpAppInfo = {XR_TYPE_MCP_APP_INFO_DXR};
        strncpy(mcpAppInfo.appId, "modelviewer", sizeof(mcpAppInfo.appId) - 1);
        XrResult ar = xr.pfnSetMCPAppInfo(xr.session, &mcpAppInfo);
        if (XR_SUCCEEDED(ar)) {
            XrMCPToolInfoDXR loadTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
            loadTool.name = "load_model";
            loadTool.description =
                "Load a 3D model file into the viewer, replacing the current model. "
                "Supported formats: glTF (.glb/.gltf), STL, OBJ, FBX, USD "
                "(.usdz/.usd/.usda/.usdc). The path must be absolute and readable by "
                "the viewer process. On success the camera re-frames the model "
                "automatically, and the animation tools (list_animations / "
                "play_animation / stop_animation) appear or disappear depending on "
                "whether the new model has animation clips.";
            loadTool.inputSchemaJson =
                "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
                "\"description\":\"Absolute filesystem path of the model file to load.\"}},"
                "\"required\":[\"path\"]}";
            XrResult t1 = xr.pfnRegisterMCPTool(xr.session, &loadTool);

            XrMCPToolInfoDXR statusTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
            statusTool.name = "get_status";
            statusTool.description =
                "Read the viewer's live state: loaded model file and primitive count, "
                "animation clip count + active clip + playing flag, camera orbit "
                "(azimuth/elevation in degrees, world position, zoom factor), active "
                "rendering-mode index, and whether the XR session is running.";
            statusTool.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult t2 = xr.pfnRegisterMCPTool(xr.session, &statusTool);

            XrMCPToolInfoDXR orbitTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
            orbitTool.name = "set_orbit";
            orbitTool.description =
                "Orbit the camera around the model. azimuth_deg rotates around the "
                "vertical axis (0 = the model's authored front, increasing turns the "
                "model to the right), elevation_deg tilts the view up/down (clamped to "
                "±85), zoom scales the model on screen (>1 = larger, clamped 0.1–10, "
                "default 1). All fields are optional; omitted ones keep their current "
                "value. Also resets the idle auto-orbit timer, like any user input. "
                "Verify the result visually with capture_frame.";
            orbitTool.inputSchemaJson =
                "{\"type\":\"object\",\"properties\":{"
                "\"azimuth_deg\":{\"type\":\"number\",\"description\":\"Orbit angle around the vertical axis, degrees.\"},"
                "\"elevation_deg\":{\"type\":\"number\",\"description\":\"Tilt above (+) / below (−) the horizon, degrees, clamped to ±85.\"},"
                "\"zoom\":{\"type\":\"number\",\"description\":\"View scale factor, 0.1–10; 1 = the auto-fit framing.\"}}}";
            XrResult t3 = xr.pfnRegisterMCPTool(xr.session, &orbitTool);

            XrMCPToolInfoDXR frameTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
            frameTool.name = "frame_model";
            frameTool.description =
                "Reset the camera to the loaded model's auto-fit framed pose (same as "
                "pressing Space): centers the model with comfortable headroom and "
                "restores zoom to 1. Requires a model to be loaded.";
            frameTool.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult t4 = xr.pfnRegisterMCPTool(xr.session, &frameTool);

            xr.mcpToolsReady = true;
            LOG_INFO("XR_DXR_mcp_tools: appId=modelviewer load_model=%d get_status=%d "
                     "set_orbit=%d frame_model=%d", t1, t2, t3, t4);
        } else {
            LOG_INFO("XR_DXR_mcp_tools: appId not accepted (%d) — no agent surface", ar);
        }
    }

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeIsRequestable[i] = (modes[i].isRequestable == XR_TRUE);
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
                    // Only a 3D mode is a candidate for the app's default. The
                    // display commonly reports its 2D mode active at startup
                    // (it stays 2D until something asks for 3D), and adopting
                    // that would open this 3D demo in mono.
                    if (modes[i].isActive == XR_TRUE && modes[i].hardwareDisplay3D == XR_TRUE)
                        xr.activeRenderingMode = (int32_t)i;
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, tiles=%ux%u, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY,
                        xr.renderingModeTileColumns[i], xr.renderingModeTileRows[i],
                        modes[i].hardwareDisplay3D);
                }
            }
        }
    }

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));

    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, views.data());
    xr.maxViewCount = viewCount;
    LOG_INFO("View config: %u views reported by runtime", viewCount);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());

    int64_t selectedFmt = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0];
    for (auto f : fmts) {
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
    }

    // Size the swapchain at init from the largest atlas any rendering mode
    // could produce when the app is running full-screen — atlas dims per
    // mode are (cols × scaleX × displayPixelW) × (rows × scaleY × displayPixelH).
    // For sim_display and Leia SR this collapses to the panel resolution
    // (max(cols × scaleX) ≤ 1 across all their advertised modes). The atlas
    // the app actually writes per frame is smaller — driven by the live
    // window size — but the swapchain has to accommodate full-screen so the
    // app can resize / fullscreen at any time without reallocating. Falls
    // back to recommended × (2,1) if display info is unavailable.
    uint32_t w = views[0].recommendedImageRectWidth * 2;
    uint32_t h = views[0].recommendedImageRectHeight;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        w = xr.displayPixelWidth;
        h = xr.displayPixelHeight;
        if (xr.renderingModeCount > 0) {
            uint32_t maxAtlasW = 0, maxAtlasH = 0;
            for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] *
                                          xr.renderingModeScaleX[i] *
                                          (double)xr.displayPixelWidth);
                uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] *
                                          xr.renderingModeScaleY[i] *
                                          (double)xr.displayPixelHeight);
                if (aw > maxAtlasW) maxAtlasW = aw;
                if (ah > maxAtlasH) maxAtlasH = ah;
            }
            if (maxAtlasW > w) w = maxAtlasW;
            if (maxAtlasH > h) h = maxAtlasH;
        }
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;

    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, selectedFmt);
    return true;
}

// ============================================================================
// XR_DXR_mcp_tools dispatch (#22)
// ============================================================================
// Minimal JSON helpers — hand-rolled on purpose, matching the runtime
// reference adopter (cube_handle_metal_macos): tool args are tiny one-level
// objects, so a JSON dependency isn't warranted.

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract "key":"value" (string) with backslash-escape handling, incl. a
// basic \uXXXX → UTF-8 decode (no surrogate pairs — file paths don't need
// them). False when the key is absent or its value is not a string.
static bool JsonGetString(const char* json, const char* key, std::string& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (*c != '"') return false;
    c++;
    out.clear();
    while (*c && *c != '"') {
        if (*c == '\\' && c[1]) {
            c++;
            switch (*c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    int ndig = 0;
                    while (ndig < 4 && c[1]) {
                        char h = c[1];
                        unsigned v;
                        if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
                        else break;
                        cp = (cp << 4) | v;
                        c++;
                        ndig++;
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += *c; break;  // \" \\ \/
            }
        } else {
            out += *c;
        }
        c++;
    }
    return *c == '"';
}

// Extract "key": <number>. False when absent or not numeric (strtod refuses
// a leading quote, so string values correctly fail).
static bool JsonGetNumber(const char* json, const char* key, double& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    char* end = nullptr;
    double v = strtod(c + 1, &end);
    if (end == c + 1) return false;
    out = v;
    return true;
}

// Dispatch one agent tool call. Runs on the main loop (called from
// PollEvents), where app state is naturally consistent — no locking. EVERY
// call is answered — success=XR_FALSE + {"error":…} for bad args — because an
// unanswered call only fails to the agent after the runtime's ~5 s timeout.
static void HandleMcpToolCall(AppXrSession& xr, const XrEventDataMCPToolCallDXR* call) {
    // Two-call idiom: argsSize from the event is the required capacity incl. NUL.
    std::string args;
    if (xr.pfnGetMCPToolCallArgs && call->argsSize > 0) {
        std::vector<char> buf(call->argsSize, '\0');
        uint32_t needed = 0;
        if (XR_SUCCEEDED(xr.pfnGetMCPToolCallArgs(xr.session, call->callId,
                                                  (uint32_t)buf.size(), &needed, buf.data())))
            args.assign(buf.data());
    }
    const char* a = args.c_str();
    std::string result;
    XrBool32 ok = XR_TRUE;
    char buf[1024];

    if (strcmp(call->toolName, "load_model") == 0) {
        std::string path;
        if (!JsonGetString(a, "path", path) || path.empty()) {
            ok = XR_FALSE;
            result = "{\"error\":\"missing required string argument 'path'\"}";
        } else if (!model_validate_file(path)) {
            ok = XR_FALSE;
            result = "{\"error\":\"not a readable supported model file: " +
                     JsonEscape(path) + "\"}";
        } else if (!g_modelRenderer.loadModel(path.c_str())) {
            ok = XR_FALSE;
            result = "{\"error\":\"failed to load (corrupt or unsupported): " +
                     JsonEscape(path) + "\"}";
        } else {
            g_loadedFileName = model_basename(path);
            LOG_INFO("Model loaded via MCP: %s (%s)", g_loadedFileName.c_str(),
                     model_filesize_str(path).c_str());
            // Re-frames the camera, refreshes the clip button, and registers/
            // unregisters the agent animation tools for the new model.
            ApplyAutoFitForLoadedScene();
            snprintf(buf, sizeof(buf),
                     "{\"file\":\"%s\",\"primitives\":%u,\"animation_count\":%d}",
                     JsonEscape(g_loadedFileName).c_str(),
                     g_modelRenderer.primitiveCount(), g_modelRenderer.animationCount());
            result = buf;
        }
    } else if (strcmp(call->toolName, "get_status") == 0) {
        std::string clip; int ci = -1, cn = 0; float ct = 0, cd = 0; bool playing = false;
        const bool hasClip = g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing);
        const float azDeg = fmodf(g_input.yaw * 57.29578f, 360.0f);
        const float elDeg = g_input.pitch * 57.29578f;
        std::string clipJson = hasClip ? "\"" + JsonEscape(clip) + "\"" : "null";
        snprintf(buf, sizeof(buf),
                 "{\"file\":\"%s\",\"loaded\":%s,\"primitives\":%u,"
                 "\"animation_count\":%d,\"active_animation\":%d,"
                 "\"active_animation_name\":%s,\"animation_playing\":%s,"
                 "\"camera\":{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,"
                 "\"position\":[%.3f,%.3f,%.3f],\"zoom\":%.2f},"
                 "\"rendering_mode\":%u,\"session_running\":%s}",
                 JsonEscape(g_loadedFileName).c_str(),
                 g_modelRenderer.hasModel() ? "true" : "false",
                 g_modelRenderer.primitiveCount(),
                 g_modelRenderer.animationCount(),
                 hasClip ? ci : -1, clipJson.c_str(),
                 (hasClip && playing) ? "true" : "false",
                 azDeg, elDeg,
                 g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                 g_input.viewParams.scaleFactor,
                 g_input.currentRenderingMode,
                 xr.sessionRunning ? "true" : "false");
        result = buf;
    } else if (strcmp(call->toolName, "set_orbit") == 0) {
        double az, el, zm;
        bool any = false;
        if (JsonGetNumber(a, "azimuth_deg", az)) {
            g_input.yaw = (float)(az * 0.0174532925);
            any = true;
        }
        if (JsonGetNumber(a, "elevation_deg", el)) {
            if (el > 85.0) el = 85.0;
            if (el < -85.0) el = -85.0;
            g_input.pitch = (float)(el * 0.0174532925);
            any = true;
        }
        if (JsonGetNumber(a, "zoom", zm)) {
            if (zm < 0.1) zm = 0.1;
            if (zm > 10.0) zm = 10.0;
            g_input.viewParams.scaleFactor = (float)zm;
            any = true;
        }
        if (!any) {
            ok = XR_FALSE;
            result = "{\"error\":\"provide at least one of azimuth_deg, elevation_deg, zoom\"}";
        } else {
            MarkUserInput(g_input);  // agent input is input: reset the auto-orbit idle timer
            snprintf(buf, sizeof(buf),
                     "{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,\"zoom\":%.2f}",
                     g_input.yaw * 57.29578f, g_input.pitch * 57.29578f,
                     g_input.viewParams.scaleFactor);
            result = buf;
        }
    } else if (strcmp(call->toolName, "frame_model") == 0) {
        if (!g_modelRenderer.hasModel()) {
            ok = XR_FALSE;
            result = "{\"error\":\"no model loaded — call load_model first\"}";
        } else {
            g_input.resetViewRequested = true;  // applied by UpdateCameraMovement next frame
            result = "{\"framed\":true}";
        }
    } else if (strcmp(call->toolName, "list_animations") == 0) {
        const int n = g_modelRenderer.animationCount();
        std::string clips = "[";
        for (int i = 0; i < n; i++) {
            std::string nm; float dur = 0;
            g_modelRenderer.getAnimationInfo(i, nm, dur);
            snprintf(buf, sizeof(buf), "%s{\"index\":%d,\"name\":\"%s\",\"duration_s\":%.2f}",
                     i ? "," : "", i, JsonEscape(nm).c_str(), dur);
            clips += buf;
        }
        clips += "]";
        snprintf(buf, sizeof(buf), ",\"active_index\":%d,\"playing\":%s}",
                 g_modelRenderer.activeAnimation(),
                 (g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused()) ? "true" : "false");
        result = "{\"animations\":" + clips + buf;
    } else if (strcmp(call->toolName, "play_animation") == 0) {
        const int n = g_modelRenderer.animationCount();
        int target = -1;
        double idx; std::string nm;
        if (JsonGetNumber(a, "index", idx)) {
            target = (int)idx;
            if (target < 0 || target >= n) {
                ok = XR_FALSE;
                snprintf(buf, sizeof(buf), "{\"error\":\"index out of range (0..%d)\"}", n - 1);
                result = buf;
            }
        } else if (JsonGetString(a, "name", nm)) {
            for (int i = 0; i < n && target < 0; i++) {
                std::string c; float d;
                g_modelRenderer.getAnimationInfo(i, c, d);
                if (c == nm) target = i;
            }
            if (target < 0) {
                ok = XR_FALSE;
                result = "{\"error\":\"no clip named '" + JsonEscape(nm) +
                         "' — see list_animations\"}";
            }
        } else {
            target = g_modelRenderer.activeAnimation();  // resume the active clip
            if (target < 0) target = 0;
        }
        if (ok == XR_TRUE) {
            if (n == 0) {
                ok = XR_FALSE;
                result = "{\"error\":\"the loaded model has no animation clips\"}";
            } else {
                if (target != g_modelRenderer.activeAnimation())
                    g_modelRenderer.setActiveAnimation(target);
                g_modelRenderer.setPaused(false);
                UpdateAnimButton();
                std::string c; float d = 0;
                g_modelRenderer.getAnimationInfo(target, c, d);
                snprintf(buf, sizeof(buf),
                         "{\"playing\":\"%s\",\"index\":%d,\"duration_s\":%.2f}",
                         JsonEscape(c).c_str(), target, d);
                result = buf;
            }
        }
    } else if (strcmp(call->toolName, "stop_animation") == 0) {
        g_modelRenderer.setPaused(true);
        UpdateAnimButton();
        std::string c; float d = 0;
        const int active = g_modelRenderer.activeAnimation();
        if (active >= 0) g_modelRenderer.getAnimationInfo(active, c, d);
        snprintf(buf, sizeof(buf), "{\"playing\":false,\"paused_clip\":%s%s%s}",
                 active >= 0 ? "\"" : "", active >= 0 ? JsonEscape(c).c_str() : "null",
                 active >= 0 ? "\"" : "");
        result = buf;
    } else {
        ok = XR_FALSE;
        result = "{\"error\":\"unhandled tool\"}";
    }

    if (xr.pfnSubmitMCPToolResult)
        xr.pfnSubmitMCPToolResult(xr.session, call->callId, ok, result.c_str());
}

static void PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = ssc->state;
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = xr.viewConfigType;
                xrBeginSession(xr.session, &bi);
                xr.sessionRunning = true;
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING) {
                xr.exitRequested = true;
            }
        } else if (event.type == (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR) {
            // Runtime (or another client / shell) switched rendering mode on us.
            auto* rmc = (XrEventDataRenderingModeChangedDXR*)&event;
            if (rmc->currentModeIndex < xr.renderingModeCount) {
                g_input.currentRenderingMode = rmc->currentModeIndex;
                g_msLastMode = rmc->currentModeIndex; // keep the ramp's from-mode in sync
                UpdateTopBarButtonTitles(xr);
                LOG_INFO("Rendering mode changed: %u -> %u (%s)",
                    rmc->previousModeIndex, rmc->currentModeIndex,
                    xr.renderingModeNames[rmc->currentModeIndex]);
            }
        } else if (event.type == (XrStructureType)XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_DXR) {
            // An agent invoked one of our XR_DXR_mcp_tools tools (#22).
            HandleMcpToolCall(xr, (const XrEventDataMCPToolCallDXR*)&event);
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(xr.session, nullptr, &fs);
    if (XR_FAILED(r)) return false;
    return XR_SUCCEEDED(xrBeginFrame(xr.session, nullptr));
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 1000000000;
    return XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &wi));
}

static void ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
}

static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount,
    const void* frameEndNext) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.next = frameEndNext;   // XR_DXR_depth_budget ROI (content mask / bounds)
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = 1; ei.layers = layers;
    xrEndFrame(xr.session, &ei);
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.localSpace) xrDestroySpace(xr.localSpace);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
}

/*!
 * True when NOT under a workspace controller: no modes enumerated (pre-v7
 * runtime) or the active mode is app-requestable (a controller marks them
 * non-requestable). Gates the no-budget ZDP clip and the transparent half of
 * the auto-orbit hold — the Windows/Linux IsStandaloneSession.
 */
static bool IsStandaloneSession(const AppXrSession& xr) {
    return (xr.renderingModeCount == 0) ||
           (g_input.currentRenderingMode < xr.renderingModeCount &&
            xr.renderingModeIsRequestable[g_input.currentRenderingMode]);
}

/*!
 * Log the rear depth budget on change only — never per frame (linux/main.cpp's
 * LogRearDepthBudget). Prints on every STATE change and once the ramped value
 * has settled; `applied` is what dxr::ResolveClipPlanes used (1000 whenever
 * the frame is opaque).
 */
static void LogRearDepthBudget(const XrRearDepthBudgetDXR* budget, float applied, bool transparent,
                               bool standalone) {
    static bool s_haveLogged = false;
    static bool s_lastHadBudget = false;
    static int s_lastState = -1;
    static float s_lastLoggedVH = -1.0f, s_lastLoggedApplied = -1.0f, s_prevVH = -1.0f;
    static bool s_lastTransparent = false;
    static float s_lastFallbackApplied = -1.0f;

    if (budget == nullptr) {
        if (!s_haveLogged || s_lastHadBudget || applied != s_lastFallbackApplied) {
            s_haveLogged = true;
            s_lastHadBudget = false;
            s_lastFallbackApplied = applied;
            LOG_INFO("Rear depth budget: none from the runtime — fallback rule, applied farOffsetVH=%.0f "
                     "(transparent=%d standalone=%d)", applied, transparent ? 1 : 0, standalone ? 1 : 0);
        }
        return;
    }
    const float vh = budget->farOffsetVH;
    const bool stateChanged = !s_lastHadBudget || (int)budget->state != s_lastState;
    const bool settled = (vh == s_prevVH) && (vh != s_lastLoggedVH || applied != s_lastLoggedApplied);
    const bool transparentChanged = transparent != s_lastTransparent;
    s_prevVH = vh;
    if (!stateChanged && !settled && !transparentChanged) return;
    s_haveLogged = true;
    s_lastHadBudget = true;
    s_lastState = (int)budget->state;
    s_lastLoggedVH = vh;
    s_lastLoggedApplied = applied;
    s_lastTransparent = transparent;
    LOG_INFO("Rear depth budget: state=%s farOffsetVH=%.2f (%.3f m) -> applied farOffsetVH=%.2f%s "
             "(transparent=%d standalone=%d)",
             dxr::RearDepthBudgetStateName(budget->state), vh, budget->farOffsetMeters, applied,
             (transparent && applied < 1000.0f) ? " [rear clip ON]" : " [no rear clip]",
             transparent ? 1 : 0, standalone ? 1 : 0);
}

// ============================================================================
// Placeholder rendering (clear to dark gray when no scene loaded)
// ============================================================================

static void RenderPlaceholder(VkDevice dev, VkQueue queue, VkCommandPool pool,
                               VkImage image, uint32_t w, uint32_t h,
                               float yaw, float pitch, bool transparentBg) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Tint color based on camera direction so drag-rotation gives visual feedback
    float ny = (yaw / 3.14159f) * 0.5f + 0.5f;   // 0..1 over ±π
    float np = (pitch / 1.5f) * 0.5f + 0.5f;       // 0..1 over ±1.5 rad
    // Opaque: alpha exactly 1 everywhere. Transparent (#181): alpha 0, so an
    // empty viewer is see-through rather than a tinted slab on the desktop.
    VkClearColorValue cc = transparentBg
        ? VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}
        : VkClearColorValue{{0.05f + ny * 0.15f, 0.08f + np * 0.12f, 0.15f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

// ============================================================================
// Bundled-scene auto-load
// ============================================================================

static std::string ExeDir() {
    char buf[PATH_MAX]; uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return "";
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved)) return std::string(buf);
    return std::string(dirname(resolved));
}

static bool FileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Compute robust scene bounds (5th–95th percentile per axis) and set the
// display rig pose + vHeight to frame the model. Display orientation is
// kept identity (forward = world −Z): a model's canonical front is its
// authored +Z, which glTF places facing the viewer; the user can rotate
// with mouse drag from a predictable starting pose.
static void ApplyAutoFitForLoadedScene() {
    UpdateAnimButton();        // show/hide + label the clip button for the new model
    UpdateMcpAnimationTools(); // (un)register the agent animation tools (#22)
    float center[3], extent[3];
    // Robust AABB (5th–95th percentile per axis): center for the rig position,
    // extent[0]/extent[1] (FULL sizes, not half-extents) for the width-aware
    // fit. Percentile trim rejects stray outlier vertices that would otherwise
    // inflate the frame.
    if (g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent)) {
        g_fitCenter[0] = center[0];
        g_fitCenter[1] = center[1];
        g_fitCenter[2] = center[2];
        // Viewport the fit frames against: the demo's display zone is the full
        // window, so that's the content view. Only the aspect matters, so the
        // backing scale factor is irrelevant here. Falls back to the cached
        // drawable dims before the window exists; (0,0) is legal and degrades
        // dxr::AutoFitVHeight to the height-only fit.
        float viewportW = (float)g_windowW, viewportH = (float)g_windowH;
        if (g_window) {
            NSSize cs = [[g_window contentView] bounds].size;
            if (cs.width > 0.0 && cs.height > 0.0) {
                viewportW = (float)cs.width;
                viewportH = (float)cs.height;
            }
        }
        float sweptW = 0.0f;
        float vh = modelviewer::FitVHeight(extent, viewportW, viewportH, &sweptW);
        if (!(vh > 1e-3f)) vh = kDefaultVirtualDisplayHeightM; // degenerate scene
        if (g_vhOverride > 0.0f) vh = g_vhOverride;             // --vh pins it
        g_fitVHeight = vh;
        // Full extents for the XR_DXR_depth_budget v2 content bounds.
        g_fitExtentW = sweptW > 0.0f ? sweptW : hypotf(extent[0], extent[2]);
        g_fitExtentH = extent[1];
        g_fitExtentD = extent[2];

        // glTF models are authored front-facing (+Z toward the viewer), so a
        // yaw scan isn't needed — start at yaw=0 and let the user rotate.
        g_fitYaw = 0.0f;

        g_fitValid = true;
        // Which axis bound the fit: width wins when the model is wider than
        // the viewport aspect can hold at the height-only vHeight.
        const bool haveViewport = (viewportW > 0.0f && viewportH > 0.0f);
        const float aspect = haveViewport ? (viewportW / viewportH) : 0.0f;
        const char *boundBy = !haveViewport
                            ? "height (no viewport)"
                            : modelviewer::FitBoundBy(sweptW, extent[1], extent[2], aspect);
        LOG_INFO("Auto-fit: center=(%.3f, %.3f, %.3f) extent W=%.3f H=%.3f D=%.3f swept-W=%.3f "
                 "viewport=%.0fx%.0f (aspect %.3f) bound-by=%s fill=%.0f%% vHeight=%.3f yaw=%.0fdeg",
                 center[0], center[1], center[2],
                 extent[0], extent[1], extent[2], sweptW,
                 viewportW, viewportH, aspect, boundBy,
                 dxr::kAutoFitDefaultFill * 100.0f, vh, g_fitYaw * 57.2957795f);
    } else {
        g_fitValid = false;
    }

    g_input.cameraPosX = g_fitValid ? g_fitCenter[0] : 0.0f;
    g_input.cameraPosY = g_fitValid ? g_fitCenter[1] : 0.0f;
    g_input.cameraPosZ = g_fitValid ? g_fitCenter[2] : 0.0f;
    g_input.yaw = g_fitValid ? g_fitYaw : 0.0f;
    g_input.pitch = 0.0f;
    g_input.viewParams.virtualDisplayHeight = g_fitValid ? g_fitVHeight : kDefaultVirtualDisplayHeightM;
    g_input.viewParams.scaleFactor = 1.0f;

    // DXR_MODELVIEWER_CAMERA=<name|index> starts AT one of the file's cameras
    // (USD GeomCamera) instead of the auto-fit -- the viewpoint of the file's own
    // reference renders. The rig is display-centric, so the camera becomes a
    // virtual display focusDistance down its view axis, sized so the physical
    // viewer (nominalViewerZ in front of a displayHeight panel) scales onto the
    // camera's position: vHeight = focusDistance * displayHeight / nominalViewerZ.
    // Roll is not representable (the rig has yaw + pitch) and the field of view
    // is the panel's -- from the same eye, a crop of the authored frame.
    if (const char* want = getenv("DXR_MODELVIEWER_CAMERA"); want && want[0]) {
        const auto& cams = g_modelRenderer.sceneCameras();
        const ModelCamera* pick = nullptr;
        for (const auto& c : cams) if (c.name == want) pick = &c;
        if (!pick) { char* e = nullptr; long i = strtol(want, &e, 10);
                     if (e && !*e && i >= 0 && (size_t)i < cams.size()) pick = &cams[(size_t)i]; }
        if (pick && g_displayHeightM > 0.0f && g_input.nominalViewerZ > 0.0f) {
            const float D = pick->focusDistance;
            g_input.cameraPosX = pick->position[0] + pick->forward[0] * D;
            g_input.cameraPosY = pick->position[1] + pick->forward[1] * D;
            g_input.cameraPosZ = pick->position[2] + pick->forward[2] * D;
            g_input.yaw   = atan2f(-pick->forward[0], -pick->forward[2]);
            g_input.pitch = asinf(fmaxf(-1.0f, fminf(1.0f, pick->forward[1])));
            g_input.viewParams.virtualDisplayHeight = D * g_displayHeightM / g_input.nominalViewerZ;
            g_input.animateEnabled = false;   // hold the authored viewpoint
            LOG_INFO("Scene camera '%s': eye (%.3f, %.3f, %.3f) focus %.3f m vHeight %.3f",
                     pick->name.c_str(), pick->position[0], pick->position[1], pick->position[2],
                     D, g_input.viewParams.virtualDisplayHeight);
        } else {
            LOG_WARN("DXR_MODELVIEWER_CAMERA='%s': no such camera (%zu in file)", want, cams.size());
        }
    }
    // Treat scene load as a fresh user interaction so the auto-orbit idle
    // timer restarts. Without this, an asset loaded after the 10s idle
    // threshold starts rotating immediately on first display.
    MarkUserInput(g_input);

    // glTF uses a +Y-up, right-handed coordinate system natively.
    // ModelRenderer::updateUniforms negates the Y row of proj_mat to match the
    // +Y-up convention. No runtime view-stage flips needed.
}

// A bundled environment.hdr, when present next to the exe, becomes the IBL
// source at startup (issue #70). Absent, the viewer keeps the procedural
// analytic sky — so this is purely additive and the demo still runs with no
// environment asset at all. Either way the HUD names the active environment,
// which is what makes a reference capture self-documenting.
// Deterministic-capture switch (issue #70 phase 3). Reference renders are only
// comparable if nothing moves between them, and this viewer's idle auto-orbit
// starts rotating the scene ~10 s after the last input — long enough that a
// scripted "launch, wait, capture" sequence lands at an unpredictable angle.
// Every regression measurement taken during phases 0-2 needed auto-orbit
// disabled by hand-editing the source, which is not a workflow anyone should
// inherit. DXR_MODELVIEWER_DETERMINISTIC=1 pins it off at startup.
//
// An environment variable rather than a CLI flag because the macOS build is an
// .app bundle with no argv to parse, and a capture harness has to be able to set
// this identically on every platform.
static bool DeterministicCaptureRequested() {
    const char* v = getenv("DXR_MODELVIEWER_DETERMINISTIC");
    return v && v[0] && v[0] != '0';
}

static void TryAutoLoadBundledEnvironment() {
    std::string dir = ExeDir();
    if (dir.empty()) return;
    std::string path = dir + "/environment.hdr";
    if (!FileExists(path)) {
        LOG_INFO("No bundled environment.hdr (using the procedural analytic sky)");
        return;
    }
    if (g_modelRenderer.setEnvironment(path.c_str())) {
        LOG_INFO("Environment: %s", g_modelRenderer.environmentName().c_str());
    }
}

// Positional model path from the launch contract (dxr::ParseLaunchArgs).
static std::string g_launchModelPath;

static void TryAutoLoadBundledScene() {
    std::string dir = ExeDir();
    if (dir.empty()) return;
    std::string path = dir + "/sample.glb";
    if (!g_launchModelPath.empty()) path = g_launchModelPath;
    // DXR_MODELVIEWER_MODEL=<path> replaces the bundled sample at startup, any
    // format the loader takes (.glb/.gltf/.usd*/.obj/.fbx/.stl). An environment
    // variable for the same reason as DXR_MODELVIEWER_DETERMINISTIC: the .app
    // has no argv, and a capture harness must not have to overwrite sample.glb.
    if (const char* m = getenv("DXR_MODELVIEWER_MODEL"); m && m[0]) path = m;
    if (!FileExists(path)) {
        LOG_INFO("No bundled model at %s (skipping auto-load)", path.c_str());
        return;
    }
    if (!model_validate_file(path)) return;
    LOG_INFO("Auto-loading bundled model: %s", path.c_str());
    if (g_modelRenderer.loadModel(path.c_str())) {
        g_loadedFileName = model_basename(path);
        LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(), model_filesize_str(path).c_str());
        ApplyAutoFitForLoadedScene();
    } else {
        LOG_WARN("Auto-load failed for %s", path.c_str());
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGUSR1, ToggleSignalHandler);   // scripted Ctrl+T (see ToggleSignalHandler)

    LOG_INFO("=== DisplayXR 3D Model Viewer (Vulkan) + External macOS Window ===");

    // Launch contract (shared with Windows/Linux, dxr::ParseLaunchArgs): the
    // first positional argument is a model path, --vh=<metres> pins the
    // virtual display height, --transparent starts transparent. The rest of
    // the undock contract (--rect, --pose, --margin, --src, --title) is parsed
    // and reported as unsupported on macOS rather than silently ignored.
    // Finder's legacy -psn_<n> process-serial argument is dropped first.
    bool launchTransparent = false;
    {
        std::vector<std::string> args;
        for (int i = 1; i < argc; i++) {
            if (strncmp(argv[i], "-psn_", 5) == 0) continue;
            args.emplace_back(argv[i]);
        }
        if (!args.empty()) {
            const dxr::LaunchArgs la = dxr::ParseLaunchArgs(args);
            for (const std::string& w : la.warnings) LOG_WARN("launch: %s", w.c_str());
            if (!la.ok()) {
                for (const std::string& e : la.errors) LOG_ERROR("launch: %s", e.c_str());
                return 1;
            }
            if (la.hasVh && la.vh > 0.0f) { g_vhOverride = la.vh; LOG_INFO("launch: --vh=%.4f m", la.vh); }
            if (!la.positionalPath.empty()) g_launchModelPath = la.positionalPath;
            if (la.srcKind == dxr::LaunchSrcKind::LocalPath && !la.src.empty()) g_launchModelPath = la.src;
            if (la.transparent) { launchTransparent = true; LOG_INFO("launch: --transparent"); }
            if (la.srcKind == dxr::LaunchSrcKind::Url) LOG_WARN("launch: --src URL download is not supported on macOS (ignored)");
            if (la.hasRect) LOG_WARN("launch: --rect is not supported on macOS (ignored)");
            if (la.hasPose) LOG_WARN("launch: --pose is not supported on macOS (ignored)");
            if (la.hasMargin) LOG_WARN("launch: --margin is not supported on macOS (ignored)");
            if (!la.title.empty()) LOG_WARN("launch: --title is not supported on macOS (ignored)");
        }
    }

    // MODEL_TRANSPARENT (Linux parity; #181):
    //   unset / empty  transparent-capable, start opaque, Ctrl/Cmd+T toggles
    //   1 (non-zero)   transparent-capable, start transparent (= --transparent)
    //   0              NOT capable: opaque layer, transparentBackgroundEnabled =
    //                  XR_FALSE — the pre-#181 behaviour; Ctrl+T refuses.
    {
        const char* te = getenv("MODEL_TRANSPARENT");
        if (te != nullptr && te[0] == '0') {
            g_transparentCapable = false;
            if (launchTransparent) LOG_WARN("launch: --transparent overridden by MODEL_TRANSPARENT=0");
            LOG_INFO("MODEL_TRANSPARENT=0 — transparent background disabled for this session");
        } else if ((te != nullptr && te[0] != '\0') || launchTransparent) {
            g_transparentBg = true;
        }
        const char* ap = getenv("DXR_MODELVIEWER_ALPHA_PROBE");
        g_alphaProbe = ap != nullptr && ap[0] != '\0' && ap[0] != '0';
    }

    // Initialize rendering mode from env var (legacy fallback)
    {
        const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
        if (mode_str) {
            // "2d" selects the single-view mode. It used to fall through to the
            // anaglyph default, so there was NO value of this variable that could ask
            // for one view — which matters for measurement: a material capture wants a
            // single un-composited tile, and anaglyph combines the views per colour
            // channel, i.e. it destroys exactly the thing being measured.
            if (strcmp(mode_str, "2d") == 0) g_input.currentRenderingMode = 0;
            else if (strcmp(mode_str, "anaglyph") == 0) g_input.currentRenderingMode = 1;
            else if (strcmp(mode_str, "sbs") == 0) g_input.currentRenderingMode = 2;
            else if (strcmp(mode_str, "blend") == 0) g_input.currentRenderingMode = 3;
            else g_input.currentRenderingMode = 1; // unrecognised → the first 3D mode
        }
    }

    // Step 1: Initialize OpenXR — BEFORE the window, so it can be created
    // directly on the 3D panel (INV-1.3, XrDisplayDesktopPositionDXR,
    // runtime#715). Nothing in the OpenXR / Vulkan init needs the NSWindow
    // until CreateSession (same ordering as cube_handle_metal_macos).
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // Step 2: Create macOS window at the panel's desktop position
    g_windowW = 1280; g_windowH = 720;
    if (!CreateMacOSWindow(g_windowW, g_windowH,
                           xr.displayScreenLeft, xr.displayScreenTop)) {
        LOG_ERROR("Failed to create macOS window");
        CleanupOpenXR(xr);
        return 1;
    }

    { NSSize cs = [[g_window contentView] bounds].size;
      CGFloat bs = [g_window backingScaleFactor];
      g_windowW = (uint32_t)(cs.width * bs);
      g_windowH = (uint32_t)(cs.height * bs);
      LOG_INFO("Window drawable: %ux%u", g_windowW, g_windowH); }

    // Try to find sim_display_set_output_mode
    { void *rtHandle = NULL;
      uint32_t ic = _dyld_image_count();
      for (uint32_t i = 0; i < ic; i++) {
          const char *name = _dyld_get_image_name(i);
          if (name && strstr(name, "openxr_displayxr")) {
              rtHandle = dlopen(name, RTLD_NOLOAD); break;
          }
      }
      if (rtHandle) g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)dlsym(rtHandle, "sim_display_set_output_mode");
      LOG_INFO("sim_display hot-reload: %s", g_pfnSetOutputMode ? "available" : "not available"); }

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }
    // Window side of the starting background (level + shadow), now that the
    // session has settled whether the capability survived.
    ApplyTransparentWindowState();
    LOG_INFO("Window: opaque=%d layer.opaque=%d level=%ld (capable=%d)",
             g_window.isOpaque ? 1 : 0, g_metalView.layer.opaque ? 1 : 0, (long)g_window.level,
             g_transparentCapable ? 1 : 0);
    if (g_transparentBg) LOG_INFO("Transparent background: ON (startup)");

    // Adopt the display's active mode rather than assuming a view count, so a
    // 4-view Quad display renders a 4-tile atlas instead of a hard-coded 2.
    // Gated on the mode being 3D: sim_display (and a real panel) typically
    // reports its 2D mode active at startup, and adopting that verbatim opened
    // this 3D demo in mono — measured as a 1-tile atlas where the previous
    // build produced 2. When nothing 3D is active the earlier default (first 3D
    // mode) stands. Runtime/controller changes are tracked afterwards via
    // XrEventDataRenderingModeChangedDXR (handled below).
    if (xr.activeRenderingMode >= 0)
        g_input.currentRenderingMode = (uint32_t)xr.activeRenderingMode;

    // Model-load paths can now flip the agent animation-tool registration
    // (XR_DXR_mcp_tools late registration, #22). Set before the bundled-scene
    // auto-load below so it too funnels through UpdateMcpAnimationTools.
    g_xrForMcp = &xr;

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    // Enumerate swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    { uint32_t count = xr.swapchain.imageCount;
      swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    // Initialize model renderer
    { uint32_t rw = xr.swapchain.width;   // Full width — mono uses entire swapchain
      uint32_t rh = xr.swapchain.height;
      if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue, queueFamilyIndex, rw, rh))
          LOG_WARN("model renderer init failed");
    }

    // Hand tracking: trackers on the session (inert without a provider), and
    // the hand mesh pipeline against the renderer's own render pass so the
    // overlay hook draws it inside every view's tile after the opaque model.
    g_handTracking.init(xr.instance, xr.systemId, xr.session, xr.hasHandTrackingExt);
    g_handTracking.setViewRigSpecVersion(xr.hasViewRigExt ? xr.viewRigSpecVersion : 0);
    if (g_handTracking.enabled()) {
        modelviewer::HandGestureConfig gcfg;
        gcfg.applyEnvironment();
        g_handGestures.setConfig(gcfg);
        if (g_handRenderer.init(vkDevice, physDevice, g_modelRenderer.renderPass(),
                                g_modelRenderer.sampleCountFlag(),
                                ModelRenderer::overlayColorAttachmentCount())) {
            g_modelRenderer.setOverlayDraw(
                [](VkCommandBuffer cmd, const float view[16], const float proj[16], bool encodeSrgb) {
                    g_handRenderer.draw(cmd, view, proj, encodeSrgb);
                });
        }
    }

    // Command pool for placeholder rendering
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      ci.queueFamilyIndex = queueFamilyIndex;
      vkCreateCommandPool(vkDevice, &ci, nullptr, &cmdPool); }

    g_input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
    g_input.nominalViewerZ = xr.nominalViewerZ;
    g_displayHeightM = xr.displayHeightM;
    g_input.renderingModeCount = xr.renderingModeCount;
    // Align the runtime's active rendering mode with the app's default
    // (currentRenderingMode = 1, the first 3D mode) at startup. The sim display
    // boots in 2D (mode 0); without this the display stays 2D until the user
    // toggles. The main-loop dispatch holds this request until the session is
    // running, so it isn't issued to a not-yet-begun session and lost.
    g_input.renderingModeChangeRequested = true;
    g_input.lastInputTimeSec = NowSec();

    // Reflect initial state in top-bar buttons.
    UpdateTopBarButtonTitles(xr);

    // Environment before model: setEnvironment rebakes the IBL cubes, and doing
    // it first means the model's first rendered frame is already lit correctly.
    if (DeterministicCaptureRequested()) {
        g_input.animateEnabled = false;
        LOG_INFO("Deterministic capture: auto-orbit disabled (DXR_MODELVIEWER_DETERMINISTIC)");
    }

    TryAutoLoadBundledEnvironment();

    // Try loading the bundled sample.glb model (copied next to the exe by CMake).
    TryAutoLoadBundledScene();

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom  DblClick=Focus");
    LOG_INFO("          -/= Depth  Space=Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          [/]=Exposure  G=Tone curve  (drop an .hdr to change the environment)");
    LOG_INFO("          Ctrl+O/Open=Load  Ctrl/Cmd+T=Transparent bg  Tab=HUD  ESC=Quit  (.glb/.gltf also accept drag-and-drop)");
    if (g_handTracking.enabled())
        LOG_INFO("          Hands: pinch+drag=Rotate  two-hand pinch: spread=Zoom twist=Roll drag=Rotate");

    // Depth-aware cursor: the runtime draws it (default look) in front of the
    // model where the model pops out of the glass; the app supplies the depth
    // (ModelRenderer::raycast). Inert when the extension is off / opted out.
    dxr::CursorDepth cursor;
    modelviewer::CursorDepthAdoption cursorAdopt;
    cursorAdopt.setup(cursor, g_modelRenderer, xr.cursorDepthSpecVersion);
    if (!cursorAdopt.scripted()) cursor.setWindowCocoa((__bridge void*)g_metalView);

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PumpMacOSEvents();

        // Ctrl+T / Cmd+T (#181): opaque <-> transparent background.
        if (g_input.transparentBgToggleRequested) {
            g_input.transparentBgToggleRequested = false;
            if (!g_transparentCapable) {
                // Refuse loudly. The session's transparency was fixed at
                // xrCreateSession; clearing to alpha 0 into an OPAQUE layer
                // would draw a black window, not a see-through one.
                LOG_WARN("Ctrl+T ignored — this session is not transparent-capable "
                         "(MODEL_TRANSPARENT=0 or no cocoa window binding). "
                         "Restart without MODEL_TRANSPARENT=0 to enable it.");
                ShowToast("Transparent background unavailable (MODEL_TRANSPARENT=0)");
            } else {
                g_transparentBg = !g_transparentBg;
                LOG_INFO("Transparent background: %s (Ctrl+T)", g_transparentBg ? "ON" : "OFF");
                ApplyTransparentWindowState();
                ShowToast("Background  %s", g_transparentBg ? "TRANSPARENT" : "OPAQUE");
                // Same as the other legs: the turntable restarts its idle
                // countdown rather than snapping on the instant you go opaque.
                g_input.lastInputTimeSec = NowSec();
            }
        }
        UpdateClickThrough();
        UpdateToast();

        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        g_frameCount++;
        g_avgFrameTime = g_avgFrameTime * 0.95 + deltaTime * 0.05;

        // Handle load request (from L key or Open button)
        if (g_input.loadRequested) {
            g_input.loadRequested = false;
            OpenLoadDialog();
        }

        // Handle drag-and-drop load
        if (!g_input.pendingLoadPath.empty()) {
            std::string p = g_input.pendingLoadPath;
            g_input.pendingLoadPath.clear();
            if (model_validate_file(p)) {
                LOG_INFO("Loading dropped model: %s", p.c_str());
                if (g_modelRenderer.loadModel(p.c_str())) {
                    g_loadedFileName = model_basename(p);
                    ApplyAutoFitForLoadedScene();
                }
            }
        }

        // Handle Auto-Orbit toggle (M key or button)
        if (g_input.animateToggleRequested) {
            g_input.animateToggleRequested = false;
            g_input.animateEnabled = !g_input.animateEnabled;
            g_input.lastInputTimeSec = NowSec(); // don't snap-start
            UpdateTopBarButtonTitles(xr);
        }

        UpdateCameraMovement(g_input, deltaTime, xr.displayHeightM);
        // Clip playback (N=next, K=play/pause), applied before the per-frame
        // advance so this frame reflects it.
        if (g_input.cycleClipRequested || g_input.playPauseRequested) {
            if (g_input.cycleClipRequested) { g_input.cycleClipRequested = false; g_modelRenderer.cycleAnimation(); }
            if (g_input.playPauseRequested) { g_input.playPauseRequested = false; g_modelRenderer.togglePaused(); }
            UpdateAnimButton();   // refresh label (clip name ↔ "Paused")
        }
        // Advance node/TRS animation once per frame (no-op for static models).
        g_modelRenderer.updateAnimation(deltaTime);

        // Handle rendering mode change (V=cycle, 0-3=direct, Mode button, or the
        // startup default-mode request) through the dxr::ModeSwitch sequencer:
        // it eases g_input.viewParams.ipdFactor around the switch and fires the
        // request on the right frame. Held until the session is running so the
        // request reaches a begun session (this handler runs before PollEvents).
        if (!g_modeSwitchConfigured) {
            g_modeSwitch.configure(0.18f, dxr::ModeSwitchEasing::SmoothStep);
            g_modeSwitchConfigured = true;
        }
        {
            const float steady = g_input.viewParams.steadyIpdFactor;
            auto vcOf = [&](uint32_t m) -> uint32_t {
                return (m < xr.renderingModeCount && xr.renderingModeViewCounts[m] > 0)
                           ? xr.renderingModeViewCounts[m] : 1;
            };
            if (g_input.renderingModeChangeRequested && xr.sessionRunning &&
                xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
                g_input.renderingModeChangeRequested = false;
                const uint32_t target = g_input.currentRenderingMode;
                if (target == g_msLastMode) {
                    // Startup / re-assert / same mode: fire directly, no ramp.
                    xr.pfnRequestDisplayRenderingModeEXT(xr.session, target);
                    g_msLastMode = target;
                    UpdateTopBarButtonTitles(xr);
                } else {
                    // currentIpd = on-screen disparity now (last ramp output, else
                    // steady) so the first switch doesn't snap from full disparity.
                    const float curIpd = g_modeSwitch.active() ? g_modeSwitch.ipd() : steady;
                    g_modeSwitch.request(target, vcOf(target), g_msLastMode, vcOf(g_msLastMode),
                                         curIpd, steady);
                }
            }
            if (g_modeSwitch.active()) {
                float ipd = steady; bool fire = false; uint32_t mode = g_msLastMode;
                g_modeSwitch.update(deltaTime, &ipd, &fire, &mode);
                g_input.viewParams.ipdFactor = ipd;
                if (fire && xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
                    xr.pfnRequestDisplayRenderingModeEXT(xr.session, mode);
                    g_msLastMode = mode;
                    UpdateTopBarButtonTitles(xr);
                }
            } else {
                g_input.viewParams.ipdFactor = steady;
            }
        }

        // Handle eye tracking mode toggle
        if (g_input.eyeTrackingModeToggleRequested) {
            g_input.eyeTrackingModeToggleRequested = false;
            if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
                XrEyeTrackingModeDXR newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                    ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
                xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            }
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                std::vector<XrCompositionLayerProjectionView> projectionViews;
                bool rendered = false;
                // XR_DXR_depth_budget v2 content-bounds ROI, filled in the render
                // block below, chained at xrEndFrame (Windows: contentBoundsUV).
                XrRect2Df contentBoundsUV{};
                bool haveContentBounds = false;

                if (frameState.shouldRender) {
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    XrViewEyeTrackingStateDXR eyeTrackingState = {};
                    eyeTrackingState.type = (XrStructureType)XR_TYPE_VIEW_EYE_TRACKING_STATE_DXR;
                    viewState.next = &eyeTrackingState;

                    // Hands (XR_EXT_hand_tracking): locate both hands at display
                    // time in the SAME space the views are located in, turn the
                    // pinches into rig deltas, and apply them exactly where the
                    // mouse drag would (yaw/pitch/roll + scaleFactor). The joints
                    // are physical display metres here; the gesture math runs on
                    // them untransformed (rig-independent, no feedback), and only
                    // the RENDERED hand is rebased into the model's world below.
                    if (g_handTracking.enabled()) {
                        g_handTracking.locate(xr.localSpace, frameState.predictedDisplayTime);
                        modelviewer::HandGestureFrame hf;
                        hf.timeSec = NowSec();
                        for (int h = 0; h < 2; h++)
                            modelviewer::HandTracking::toGestureSample(g_handTracking.hand(h), hf.hand[h]);
                        const modelviewer::HandGestureDelta hg = g_handGestures.update(hf);
                        if (hg.any()) {
                            MarkUserInput(g_input);   // a pinch is user input: parks auto-orbit
                            g_input.yaw += hg.yaw;
                            g_input.pitch += hg.pitch;
                            const float maxPitch = 1.5f;
                            if (g_input.pitch > maxPitch) g_input.pitch = maxPitch;
                            if (g_input.pitch < -maxPitch) g_input.pitch = -maxPitch;
                            g_input.roll += hg.roll;
                            float sf = g_input.viewParams.scaleFactor * hg.zoom;
                            if (sf < 0.1f) sf = 0.1f;
                            if (sf > 10.0f) sf = 10.0f;
                            g_input.viewParams.scaleFactor = sf;
                        }
                        if (g_handTracking.anyActive() && !g_handsEverSeen) {
                            g_handsEverSeen = true;
                            g_handHintUntilSec = NowSec() + 6.0;
                            LOG_INFO("Hands detected: pinch+drag rotates, two-hand pinch zooms/rolls");
                        }
                        // Throttled trace (every ~5 s): tip distances + pinch state per
                        // hand, so a log answers "did it ever pinch" without the HUD.
                        if (g_handTracking.locateCount() % 300 == 1 && g_handTracking.anyActive()) {
                            const XrVector3f& lw = g_handTracking.hand(0).joints[XR_HAND_JOINT_WRIST_EXT].pose.position;
                            const XrVector3f& rw = g_handTracking.hand(1).joints[XR_HAND_JOINT_WRIST_EXT].pose.position;
                            LOG_INFO("hands: L active=%d tips=%.0fmm %s wrist=(%.2f,%.2f,%.2f) | "
                                     "R active=%d tips=%.0fmm %s wrist=(%.2f,%.2f,%.2f) | yaw=%.2f pitch=%.2f roll=%.2f zoom=%.2f",
                                     g_handTracking.hand(0).active, g_handGestures.tipDistance(0) * 1000.0f,
                                     g_handGestures.pinched(0) ? "PINCH" : "open", lw.x, lw.y, lw.z,
                                     g_handTracking.hand(1).active, g_handGestures.tipDistance(1) * 1000.0f,
                                     g_handGestures.pinched(1) ? "PINCH" : "open", rw.x, rw.y, rw.z,
                                     g_input.yaw, g_input.pitch, g_input.roll, g_input.viewParams.scaleFactor);
                        }
                    }

                    // Turntable (common/model_orbit.h): the orbit state — the point
                    // under the display centre, yaw/pitch/roll, zoom — is a MODEL
                    // transform about its pivot. The rig the runtime sees stays a
                    // fixed identity pose at the locate origin with the fit's vH, so
                    // the display plane, the convergence distance, the depth budget
                    // and the hand space never move; only the model does. (Clean
                    // +Y-up frame — the ModelRenderer flips Vulkan-Y at the raster
                    // stage, not by reflecting the view; see model_renderer.cpp.)
                    XrPosef orbitPose;
                    quat_from_yaw_pitch_roll(g_input.yaw, g_input.pitch, g_input.roll, &orbitPose.orientation);
                    orbitPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};
                    {
                        float sceneXf[16];
                        modelviewer::BuildSceneTransform(sceneXf, orbitPose, g_input.viewParams.scaleFactor);
                        g_modelRenderer.setSceneTransform(sceneXf);
                    }
                    const XrPosef cameraPose = modelviewer::OrbitRigPose();
                    const float rigVH = g_input.viewParams.virtualDisplayHeight;

                    // XR_DXR_view_rig (#396 W7): chain the display rig so the runtime
                    // owns the off-axis Kooima + window resolve, returning render-ready
                    // XrView{pose, fov}. The raw channel carries display-space eyes for HUD.
                    const bool useRig =
                        xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;
                    XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
                    XrViewDisplayRawDXR viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
                    if (useRig) {
                        displayRig.pose = cameraPose;
                        displayRig.virtualDisplayHeight = rigVH;
                        displayRig.ipdFactor = g_input.viewParams.ipdFactor;
                        displayRig.parallaxFactor = g_input.viewParams.parallaxFactor;
                        displayRig.perspectiveFactor = g_input.viewParams.perspectiveFactor;
                        locateInfo.next = &displayRig;
                        eyeTrackingState.next = &viewRigRaw;
                    }

                    // XR_DXR_depth_budget (ADR-040): chain the advisory rear depth
                    // budget. Only under the rig, as on Windows/Linux — ez/vH, the
                    // ResolveClipPlanes inputs, are rig quantities.
                    XrRearDepthBudgetDXR rearBudget = {};
                    const bool useDepthBudget = xr.hasDepthBudgetExt && useRig;
                    if (useDepthBudget) dxr::ChainRearDepthBudget(viewState, rearBudget);

                    uint32_t runtimeViewCount = xr.maxViewCount > 0 ? xr.maxViewCount : 2;
                    if (runtimeViewCount > 8) runtimeViewCount = 8;
                    XrView views[8] = {};
                    for (uint32_t v = 0; v < runtimeViewCount; v++) views[v].type = XR_TYPE_VIEW;

                    cursor.chain(locateInfo, viewState);
                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState,
                        runtimeViewCount, &runtimeViewCount, views);
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                        // A runtime that recognizes the chained struct always
                        // overwrites `type`; an untouched zero-init (older runtime)
                        // stays null so ResolveClipPlanes falls back to the
                        // pre-extension rule (clip_policy.h contract).
                        const XrRearDepthBudgetDXR* rearBudgetPtr =
                            (useDepthBudget && rearBudget.type == (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR)
                                ? &rearBudget : nullptr;
                        const bool standalone = IsStandaloneSession(xr);
                        g_standaloneSession = standalone;

                        // --- Per-frame mode metadata ---
                        uint32_t modeViewCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount)
                            ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                        if (modeViewCount < 1) modeViewCount = 1;
                        // INV-3.1 clamp: the mode's advertised count is one of three
                        // bounds. `runtimeViewCount` now holds what xrLocateViews
                        // actually wrote; xr.maxViewCount is the session view
                        // configuration's count, which is what the atlas swapchain was
                        // worst-case-sized for (arraySize is 1 — the views are tiles in
                        // image 0, so tile capacity is the slice bound). Submitting past
                        // any of them fails xrEndFrame validation every frame — exactly
                        // what a Quad mode did before the PRIMARY_MULTIVIEW_DXR opt-in.
                        // Log ONCE per disagreeing combination, never per frame.
                        {
                            uint32_t bound = modeViewCount;
                            if (runtimeViewCount > 0 && runtimeViewCount < bound) bound = runtimeViewCount;
                            if (xr.maxViewCount > 0 && xr.maxViewCount < bound) bound = xr.maxViewCount;
                            if (bound < 1) bound = 1;
                            if (bound != modeViewCount) {
                                static uint32_t s_lastClampKey = 0;
                                const uint32_t key = (modeViewCount << 16) | (runtimeViewCount << 8) | xr.maxViewCount;
                                if (key != s_lastClampKey) {
                                    s_lastClampKey = key;
                                    LOG_WARN("[INV-3.1] submitted view count clamped %u -> %u "
                                             "(mode=%u located=%u viewConfig=%u, %s)",
                                             modeViewCount, bound, modeViewCount, runtimeViewCount,
                                             xr.maxViewCount, DxrViewConfigTypeName(xr.viewConfigType));
                                }
                                modeViewCount = bound;
                            }
                        }
                        bool display3D = (xr.renderingModeCount > 0)
                            ? xr.renderingModeDisplay3D[g_input.currentRenderingMode] : true;
                        bool monoMode = !display3D;
                        uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.renderingModeTileColumns[g_input.currentRenderingMode] > 0)
                            ? xr.renderingModeTileColumns[g_input.currentRenderingMode]
                            : (monoMode ? 1u : 2u);
                        uint32_t tileRows = (xr.renderingModeCount > 0 && xr.renderingModeTileRows[g_input.currentRenderingMode] > 0)
                            ? xr.renderingModeTileRows[g_input.currentRenderingMode]
                            : 1u;

                        int eyeCount = monoMode ? 1 : (int)modeViewCount;
                        cursor.afterLocate(views, (uint32_t)eyeCount);
                        if (cursorAdopt.endFrame(cursor)) LOG_INFO("%s", cursorAdopt.summary());

                        // HUD eye readout. Under the rig, views[] carries render-ready
                        // WORLD eyes, so the display-space eyes come from the raw channel
                        // (XrViewDisplayRawDXR); without the rig, fall back to views[].
                        if (useRig && viewRigRaw.eyeCountOutput > 0) {
                            for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                                xr.eyePositions[v][0] = viewRigRaw.rawEyes[v].x;
                                xr.eyePositions[v][1] = viewRigRaw.rawEyes[v].y;
                                xr.eyePositions[v][2] = viewRigRaw.rawEyes[v].z;
                            }
                        } else {
                            for (uint32_t v = 0; v < modeViewCount && v < 8; v++) {
                                xr.eyePositions[v][0] = views[v].pose.position.x;
                                xr.eyePositions[v][1] = views[v].pose.position.y;
                                xr.eyePositions[v][2] = views[v].pose.position.z;
                            }
                        }
                        xr.isEyeTracking = (eyeTrackingState.isTracking == XR_TRUE);
                        xr.activeEyeTrackingMode = (uint32_t)eyeTrackingState.activeMode;

                        // Hand mesh for this frame: joints (physical metres, locate
                        // space) -> the model's world through the rig the app just
                        // chained (display plane -> canvas -> m2v -> rig pose), so a
                        // hand 20 cm in front of the panel floats 20 cm x m2v in
                        // front of the virtual display, in every view. Without the
                        // rig the locate space IS the world and the map is identity.
                        // Preferred (XR_DXR_view_rig spec >= 4): the runtime returns the
                        // joints in the rig frame for the same descriptor the views
                        // used (located after them); the map is then the identity.
                        if (g_handRenderer.initialized()) {
                            const bool rigScoped = useRig &&
                                g_handTracking.locateRig(xr.localSpace, frameState.predictedDisplayTime, &displayRig);
                            const modelviewer::HandRigMap map = rigScoped ? modelviewer::HandRigMap{}
                                : modelviewer::MakeHandRigMap(
                                useRig ? &viewRigRaw : nullptr,
                                xr.displayPixelWidth, xr.displayPixelHeight,
                                xr.displayWidthM, xr.displayHeightM, cameraPose, rigVH);
                            modelviewer::HandRenderHand hands[2];
                            for (int h = 0; h < 2; h++) {
                                const modelviewer::HandJointSet& js =
                                    rigScoped ? g_handTracking.rigHand(h) : g_handTracking.hand(h);
                                hands[h].active = js.active;
                                memcpy(hands[h].color, h == 0 ? kHandColorLeft : kHandColorRight, sizeof(hands[h].color));
                                for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                                    const XrHandJointLocationEXT& jl = js.joints[j];
                                    modelviewer::HandRenderJoint& rj = hands[h].joints[j];
                                    rj.valid = js.active && (jl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
                                    if (!rj.valid) continue;
                                    const XrVector3f w = modelviewer::HandRigMapPoint(map, jl.pose.position);
                                    rj.pos[0] = w.x; rj.pos[1] = w.y; rj.pos[2] = w.z;
                                    rj.radius = modelviewer::HandRigMapRadius(map, jl.radius > 0.002f ? jl.radius : 0.008f);
                                }
                            }
                            g_handRenderer.update(hands, deltaTime);
                            if (g_handTracking.locateCount() % 300 == 1 && g_handTracking.anyActive()) {
                                const int h = g_handTracking.hand(0).active ? 0 : 1;
                                const modelviewer::HandRenderJoint& w = hands[h].joints[XR_HAND_JOINT_WRIST_EXT];
                                LOG_INFO("hands: %s map m2v=%.2f canvasOff=(%.3f,%.3f) plane=(%.2f,%.2f,%.2f) "
                                         "%s wrist world=(%.2f,%.2f,%.2f) r=%.3f | rig=(%.2f,%.2f,%.2f) vH=%.2f eye0=(%.2f,%.2f,%.2f) mesh=%s",
                                         rigScoped ? "rig-scoped" : "HandRigMap", map.m2v, map.canvasOffsetM.x, map.canvasOffsetM.y,
                                         map.displayPlane.position.x, map.displayPlane.position.y, map.displayPlane.position.z,
                                         h == 0 ? "L" : "R", w.pos[0], w.pos[1], w.pos[2], w.radius,
                                         cameraPose.position.x, cameraPose.position.y, cameraPose.position.z, rigVH,
                                         views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
                                         g_handRenderer.visible() ? "visible" : "empty");
                            }
                        }

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (used for the projection viewport per eye and for the 'I'
                        // capture region) follow as cols × renderW × rows × renderH.
                        // The swapchain was sized at creation time to fit the
                        // largest atlas across all advertised modes, so no clamp
                        // is needed here.
                        float scaleX = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleX[g_input.currentRenderingMode] : 1.0f;
                        float scaleY = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleY[g_input.currentRenderingMode] : 1.0f;
                        uint32_t renderW = (uint32_t)((double)g_windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)g_windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;
                        g_renderW = renderW; g_renderH = renderH;

                        // --- Consume the runtime's render-ready XrView{pose, fov} (#396 W7) ---
                        // The runtime owns the off-axis Kooima (window resolve included); the
                        // app keeps only the clip policy (fov is clip-independent), resolved by
                        // dxr::ResolveClipPlanes: near = ez - vH; far = ez + 1000*vH when
                        // opaque, ez + the runtime's rear budget (or 0 = the ZDP, standalone
                        // with no budget) when transparent — exactly Windows/Linux.
                        // ez = RigLocalEyeZ (== the display-space eye Z display3d resolved).
                        // The view matrix is the plain clean-frame mat4_view_from_xr_pose —
                        // ModelRenderer owns the Vulkan Y-down flip via a negative viewport.
                        // GL projection → [0,1] depth remap kept (mesh uses the depth buffer).
                        std::vector<Display3DView> eyeViews((size_t)eyeCount);
                        // Per-eye far cull (0 = none) and the UNRESTRICTED-far twin
                        // projection for the v3 coverage pass, so the content mask
                        // never depends on the budget it feeds (runtime#1470).
                        std::vector<float> eyeClipFar((size_t)eyeCount, 0.0f);
                        std::vector<std::array<float, 16>> eyeProjUnclipped((size_t)eyeCount);
                        bool hasKooima = useRig;
                        if (useRig) {
                            // Mono: collapse the active views to their centroid (pose + fov).
                            std::vector<XrView> srcViews;
                            if (monoMode && modeViewCount >= 1) {
                                XrView cv = views[0];
                                XrVector3f c = {0, 0, 0};
                                XrFovf f = {0, 0, 0, 0};
                                for (uint32_t v = 0; v < modeViewCount; v++) {
                                    c.x += views[v].pose.position.x;
                                    c.y += views[v].pose.position.y;
                                    c.z += views[v].pose.position.z;
                                    f.angleLeft  += views[v].fov.angleLeft;
                                    f.angleRight += views[v].fov.angleRight;
                                    f.angleUp    += views[v].fov.angleUp;
                                    f.angleDown  += views[v].fov.angleDown;
                                }
                                float inv = 1.0f / (float)modeViewCount;
                                cv.pose.position = {c.x * inv, c.y * inv, c.z * inv};
                                cv.fov = {f.angleLeft * inv, f.angleRight * inv,
                                          f.angleUp * inv, f.angleDown * inv};
                                srcViews.assign(1, cv);
                            } else {
                                for (int e = 0; e < eyeCount; e++)
                                    srcViews.push_back(views[e < (int)runtimeViewCount ? e : 0]);
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                const XrView& sv = srcViews[eye];
                                float ez = RigLocalEyeZ(cameraPose, sv.pose.position);
                                const dxr::ClipPlanes clip = dxr::ResolveClipPlanes(
                                    ez, rigVH, rearBudgetPtr, g_transparentBg, standalone);
                                const float near_z = clip.near_z;
                                const float far_z  = clip.far_z;
                                eyeClipFar[eye] = clip.clipFar;
                                if (eye == 0) LogRearDepthBudget(rearBudgetPtr, clip.farOffsetVH, g_transparentBg, standalone);
                                mat4_view_from_xr_pose(eyeViews[eye].view_matrix, sv.pose);
                                mat4_from_xr_fov(eyeViews[eye].projection_matrix, sv.fov, near_z, far_z);
                                // GL ([-1,1] clip-z) → Vulkan [0,1] depth for the mesh's depth buffer.
                                convert_projection_gl_to_zero_to_one(eyeViews[eye].projection_matrix);
                                // The unrestricted twin: transparent=false IS the
                                // farOffsetVH = 1000 branch; near is budget-independent.
                                const dxr::ClipPlanes clipU = dxr::ResolveClipPlanes(
                                    ez, rigVH, nullptr, /*transparent=*/false, standalone);
                                mat4_from_xr_fov(eyeProjUnclipped[eye].data(), sv.fov, clipU.near_z, clipU.far_z);
                                convert_projection_gl_to_zero_to_one(eyeProjUnclipped[eye].data());
                                eyeViews[eye].fov = sv.fov;
                                eyeViews[eye].eye_world = sv.pose.position;
                                eyeViews[eye].orientation = sv.pose.orientation;
                                eyeViews[eye].eye_display = {0.0f, 0.0f, ez};
                                eyeViews[eye].near_z = near_z;
                                eyeViews[eye].far_z = far_z;
                            }
                        }

                        // Double-click focus: ray from CENTER physical eyes through the
                        // physical mouse location on the display surface, pick nearest surface,
                        // then smoothly move & re-orient the virtual display to face back
                        // along the ray.
                        if (g_input.teleportRequested && useRig) {
                            g_input.teleportRequested = false;
                            NSSize viewSize = [[g_window contentView] bounds].size;
                            float ndcX = 2.0f * g_input.teleportMouseX / (float)viewSize.width - 1.0f;
                            // Cocoa locationInWindow has y=0 at the BOTTOM of the window.
                            // OpenGL/+Y-up NDC also has y=-1 at the bottom, so this maps
                            // directly with no negation. (The Windows demo negates because
                            // Win32 mouse y=0 is at the TOP.)
                            float ndcY = 2.0f * g_input.teleportMouseY / (float)viewSize.height - 1.0f;

                            // Center-eye pick view reconstructed from the render-ready rig
                            // views: average the active eye poses + fovs into a symmetric
                            // center frustum in the clean +Y-up world frame the model lives in
                            // (no Y flip — the pick ray must match the world, not the Vulkan
                            // raster). GL projection (no [0,1] remap) since the ray is a full
                            // line.
                            XrVector3f cpos = {0, 0, 0};
                            XrFovf cfov = {0, 0, 0, 0};
                            for (int e = 0; e < eyeCount; e++) {
                                cpos.x += eyeViews[e].eye_world.x;
                                cpos.y += eyeViews[e].eye_world.y;
                                cpos.z += eyeViews[e].eye_world.z;
                                cfov.angleLeft  += eyeViews[e].fov.angleLeft;
                                cfov.angleRight += eyeViews[e].fov.angleRight;
                                cfov.angleUp    += eyeViews[e].fov.angleUp;
                                cfov.angleDown  += eyeViews[e].fov.angleDown;
                            }
                            float invE = 1.0f / (float)eyeCount;
                            XrPosef cpose;
                            cpose.position = {cpos.x * invE, cpos.y * invE, cpos.z * invE};
                            cpose.orientation = cameraPose.orientation;
                            cfov = {cfov.angleLeft * invE, cfov.angleRight * invE,
                                    cfov.angleUp * invE, cfov.angleDown * invE};
                            float ez = RigLocalEyeZ(cameraPose, cpose.position);
                            float pickNear = (ez - rigVH > 1.0e-4f) ? (ez - rigVH) : 1.0e-4f;
                            float pickFar = ez + 1000.0f * rigVH;
                            float pickView[16], pickProj[16];
                            mat4_view_from_xr_pose(pickView, cpose);
                            mat4_from_xr_fov(pickProj, cfov, pickNear, pickFar);

                            XrVector3f rayOriginV, rayDirV;
                            display3d_unproject_ndc_to_ray(ndcX, ndcY,
                                pickView, pickProj, &rayOriginV, &rayDirV);

                            float rayOrigin[3] = {rayOriginV.x, rayOriginV.y, rayOriginV.z};
                            float rayDir[3]    = {rayDirV.x,    rayDirV.y,    rayDirV.z};
                            float hitPos[3];
                            if (g_modelRenderer.pickSurface(rayOrigin, rayDir, hitPos)) {
                                // Both endpoints stored in the clean +Y-up WORLD frame
                                // (the same frame as g_input.cameraPosX/Y/Z and the model)
                                // so the slerp interpolates consistently. (#396 W7: cameraPose
                                // is no longer Y-negated — the renderer flips at the raster
                                // stage — so this is just the world camera position.)
                                // The hit is in locate space; the orbit state (the
                                // point under the display centre) lives in MODEL space.
                                float hitModel[3];
                                g_modelRenderer.sceneToModelPoint(hitPos, hitModel);
                                XrPosef fromWorld;
                                fromWorld.orientation = orbitPose.orientation;
                                fromWorld.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};
                                XrPosef target;
                                target.position = {hitModel[0], hitModel[1], hitModel[2]};
                                target.orientation = orbitPose.orientation;
                                g_input.transitionFrom = fromWorld;
                                g_input.transitionTo = target;
                                g_input.transitionT = 0.0f;
                                g_input.transitioning = true;
                                LOG_INFO("Focus on surface (%.3f, %.3f, %.3f)",
                                    hitPos[0], hitPos[1], hitPos[2]);
                            }
                        } else if (g_input.teleportRequested) {
                            g_input.teleportRequested = false; // consume without Kooima
                        }

                        rendered = true;
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            // ADR-041 (runtime #1612): the layer carries EVERY located
                            // view; only [0, eyeCount) are rendered (1 in a 2D mode) and
                            // the tail is aliased onto view 0 after the loop below.
                            // Under PRIMARY_MULTIVIEW_DXR xrEndFrame rejects a shorter
                            // layer — the panel then froze on the last 3D frame.
                            const uint32_t locatedViewCount =
                                (runtimeViewCount > (uint32_t)eyeCount) ? runtimeViewCount : (uint32_t)eyeCount;
                            projectionViews.assign((size_t)locatedViewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                            std::vector<std::array<float, 16>> viewMat((size_t)eyeCount);
                            std::vector<std::array<float, 16>> projMat((size_t)eyeCount);
                            std::vector<std::array<float, 16>> projMatUnclipped((size_t)eyeCount);
                            std::vector<float> clipFar((size_t)eyeCount, 0.0f);
                            std::vector<std::pair<uint32_t, uint32_t>> tileOffsets((size_t)eyeCount);
                            for (int eye = 0; eye < eyeCount; eye++) {
                                int srcView = eye < (int)runtimeViewCount ? eye : 0;
                                if (hasKooima) {
                                    memcpy(viewMat[eye].data(), eyeViews[eye].view_matrix, sizeof(float) * 16);
                                    memcpy(projMat[eye].data(), eyeViews[eye].projection_matrix, sizeof(float) * 16);
                                    projMatUnclipped[eye] = eyeProjUnclipped[eye];
                                    clipFar[eye] = eyeClipFar[eye];
                                    views[srcView].pose.position = eyeViews[eye].eye_world;
                                    views[srcView].pose.orientation = cameraPose.orientation;
                                } else {
                                    mat4_view_from_xr_pose(viewMat[eye].data(), views[srcView].pose);
                                    mat4_from_xr_fov(projMat[eye].data(), views[srcView].fov, 0.01f, 100.0f);
                                    projMatUnclipped[eye] = projMat[eye];   // no budget without the rig
                                }

                                // Tile-aware viewport: row-major eye layout in the atlas.
                                // For mono (cols=rows=1) this collapses to (0, 0).
                                uint32_t tileX = (uint32_t)(eye % (int)tileColumns);
                                uint32_t tileY = (uint32_t)(eye / (int)tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;
                                tileOffsets[eye] = {vpX, vpY};

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                                projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = views[srcView].pose;
                                projectionViews[eye].fov = hasKooima ? eyeViews[eye].fov : views[srcView].fov;
                            }
                            // Inactive tail -> view 0's tile, own located pose/fov.
                            DxrAliasInactiveViews(projectionViews.data(), views, locatedViewCount,
                                                  (uint32_t)eyeCount);

                            // Render model or placeholder
                            VkImage targetImage = swapchainImages[imageIndex].image;
                            VkFormat swapFormat = (VkFormat)xr.swapchain.format;

                            // XR_DXR_depth_budget v2: the model's full-extent AABB
                            // projected through this frame's view-projections, unioned
                            // over the rendered eyes, canvas-normalised. Rig-only and
                            // model-only (linux/main.cpp, Windows #116 v2).
                            if (xr.hasDepthBudgetExt && hasKooima && g_fitValid && g_modelRenderer.hasModel()) {
                                const float halfW = 0.5f * g_fitExtentW, halfH = 0.5f * g_fitExtentH,
                                            halfD = 0.5f * g_fitExtentD;
                                const float aabbMin[3] = {g_fitCenter[0] - halfW, g_fitCenter[1] - halfH,
                                                          g_fitCenter[2] - halfD};
                                const float aabbMax[3] = {g_fitCenter[0] + halfW, g_fitCenter[1] + halfH,
                                                          g_fitCenter[2] + halfD};
                                std::vector<std::array<float, 16>> viewProj((size_t)eyeCount);
                                std::vector<const float*> viewProjPtr((size_t)eyeCount);
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    mat4_multiply(viewProj[eye].data(), projMat[eye].data(), viewMat[eye].data());
                                    viewProjPtr[eye] = viewProj[eye].data();
                                }
                                dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, viewProjPtr.data(),
                                                               (uint32_t)eyeCount, &contentBoundsUV);
                                haveContentBounds = true;
                            }

                            // XR_DXR_depth_budget v3: arm the renderer's coverage-only
                            // pass (no far clip, unrestricted projection) so the mask is
                            // the silhouette AT AN UNRESTRICTED BUDGET (runtime#1470).
                            // Gated on the runtime's version, transparency and an app
                            // window (the Windows "transparent && borderless" gate).
                            const bool wantContentMask = xr.hasDepthBudgetExt && xr.depthBudgetExtVersion >= 3 &&
                                                         hasKooima && g_transparentBg && g_transparentCapable;
                            g_modelRenderer.beginContentMaskFrame(wantContentMask);

                            if (g_modelRenderer.hasModel()) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    g_modelRenderer.renderEye(
                                        targetImage, swapFormat,
                                        xr.swapchain.width, xr.swapchain.height,
                                        tileOffsets[eye].first, tileOffsets[eye].second,
                                        renderW, renderH,
                                        viewMat[eye].data(), projMat[eye].data(),
                                        g_transparentBg, clipFar[eye], projMatUnclipped[eye].data());
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, cmdPool,
                                    targetImage, xr.swapchain.width, xr.swapchain.height,
                                    g_input.yaw, g_input.pitch, g_transparentBg);
                            }

                            // v3: reduce the frame's UNCLIPPED coverage to the
                            // extension's occupancy grid (linux/main.cpp sizing).
                            if (wantContentMask) {
                                const uint8_t* cov = g_modelRenderer.contentMaskCoverage();
                                std::vector<uint8_t> cells;
                                const uint32_t covW = ModelRenderer::kContentMaskCovW;
                                const uint32_t covH = ModelRenderer::kContentMaskCovH;
                                const uint32_t mW = std::max(1u, std::min(covW / 4u, dxr::kContentMaskRecommendedCells));
                                const uint32_t mH = std::max(1u, std::min(covH / 4u, dxr::kContentMaskRecommendedCells));
                                const bool ok = cov != nullptr &&
                                    dxr::ContentMaskFromCoverage(cov, covW, covH, covW, g_windowW, g_windowH,
                                                                 nullptr, mW, mH, cells) &&
                                    dxr::ContentMaskCoverageCells(cells) > 0;
                                if (ok) {
                                    g_contentMaskCells.swap(cells);
                                    g_contentMaskW = mW;
                                    g_contentMaskH = mH;
                                    if (!g_contentMaskChaining) {
                                        g_contentMaskChaining = true;
                                        LOG_INFO("XR_DXR_depth_budget v3: content mask chaining started (%ux%u cells, %u occupied)",
                                                 mW, mH, dxr::ContentMaskCoverageCells(g_contentMaskCells));
                                    }
                                } else {
                                    g_contentMaskCells.clear();
                                }
                            }
                            if (!wantContentMask || g_contentMaskCells.empty()) {
                                g_contentMaskCells.clear();
                                if (g_contentMaskChaining) {
                                    g_contentMaskChaining = false;
                                    LOG_INFO("XR_DXR_depth_budget v3: content mask chaining stopped "
                                             "— falling back to the content bounds / runtime ROI");
                                }
                            }

                            // Click-through coverage (#181): the frame's own alpha,
                            // first + last view tile, read back one frame late.
                            // Also run in opaque mode under DXR_MODELVIEWER_ALPHA_PROBE.
                            {
                                MacClickthroughParams cp;
                                cp.dev = vkDevice;
                                cp.phys = physDevice;
                                cp.queue = graphicsQueue;
                                cp.queueFamily = queueFamilyIndex;
                                cp.viewImage = targetImage;
                                cp.viewFormat = swapFormat;
                                cp.tileW = renderW;
                                cp.tileH = renderH;
                                cp.firstTileX = tileOffsets[0].first;
                                cp.firstTileY = tileOffsets[0].second;
                                cp.lastTileX = tileOffsets[(size_t)eyeCount - 1].first;
                                cp.lastTileY = tileOffsets[(size_t)eyeCount - 1].second;
                                cp.twoViews = eyeCount > 1;
                                cp.winW = g_windowW;
                                cp.winH = g_windowH;
                                cp.active = (g_transparentBg && g_transparentCapable) || g_alphaProbe;
                                MacClickthroughUpdate(cp);
                                if (g_alphaProbe) {
                                    static double s_nextProbe = 0.0;
                                    MacAlphaStats st;
                                    if (NowSec() >= s_nextProbe && MacClickthroughAlphaStats(&st)) {
                                        s_nextProbe = NowSec() + 2.0;
                                        const NSRect cr = [g_window contentRectForFrameRect:[g_window frame]];
                                        const NSPoint mp = [NSEvent mouseLocation];
                                        LOG_INFO("alpha-probe: bg=%s texels=%u alpha min=%u max=%u "
                                                 "zero=%u (%.1f%%) full=%u (%.1f%%) | cursor=(%.0f,%.0f) "
                                                 "content=(%.0f,%.0f %.0fx%.0f) ignoresMouseEvents=%d "
                                                 "[ct in=%d cov=%d hit=%d chrome=%d reach=%d uv=%.2f,%.2f]",
                                                 g_transparentBg ? "TRANSPARENT" : "OPAQUE", st.texels,
                                                 (unsigned)st.minAlpha, (unsigned)st.maxAlpha,
                                                 st.zero, 100.0 * st.zero / (st.texels ? st.texels : 1),
                                                 st.full, 100.0 * st.full / (st.texels ? st.texels : 1),
                                                 mp.x, mp.y, cr.origin.x, cr.origin.y, cr.size.width, cr.size.height,
                                                 g_window.ignoresMouseEvents ? 1 : 0,
                                                 g_ctLast.inContent, g_ctLast.haveCov, g_ctLast.hit,
                                                 g_ctLast.overChrome, g_ctLast.reachable, g_ctLast.u, g_ctLast.v);
                                    }
                                }
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasDXR
                            // (XR_DXR_atlas_capture, W6 of #396). The runtime owns
                            // the readback — no app-side staging texture. Skipped
                            // for mono (1×1). The prefix has no ".png"; the runtime
                            // appends "_atlas.png".
                            if (g_input.captureAtlasRequested) {
                                g_input.captureAtlasRequested = false;
                                uint32_t cols = tileColumns > 0 ? tileColumns : 1u;
                                uint32_t rows = tileRows > 0 ? tileRows : 1u;
                                if (!g_modelRenderer.hasModel()) {
                                    LOG_WARN("Capture skipped: no model loaded");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                } else if (xr.pfnCaptureAtlasEXT &&
                                           xr.session != XR_NULL_HANDLE) {
                                    // Strip extension from model filename
                                    // (e.g. "sample.glb" → "sample").
                                    auto dot = g_loadedFileName.find_last_of('.');
                                    std::string stem = (dot == std::string::npos)
                                        ? g_loadedFileName
                                        : g_loadedFileName.substr(0, dot);
                                    if (stem.empty()) stem = "scene";
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoDXR info = {XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR;
                                    strncpy(info.pathPrefix, prefix.c_str(),
                                            XR_ATLAS_CAPTURE_PATH_MAX_DXR - 1);
                                    XrResult cr = xr.pfnCaptureAtlasEXT(xr.session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::TriggerCaptureFlash(
                                            (__bridge void*)g_metalView);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasDXR failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_DXR_atlas_capture not available");
                                }
                            }

                            ReleaseSwapchainImage(xr);
                        } else {
                            rendered = false;
                        }
                    }
                }

                // XR_DXR_depth_budget ROI chain (Windows' frameEndNext): mask
                // first, then bounds; the runtime's precedence is mask -> bounds
                // -> whole canvas regardless of order. The retained mask
                // re-chains on frames that skip rendering.
                XrFrameEndInfo roiChain = {XR_TYPE_FRAME_END_INFO};
                XrContentMaskDXR contentMask = {};
                XrContentBoundsDXR contentBounds = {};
                if (xr.hasDepthBudgetExt && xr.depthBudgetExtVersion >= 3 && !g_contentMaskCells.empty())
                    dxr::ChainContentMask(roiChain, contentMask, g_contentMaskCells, g_contentMaskW, g_contentMaskH);
                if (haveContentBounds) dxr::ChainContentBounds(roiChain, contentBounds, contentBoundsUV);

                if (rendered) {
                    EndFrame(xr, frameState.predictedDisplayTime,
                        projectionViews.data(), (uint32_t)projectionViews.size(), roiChain.next);
                } else {
                    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
                    ei.next = roiChain.next;
                    ei.displayTime = frameState.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    ei.layerCount = 0; ei.layers = nullptr;
                    xrEndFrame(xr.session, &ei);
                }
            }
        } else {
            usleep(100000);
        }

        // Update HUD
        g_hudUpdateTimer += deltaTime;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                if (g_input.hudVisible && g_hudView != nil) {
                    double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                    NSString *sceneInfo = g_modelRenderer.hasModel()
                        ? [NSString stringWithFormat:@"Model: %s", g_loadedFileName.c_str()]
                        : @"No model loaded (Ctrl+O)";
                    // Say out loud when the asset uses material extensions we
                    // don't implement — those materials are rendering as their
                    // base layer, and #70 forbids letting that pass silently.
                    const std::string unsup = g_modelRenderer.unsupportedExtensionsSummary();
                    if (!unsup.empty()) {
                        sceneInfo = [sceneInfo stringByAppendingFormat:
                            @"\n  ! ignoring: %s", unsup.c_str()];
                    }

                    int depthPct = (int)(g_input.viewParams.ipdFactor * 100.0f + 0.5f);
                    // "held" rather than "idle countdown" while a clip plays —
                    // the countdown is being reset every frame, so it will never
                    // reach the turntable and saying otherwise is a lie.
                    const bool clipHeld =
                        g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused();
                    const bool transpHeld = g_transparentBg && g_standaloneSession;
                    const char *orbitLabel = g_input.animateEnabled
                        ? (clipHeld ? "ON (held: clip playing)"
                                    : transpHeld ? "ON (held: transparent)"
                                    : (g_input.animationActive ? "ON (running)"
                                                               : "ON (idle countdown)"))
                        : "OFF";
                    uint32_t activeViewCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount)
                        ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                    NSMutableString *eyesStr = [NSMutableString string];
                    for (uint32_t v = 0; v < activeViewCount && v < 8; v++) {
                        [eyesStr appendFormat:@"View %u: (%.3f, %.3f, %.3f)\n", v,
                            xr.eyePositions[v][0], xr.eyePositions[v][1], xr.eyePositions[v][2]];
                    }
                    NSString *handsStr = @"";
                    if (g_handTracking.enabled()) {
                        auto handWord = [](int h) -> const char* {
                            if (!g_handTracking.hand(h).active) return "--";
                            return g_handGestures.pinched(h) ? "PINCH" : "open";
                        };
                        handsStr = [NSString stringWithFormat:@"Hands: L %s  R %s  (tips %.0f / %.0f mm)\n",
                                    handWord(0), handWord(1),
                                    g_handGestures.tipDistance(0) * 1000.0f,
                                    g_handGestures.tipDistance(1) * 1000.0f];
                    }
                    NSString *text = [NSString stringWithFormat:
                        @"%s\nSession: %d\n"
                        "Mode: %s (%s, %u view%s)\n"
                        "%@\n"
                        "Env: %s  EV %+.2f  Tone: %s\n"
                        "Depth/IPD: %d%%  Zoom: %.2fx  Auto-Orbit: %s\n"
                        "Background: %s\n"
                        "FPS: %.0f (%.1f ms)\n"
                        "Render: %ux%u  Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "%@"
                        "Vdisplay: (%.2f, %.2f, %.2f)\n"
                        "%@"
                        "\nWASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom\n"
                        "DblClick=Focus  -/= Depth  Space=Reset  N=Clip  K=Play/Pause\n"
                        "M=Auto-Orbit  V=Mode  Ctrl+O=Load  [/]=Exposure  G=Tone\n"
                        "Ctrl/Cmd+T=Transparent  T=Eye-tracking mode\n"
                        "Pinch+drag=Rotate  2-hand pinch: spread=Zoom twist=Roll\n"
                        "Tab=HUD  ESC=Quit",
                        xr.systemName, (int)xr.sessionState,
                        (xr.renderingModeCount > 0 && xr.renderingModeNames[g_input.currentRenderingMode][0] != '\0') ? xr.renderingModeNames[g_input.currentRenderingMode] : "Unknown",
                        (xr.renderingModeCount > 0 ? (xr.renderingModeDisplay3D[g_input.currentRenderingMode] ? "3D" : "2D") : "3D"),
                        activeViewCount, activeViewCount == 1 ? "" : "s",
                        sceneInfo,
                        g_modelRenderer.environmentName().c_str(),
                        g_modelRenderer.exposureEV(), g_modelRenderer.toneCurveName(),
                        depthPct, g_input.viewParams.scaleFactor, orbitLabel,
                        !g_transparentCapable ? "OPAQUE (transparency off: MODEL_TRANSPARENT=0)"
                            : (g_transparentBg ? "TRANSPARENT" : "OPAQUE"),
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH, g_windowW, g_windowH,
                        xr.displayWidthM, xr.displayHeightM,
                        eyesStr,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                        handsStr];
                    g_hudView.hudText = text;
                    // Auto-size the frosted backdrop to fit the text; inner view auto-resizes.
                    NSDictionary *attrs = @{
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
                    };
                    NSRect textBounds = [text boundingRectWithSize:NSMakeSize(420, CGFLOAT_MAX)
                                         options:NSStringDrawingUsesLineFragmentOrigin
                                         attributes:attrs];
                    CGFloat pad = 20.0;
                    NSRect hudFrame = NSMakeRect(8, 8,
                        ceilf(textBounds.size.width + pad),
                        ceilf(textBounds.size.height + pad));
                    [g_hudBackdrop setFrame:hudFrame];
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudBackdrop setHidden:NO];
                } else if (g_hudView != nil && NowSec() < g_handHintUntilSec) {
                    // First hand detection: a transient hint in the HUD chip,
                    // even with the full HUD hidden.
                    NSString *hint = @"Hands detected\n"
                                      "Pinch + drag  =  rotate\n"
                                      "Two-hand pinch:  spread = zoom,  twist = roll,  drag = rotate";
                    g_hudView.hudText = hint;
                    NSDictionary *attrs = @{
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
                    };
                    NSRect tb = [hint boundingRectWithSize:NSMakeSize(420, CGFLOAT_MAX)
                                 options:NSStringDrawingUsesLineFragmentOrigin attributes:attrs];
                    [g_hudBackdrop setFrame:NSMakeRect(8, 8, ceilf(tb.size.width + 20.0), ceilf(tb.size.height + 20.0))];
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudBackdrop setHidden:NO];
                } else if (g_hudBackdrop != nil) {
                    [g_hudBackdrop setHidden:YES];
                }
            }
        }
    }

    // Clean exit
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        xrRequestExitSession(xr.session);
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr); usleep(10000);
        }
    }

    LOG_INFO("=== Shutting down ===");
    g_xrForMcp = nullptr;  // session is going away; stop touching MCP tools
    g_modelRenderer.setOverlayDraw(nullptr);
    g_modelRenderer.cleanup();      // waits for the device: the hand buffers are idle after this
    g_handRenderer.destroy();
    g_handTracking.destroy();       // trackers before the session
    MacClickthroughDestroy(vkDevice);
    if (cmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
    CleanupOpenXR(xr);
    // MoltenVK may throw std::system_error ("mutex lock failed") during device/instance
    // destruction due to internal threading cleanup.  Catch and ignore since we're exiting.
    try {
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
    } catch (const std::exception& e) {
        LOG_WARN("Vulkan cleanup exception (ignored): %s", e.what());
    }
    LOG_INFO("Application shutdown complete");
    return 0;
}

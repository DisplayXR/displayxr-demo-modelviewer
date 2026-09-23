// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux Vulkan OpenXR glTF/PBR model viewer (handle app, X11 or Wayland).
 *
 * The Linux arm of displayxr-demo-modelviewer. Renders glTF/STL/OBJ/FBX/USD
 * models on tracked 3D displays via OpenXR + Vulkan, reusing the vendor-neutral
 * model_common/ModelRenderer PBR pipeline shared with the macOS (macos/main.mm)
 * and Windows (windows/main.cpp) entry points.
 *
 * WINDOWING — HANDLE app, ONE binary for X11 and native Wayland: the window is
 * displayxr-common's displayxr::linux_window (dxr_linux_window.h), the one
 * Linux window implementation shared with the runtime's test apps and the
 * other demos. It owns a toplevel on the 3D panel and passes it via
 * XR_DXR_xlib_window_binding or XR_DXR_wayland_surface_binding, so the runtime
 * weaves window-relative and the app receives input. The platform is chosen
 * by capability at startup (--platform=x11|wayland|auto; default auto: native
 * Wayland when the compositor is ready — fractional-scale + viewporter + the
 * window-geometry extension on D-Bus — else X11, XWayland counting; never by
 * reading session environment variables). The window defaults to
 * 1920x1080 centred on the panel (XR_DXR_display_info desktop rect);
 * MODEL_WINDOW="WxH+X+Y" overrides (the position is X11-only — a Wayland
 * client cannot place itself). Asking for exactly the panel's size makes it
 * genuinely fullscreen on that monitor (INV-1.3). When no window system
 * answers the app falls back to hosted-NULL, which also keeps it startable on
 * a headless CI runner.
 *
 * INPUT — parity with windows/main.cpp + displayxr-common's input_handler.cpp,
 * sensitivities and clamps included:
 *   LMB drag      orbit          RMB drag    move the window (phase-snapped)
 *   LMB on header bar: drag (phase-snapped) / minimize / close
 *   dbl-click     focus/teleport  wheel      zoom (+Shift/Ctrl/Alt variants)
 *   WASDEQ move   SPACE reset     C rig      V / 0-8 rendering mode
 *   T eye-track   I atlas capture M orbit    N/K clip   L/G/[/] grading
 *   -/= depth     Ctrl+O open     F11 full   P then X/Y/Z recenter pins
 *   ESC quit
 * Ctrl+O opens a zenity file-selection dialog (async fork/exec, no hard
 * dependency: absent zenity logs and no-ops).
 *
 * DRAG PHASE SNAP — a windowed 3D app must keep the woven interlace phase
 * invariant while it moves or the 3D shimmers. X11 gives the client no hook
 * into a WM-owned drag (Windows proposes each step via WM_WINDOWPOSCHANGING;
 * X11 only reports the result afterwards), so on X11 the window helper owns
 * the drag itself, routing every step through xrWeaveSnapWindowRectDXR
 * (XR_DXR_weave, via DxrWeaveSnap). That rules out SERVER-side decorations,
 * not decorations: the window carries a CLIENT-side GNOME-style header bar
 * (displayxr::csd — translucent with rounded corners on the ARGB visual);
 * LMB on the bar drags through the same snapped path, with close and minimize
 * buttons, and RMB-drag anywhere still moves it too (useful when mostly
 * transparent). On native Wayland the same gestures start the compositor's
 * own move. The snap extension is OPTIONAL: resolved at runtime and, when
 * absent, the drag runs unsnapped. DXR_X11_WM_DECORATIONS=1 restores a
 * decorated, WM-dragged window; DXR_X11_TEST_DRAG="dx,dy,steps" walks the
 * window through the same snap path with nobody at the mouse.
 *
 * TRANSPARENT BACKGROUND (Ctrl+T) + CLICK-THROUGH — Windows parity. The window
 * is created on a 32-bit ARGB visual and the session is created with
 * XR_DXR_xlib_window_binding's transparentBackgroundEnabled, exactly as the
 * Windows leg sets it unconditionally (windows/xr_session.cpp): the runtime
 * wires the non-opaque swapchain compositeAlpha at xrCreateSession and it
 * CANNOT be flipped afterwards, so the capability is always on and Ctrl+T only
 * changes what the app draws. Opaque mode writes alpha = 1 everywhere, which a
 * PRE_MULTIPLIED surface shows exactly as before. Transparent mode clears to
 * alpha 0, skips the skybox, and punches the window through to the desktop with
 * an input region built from the frame's own rendered alpha
 * (linux/clickthrough.cpp — the analogue of the Windows SetWindowRgn punch;
 * the helper applies it as an XShape ShapeInput region on X11 and as the
 * surface input region on Wayland). On X11 a compositing WM must be running
 * for the desktop to show through.
 *   MODEL_TRANSPARENT=1 (or --transparent) starts transparent instead of opaque.
 *   MODEL_TRANSPARENT=0 opts out of the capability entirely — opaque root
 *   visual, transparentBackgroundEnabled = XR_FALSE, Ctrl+T reports and no-ops.
 *   That is the exact pre-transparency behaviour, kept as an escape hatch.
 *
 * NOT PORTED from the Windows leg (see the repo CLAUDE.md for why): the HUD /
 * button bar / toasts (Direct2D + DirectWrite), the capture flash overlay,
 * drag-and-drop, the displayxr-view: protocol handler and the --src URL
 * download.
 */

#include <vulkan/vulkan.h>

// dxr::ParseLaunchArgs — the shared launch contract (positional model path,
// --vh). It MUST precede the X11 headers: <X11/X.h> does `#define None 0L`,
// which would mangle dxr::LaunchSrcKind::None. (Same trap the runtime's
// DxrKey enum documents for its `Unknown` enumerator.)
#include "launch_args.h"

// X11 window + input (handle app) — before the OpenXR platform header so the
// xlib binding struct sees the real Display/Window types.
// The Linux window (displayxr::linux_window). It pulls in Xlib (and, in a
// build with libwayland, <wayland-client.h>) BEFORE the window-binding
// extension headers, so those see the real Display / Window / wl_display /
// wl_surface types. Keys arrive as X11 keysyms on both backends.
#include "dxr_linux_window.h"
#include "dxr_weave_snap.h"
#include <X11/keysym.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>
#include <openxr/XR_DXR_atlas_capture.h>
#include <openxr/XR_DXR_weave.h>   // xrWeaveSnapWindowRectDXR — drag phase-snap (runtime#1588)

#include <array>
#include <chrono>
#include <functional>
#include <thread>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <climits>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "view_params.h"
#include "dxr_view_config.h" // displayxr-common: DxrSelectViewConfigType (#1486/#1500) + DxrAliasInactiveViews (ADR-041)
#include "display3d_view.h"
#include "projection_depth.h"
#include "model_renderer.h"
#include "recenter_control.h"  // dynamic-recenter per-axis pins (P then X/Y/Z, or DXR_RECENTER_PIN)
#include "auto_fit.h"          // dxr::AutoFitVHeight — shared width-aware load-time framing
#include "model_fit.h"         // modelviewer::FitVHeight — the page-parity framing rule
#include "model_loader.h"
#include "mode_switch.h"       // dxr::ModeSwitch — smooth 2D<->3D disparity ramp (V / 0-8)
#include "rig_mode.h"          // dxr::RigResetToInitial / RigToggleMode — SPACE + C
#include "dxr_view_math.h"     // dxr_rig_max_ipd_factor — camera-rig IPD comfort ceiling
#include "clickthrough.h"      // input-region punch-through (Ctrl+T transparent mode)

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { LOG_ERROR("%s failed: %d", #call, (int)_r); return false; } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { LOG_ERROR("%s failed: %d", #call, (int)_r); return false; } \
    } while (0)

static volatile bool g_running = true;
static ModelRenderer g_modelRenderer;
static std::string g_loadedFileName;
static uint32_t g_windowW = 1280, g_windowH = 720;

// ----------------------------------------------------------------------------
// Transparent background (Ctrl+T) — the Windows leg's g_transparentBg, with
// the two-flag split collapsed to one because X11 needs no style swap.
//
// On Windows, Ctrl+T ALSO flips the window between WS_OVERLAPPEDWINDOW and
// WS_POPUP+topmost, because a shaped WS_EX_NOREDIRECTIONBITMAP window cannot
// paint an OS frame. This window is already undecorated and client-dragged in
// every mode (the drag phase snap needs that — see the header block), so there
// is no style to swap and no second flag: the only structural side effect of
// Ctrl+T here is the click-through input region, plus keep-above
// (_NET_WM_STATE_ABOVE on X11) to mirror the Windows HWND_TOPMOST /
// HWND_NOTOPMOST behaviour.
//
// g_transparentCapable is fixed at startup: the ARGB visual is chosen when
// the window is created and transparentBackgroundEnabled at xrCreateSession,
// and NEITHER can be changed live. Drawing a transparent frame into an opaque
// session gives BLACK where the desktop should be, not see-through — so when
// the capability is off, Ctrl+T must refuse rather than "work" and look broken.
static bool g_transparentCapable = true;
static bool g_transparentBg = false;
static bool g_launchTransparent = false; //!< --transparent on the command line

// The window — X11 or native Wayland, header bar, drag, click-through — is
// displayxr-common's displayxr::linux_window. Destroyed LAST (the runtime's
// VkSurfaceKHR borrows its connection).
static DxrLinuxWindow g_window;
//! Title last pushed to the window (the header bar shows the loaded file).
static std::string g_windowTitle;

// ----------------------------------------------------------------------------
// Input state — the Linux transliteration of displayxr-common's InputState
// (common/input_handler.{h,cpp}, which is <windows.h>-gated and so cannot be
// linked here). macOS does exactly the same thing in macos/main.mm. Field names
// and, more importantly, the SEMANTICS (sensitivities, clamps, signs) are kept
// identical to the Windows handler so the three legs behave the same.
// ----------------------------------------------------------------------------
struct InputState {
    // Mouse
    int mouseX = 0, mouseY = 0;
    bool leftButton = false, rightButton = false, middleButton = false;
    bool dragging = false;              //!< LMB orbit drag in flight
    int dragStartX = 0, dragStartY = 0;

    // Camera orbit
    float yaw = 0.0f, pitch = 0.0f;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;

    // Held movement keys (WASDQE)
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;

    ViewParams viewParams;
    bool cameraMode = false;            //!< display rig (false) vs camera rig (true) — 'C'
    float nominalViewerZ = 0.5f;
    float canvasWidthM = 0.0f, canvasHeightM = 0.0f;
    float initialVirtualDisplayHeight = 0.0f;

    // One-shot requests, consumed by the main loop
    bool resetViewRequested = false;            // SPACE
    bool rigModeToggleRequested = false;        // C
    bool eyeTrackingModeToggleRequested = false;// T
    bool captureAtlasRequested = false;         // I
    bool cycleRenderingModeRequested = false;   // V
    int32_t absoluteRenderingModeRequested = -1;// 0-8
    bool animateToggleRequested = false;        // M
    bool cycleClipRequested = false;            // N
    bool playPauseRequested = false;            // K
    bool loadRequested = false;                 // Ctrl+O
    bool transparentBgToggleRequested = false;  // Ctrl+T
    bool hudVisible = false;                    // TAB (no HUD on this leg yet — tracked only)

    uint32_t renderingModeCount = 0;

    // Double-click focus (teleport)
    bool teleportRequested = false;
    float teleportMouseX = 0.0f, teleportMouseY = 0.0f;
    bool transitioning = false;
    XrPosef transitionFrom = {{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef transitionTo   = {{0, 0, 0, 1}, {0, 0, 0}};
    float transitionT = 0.0f;
    float transitionDuration = 0.45f;

    // Auto-orbit (turntable) — idle-timer gated yaw advance.
    bool animateEnabled = true;
    double lastInputTimeSec = 0.0;
    bool animationActive = false;
};

static InputState g_input;

// Smooth 2D<->3D disparity ramp around a rendering-mode switch. Driven inline,
// exactly as macOS does (Windows routes it through XrSessionUpdateModeSwitch,
// which lives on displayxr-common's Win32-only XrSessionManager).
static dxr::ModeSwitch g_modeSwitch;
static bool g_modeSwitchConfigured = false;
static uint32_t g_msLastMode = 1;

// Cached auto-fit result for the currently loaded scene, so SPACE returns to
// the framed pose rather than world origin (Windows main.cpp:2670, macOS:g_fit*).
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = 1.5f;
static float g_fitYaw       = 0.0f;
static float g_fitPitch     = 0.0f;
static float g_fitZoom      = 1.0f;
static bool  g_fitValid     = false;

//! --vh=<metres>: pins the virtual display height over the auto-fit result.
static float g_vhOverride = 0.0f;

// Dynamic-recenter pins. Default X Y Z matches the Windows modelviewer's hard
// pin; P arms and X/Y/Z toggle an axis, or DXR_RECENTER_PIN sets it up front.
static dxr::RecenterControl g_recenter;

static double NowSec() {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

static void MarkUserInput(InputState& input) {
    input.lastInputTimeSec = NowSec();
    input.animationActive = false;
}

// Rig position for this frame: a pinned axis tracks the smoothed animated
// centroid; an unpinned axis stays at the user-driven camera position.
// No-op for static models (anchor invalid).
static void ComputeRigPosition(float out[3]) {
    out[0] = g_input.cameraPosX;
    out[1] = g_input.cameraPosY;
    out[2] = g_input.cameraPosZ;
    float anchor[3];
    if (g_modelRenderer.getAnimatedAnchor(anchor)) {
        const dxr::RecenterPins pins = g_recenter.pins();
        if (pins.x) out[0] = anchor[0];
        if (pins.y) out[1] = anchor[1];
        if (pins.z) out[2] = anchor[2];
    }
}

static constexpr float kDefaultVirtualDisplayHeightM = 1.5f;
// Load-time framing is the page's rule (common/model_fit.h) over the shared
// width-aware rule from displayxr-common (dxr::AutoFitVHeight, default 80%
// fill in BOTH axes), with the SWEPT horizontal extent hypot(W, D) as the
// width plus a depth backstop — no separate vertical comfort multiplier; the
// fill fraction IS the headroom. One rule across every leg: common/model_fit.h.

// ---------------------------------------------------------------------------
// Drag-time window-origin phase snap (xrWeaveSnapWindowRectDXR, runtime#1588).
//
// The vendor display processor weaves at a phase that is a function of the
// window's ABSOLUTE position in physical panel pixels. Drag the window and the
// phase travels with it; drag it continuously and the 3D shimmers. The window
// helper owns the drag on X11 and routes every step through this provider
// (displayxr-common's DxrWeaveSnap). STRICTLY OPTIONAL: when the runtime does
// not serve the entry point, it reports identity once and the drag lands on
// the raw pointer position.
// ---------------------------------------------------------------------------
static DxrWeaveSnap g_weaveSnap;

// ============================================================================
// Inline math — column-major float[16] (mirrors macos/main.mm)
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
            tmp[col * 4 + row] = sum;
        }
    memcpy(out, tmp, sizeof(tmp));
}
static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m); m[12] = tx; m[13] = ty; m[14] = tz;
}
static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp), tanD = tanf(fov.angleDown);
    float w = tanR - tanL, h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / w; m[5] = 2.0f / h;
    m[8] = (tanR + tanL) / w; m[9] = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}
static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16]; mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz); rot[1]  = 2*(qx*qy + qz*qw); rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);     rot[5]  = 1 - 2*(qx*qx + qz*qz); rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);     rot[9]  = 2*(qy*qz - qx*qw); rot[10] = 1 - 2*(qx*qx + qy*qy);
    float invRot[16]; mat4_identity(invRot);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) invRot[j*4+i] = rot[i*4+j];
    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}
static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw/2), sy = sinf(yaw/2), cp = cosf(pitch/2), sp = sinf(pitch/2);
    out->w = cy*cp; out->x = cy*sp; out->y = sy*cp; out->z = -sy*sp;
}
static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
                             float* ox, float* oy, float* oz) {
    float tx = 2.0f*(q.y*vz - q.z*vy), ty = 2.0f*(q.z*vx - q.x*vz), tz = 2.0f*(q.x*vy - q.y*vx);
    *ox = vx + q.w*tx + (q.y*tz - q.z*ty);
    *oy = vy + q.w*ty + (q.z*tx - q.x*tz);
    *oz = vz + q.w*tz + (q.x*ty - q.y*tx);
}
// Display-local eye Z (matches macos/main.mm RigLocalEyeZ).
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y, -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv, eyeWorld.x - rig.position.x, eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z, &ox, &oy, &oz);
    return oz;
}

// Extract yaw/pitch from a quaternion (matches quat_from_yaw_pitch). Only used
// after a smooth pose transition completes so subsequent drag rotation feels
// natural. Ambiguous near the poles — acceptable for this demo (mirrors macOS).
static void yaw_pitch_from_quat(XrQuaternionf q, float* yaw, float* pitch) {
    float vx = 0, vy = 0, vz = -1.0f;
    float fwdX, fwdY, fwdZ;
    quat_rotate_vec3(q, vx, vy, vz, &fwdX, &fwdY, &fwdZ);
    *yaw = atan2f(-fwdX, -fwdZ);
    float clampedY = fwdY;
    if (clampedY > 1.0f) clampedY = 1.0f;
    if (clampedY < -1.0f) clampedY = -1.0f;
    *pitch = asinf(clampedY);
}

// ============================================================================
// Camera movement — ported from displayxr-common/common/input_handler.cpp
// (UpdateCameraMovement). Same speed rule, same auto-orbit rule, same
// camera-rig IPD ceiling; only the DirectXMath calls are swapped for the
// inline quaternion helpers above.
// ============================================================================
static void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM) {
    // SPACE — absolute, drift-free reset. The shared rule resets to the app's
    // initial DISPLAY-centric state; the model viewer then restores the framed
    // pose the auto-fit produced (windows/main.cpp:2670).
    if (state.resetViewRequested) {
        state.resetViewRequested = false;
        float pos[3] = {state.cameraPosX, state.cameraPosY, state.cameraPosZ};
        dxr::RigResetToInitial(state.viewParams, state.cameraMode, pos, state.yaw, state.pitch,
                               state.initialVirtualDisplayHeight);
        state.cameraPosX = pos[0];
        state.cameraPosY = pos[1];
        state.cameraPosZ = pos[2];
        if (g_fitValid) {
            state.cameraPosX = g_fitCenter[0];
            state.cameraPosY = g_fitCenter[1];
            state.cameraPosZ = g_fitCenter[2];
            state.yaw = g_fitYaw;
            state.pitch = g_fitPitch;
            state.viewParams.scaleFactor = g_fitZoom;
            state.viewParams.virtualDisplayHeight = g_fitVHeight;
        }
        state.transitioning = false;
        state.animationActive = false;
        state.lastInputTimeSec = NowSec();
        return;
    }

    // C — disturbance-free display<->camera rig round-trip (shared converter,
    // so Windows / macOS / Linux land on the identical rig).
    if (state.rigModeToggleRequested) {
        state.rigModeToggleRequested = false;
        XrQuaternionf ori;
        quat_from_yaw_pitch(state.yaw, state.pitch, &ori);
        const float quat[4] = {ori.x, ori.y, ori.z, ori.w};
        float pos[3] = {state.cameraPosX, state.cameraPosY, state.cameraPosZ};
        dxr::RigToggleMode(state.viewParams, state.cameraMode, pos, quat, state.canvasWidthM,
                           state.canvasHeightM, state.nominalViewerZ, displayHeightM);
        state.cameraPosX = pos[0];
        state.cameraPosY = pos[1];
        state.cameraPosZ = pos[2];
        state.lastInputTimeSec = NowSec();
        LOG_INFO("Rig mode -> %s", state.cameraMode ? "CAMERA" : "DISPLAY");
        return;
    }

    // Smooth pose transition (double-click focus). Overrides WASD while active.
    if (state.transitioning) {
        state.transitionT += deltaTime;
        float u = state.transitionT / state.transitionDuration;
        if (u >= 1.0f) u = 1.0f;
        const float invU = 1.0f - u;
        const float eased = 1.0f - invU * invU * invU;   // ease-out cubic
        XrPosef cur;
        display3d_pose_slerp(&state.transitionFrom, &state.transitionTo, eased, &cur);
        state.cameraPosX = cur.position.x;
        state.cameraPosY = cur.position.y;
        state.cameraPosZ = cur.position.z;
        yaw_pitch_from_quat(cur.orientation, &state.yaw, &state.pitch);
        if (u >= 1.0f) state.transitioning = false;
        return;
    }

    // Meters-to-virtual conversion (matches Kooima projection scaling).
    float m2v = 1.0f;
    if (state.viewParams.virtualDisplayHeight > 0.0f && displayHeightM > 0.0f)
        m2v = state.viewParams.virtualDisplayHeight / displayHeightM;
    const float moveSpeed = 0.1f * m2v / state.viewParams.scaleFactor;

    XrQuaternionf ori;
    quat_from_yaw_pitch(state.yaw, state.pitch, &ori);
    float fwdX, fwdY, fwdZ, rtX, rtY, rtZ, upX, upY, upZ;
    quat_rotate_vec3(ori, 0, 0, -1, &fwdX, &fwdY, &fwdZ);
    quat_rotate_vec3(ori, 1, 0, 0, &rtX, &rtY, &rtZ);
    quat_rotate_vec3(ori, 0, 1, 0, &upX, &upY, &upZ);

    const float d = moveSpeed * deltaTime;
    if (state.keyW) { state.cameraPosX += fwdX*d; state.cameraPosY += fwdY*d; state.cameraPosZ += fwdZ*d; }
    if (state.keyS) { state.cameraPosX -= fwdX*d; state.cameraPosY -= fwdY*d; state.cameraPosZ -= fwdZ*d; }
    if (state.keyD) { state.cameraPosX += rtX*d;  state.cameraPosY += rtY*d;  state.cameraPosZ += rtZ*d; }
    if (state.keyA) { state.cameraPosX -= rtX*d;  state.cameraPosY -= rtY*d;  state.cameraPosZ -= rtZ*d; }
    if (state.keyE) { state.cameraPosX += upX*d;  state.cameraPosY += upY*d;  state.cameraPosZ += upZ*d; }
    if (state.keyQ) { state.cameraPosX -= upX*d;  state.cameraPosY -= upY*d;  state.cameraPosZ -= upZ*d; }

    // Auto-orbit: enabled + idle > 10 s slowly yaws the display. Held while a
    // clip plays — the asset already carries its own motion. Holding the idle
    // clock at "now" restarts the countdown when playback pauses, so the
    // turntable doesn't snap the instant the user hits K (macOS parity).
    if (g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused()) {
        state.animationActive = false;
        state.lastInputTimeSec = NowSec();
    } else if (state.animateEnabled && state.lastInputTimeSec > 0.0) {
        const double idleFor = NowSec() - state.lastInputTimeSec;
        state.animationActive = (idleFor > 10.0);
        if (state.animationActive) {
            const float rate = 6.2831853f / 20.0f; // one revolution per 20 seconds
            state.yaw += rate * deltaTime;
        }
    } else {
        state.animationActive = false;
    }

    // Camera-rig IPD/parallax comfort ceiling (M = 1/f). Display mode keeps its
    // own [0,1] clamp in the wheel / +- handlers.
    if (state.cameraMode) {
        dxr_rig_display_info info = {displayHeightM, 1.0f, state.nominalViewerZ};
        dxr_camera_rig cam = {};
        cam.m2v = state.viewParams.cameraM2v;
        cam.inv_convergence_distance = state.viewParams.invConvergenceDistance;
        const float ipdMax = dxr_rig_max_ipd_factor(&cam, &info);
        if (state.viewParams.steadyIpdFactor > ipdMax) state.viewParams.steadyIpdFactor = ipdMax;
        if (state.viewParams.ipdFactor > ipdMax) state.viewParams.ipdFactor = ipdMax;
        if (state.viewParams.parallaxFactor > ipdMax) state.viewParams.parallaxFactor = ipdMax;
    }
}

// ============================================================================
// OpenXR session
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

    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    bool hasDisplayInfoExt = false;
    bool hasViewRigExt = false;
    bool hasXlibBindingExt = false;   //!< XR_DXR_xlib_window_binding (handle app, X11)
    bool hasWaylandBindingExt = false;//!< XR_DXR_wayland_surface_binding (handle app, Wayland)
    //! The window platform resolved before xrCreateInstance (only its binding
    //! extension is enabled). Auto = no usable window system: hosted-NULL.
    DxrWindowBackend windowBackend = DxrWindowBackend::Auto;
    bool hasAtlasCaptureExt = false;  //!< XR_DXR_atlas_capture ('I')
    bool hasWeaveExt = false;         //!< XR_DXR_weave (drag phase-snap)
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerZ = 0.5f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
    int32_t displayScreenLeft = 0;     // 3D-panel top-left in virtual-desktop px (INV-1.3)
    int32_t displayScreenTop = 0;

    //! An app-owned window exists (g_window); false = hosted-NULL fallback.
    //! The window's CONTENT is what is bound to the runtime — the header bar
    //! never enters the canvas, the swapchain or the weave.
    bool hasAppWindow = false;

    PFN_xrRequestDisplayRenderingModeDXR pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateDisplayRenderingModesEXT = nullptr;
    PFN_xrRequestEyeTrackingModeDXR pfnRequestEyeTrackingModeEXT = nullptr;
    PFN_xrCaptureAtlasDXR pfnCaptureAtlasEXT = nullptr;
    //! XR_DXR_weave's window-origin phase snap. Resolved defensively: the
    //! desktop-Linux runtime does not advertise XR_DXR_weave yet (the plumbing
    //! is runtime#1588 / PR#1592), so this stays null and the drag runs
    //! unsnapped. NEVER a hard requirement.
    PFN_xrWeaveSnapWindowRectDXR pfnWeaveSnapWindowRect = nullptr;

    XrEyeTrackingModeDXR activeEyeTrackingMode = XR_EYE_TRACKING_MODE_MANAGED_DXR;

    uint32_t renderingModeCount = 0;
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    uint32_t currentRenderingMode = 1;
    //! Array index of the mode the runtime reports isActive at enumeration
    //! (-1 = none). The app renders whatever mode the DISPLAY is in — 2, 4, or
    //! N views — instead of hard-coding a 2-view mode, so it is view-config
    //! agnostic (a forced Quad display renders a 4-tile atlas, etc.).
    int32_t activeRenderingMode = -1;

    uint32_t maxViewCount = 2;
};

static bool InitializeOpenXR(AppXrSession& xr, DxrWindowBackend requestedBackend) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(e.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(e.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) xr.hasViewRigExt = true;
        if (strcmp(e.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasXlibBindingExt = true;
        if (strcmp(e.extensionName, XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME) == 0) xr.hasWaylandBindingExt = true;
        if (strcmp(e.extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0) xr.hasAtlasCaptureExt = true;
        if (strcmp(e.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0) xr.hasWeaveExt = true;
    }
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasViewRigExt) enabled.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    // Handle app: resolve the window platform BEFORE xrCreateInstance, so
    // exactly the binding extension the session will chain gets ENABLED.
    // Capability-probed (a connection attempt + what the server advertises),
    // never read from the session environment; an explicit --platform wins.
    // Nothing usable: hosted-NULL (no window binding).
    {
        std::string why;
        xr.windowBackend = DxrLinuxWindow::select(requestedBackend, xr.hasXlibBindingExt,
                                                  xr.hasWaylandBindingExt, &why);
        if (xr.windowBackend == DxrWindowBackend::Auto) {
            LOG_WARN("No usable window platform (%s) — hosted-NULL windowing", why.c_str());
        } else {
            LOG_INFO("Window platform: %s (requested %s) — %s", DxrLinuxWindow::backend_name(xr.windowBackend),
                     DxrLinuxWindow::backend_name(requestedBackend), why.c_str());
        }
    }
    if (xr.windowBackend == DxrWindowBackend::X11) enabled.push_back(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.windowBackend == DxrWindowBackend::Wayland)
        enabled.push_back(XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME);
    if (xr.hasAtlasCaptureExt) enabled.push_back(XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME);
    // XR_DXR_weave carries xrWeaveSnapWindowRectDXR, the drag phase-snap the
    // client-owned window move routes every step through. Purely optional:
    // desktop Linux does not advertise it yet (runtime#1588), and the drag then
    // runs unsnapped rather than failing.
    if (xr.hasWeaveExt) enabled.push_back(XR_DXR_WEAVE_EXTENSION_NAME);
    LOG_INFO("XR_DXR_view_rig: %s", xr.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_xlib_window_binding: %s", xr.hasXlibBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_wayland_surface_binding: %s", xr.hasWaylandBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_atlas_capture: %s", xr.hasAtlasCaptureExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_weave (drag phase-snap): %s", xr.hasWeaveExt ? "AVAILABLE" : "NOT FOUND");

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "DisplayXRModelViewerLinux",
            sizeof(ci.applicationInfo.applicationName) - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName) - 1);
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
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        di.next = &desktopPos;
        sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
            xr.displayScreenLeft = desktopPos.left;
            xr.displayScreenTop = desktopPos.top;
        }
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR",
                              (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
                              (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
        xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeDXR",
                              (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);
    }
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasDXR",
                              (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
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

// Parse a space-separated extension list from an xrGetVulkan*ExtensionsKHR call.
static void SplitExtList(const std::string& s, std::vector<std::string>& out) {
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(' ', i); if (e == std::string::npos) e = s.size();
        std::string n = s.substr(i, e - i);
        if (!n.empty() && n[0] != '\0') out.push_back(n);
        i = e + 1;
    }
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames; SplitExtList(extStr, extNames);
    // Linux: native Vulkan ICD — no MoltenVK portability enumeration (that's a
    // macOS-only requirement; adding it here would fail on a stock Linux loader).
    std::vector<const char*> extPtrs;
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "DisplayXRModelViewerLinux";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
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
                                      std::vector<std::string>& storage) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    SplitExtList(extStr, storage);
    // No VK_KHR_portability_subset on Linux (MoltenVK-only).
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++)
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
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

// hosted-NULL: chain ONLY the Vulkan graphics binding — no window binding, so
// the runtime self-creates the presentation window.
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
                          VkDevice dev, uint32_t qfi) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    // Handle app: the window helper hands back the binding for whichever
    // platform it is on (XR_DXR_xlib_window_binding, or
    // XR_DXR_wayland_surface_binding + its surface-geometry struct), bound to
    // the CONTENT (never the header bar), with transparentBackgroundEnabled
    // set when the window is transparent-capable — exactly as
    // windows/xr_session.cpp sets it unconditionally: the runtime picks the
    // swapchain's compositeAlpha here and cannot change it later, so Ctrl+T
    // can only work if the session was created this way. Opaque mode then
    // writes alpha = 1 throughout, which a PRE_MULTIPLIED surface composites
    // identically to a non-transparent session. hosted-NULL: Vulkan only.
    const bool useAppWindow = xr.hasAppWindow;

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = useAppWindow ? g_window.session_binding_chain(&vkBinding) : (const void*)&vkBinding;
    si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));
    if (useAppWindow) {
        // Arms the per-frame geometry feed (Wayland); a no-op on X11.
        g_window.attach_session(xr.instance, xr.session);
    }
    LOG_INFO("Session created (%s%s)", useAppWindow ? g_window.describe().c_str() : "hosted-NULL",
             (useAppWindow && g_transparentCapable)
                 ? (g_transparentBg ? ", transparent-capable — starting TRANSPARENT"
                                    : ", transparent-capable — starting opaque (Ctrl+T toggles)")
                 : ", opaque only (no Ctrl+T)");

    // Drag-time window-origin phase snap (runtime#1588). Resolved here and
    // used by the client-owned RMB drag; identity when the runtime does not
    // serve it, which is the case on desktop Linux until PR#1592 lands.
    if (useAppWindow) {
        uint32_t cw = 0, ch = 0;
        g_window.current_size(&cw, &ch);
        g_weaveSnap.attach(xr.instance, xr.session, cw, ch);
        g_window.set_snap_provider(&DxrWeaveSnap::callback, &g_weaveSnap);
        LOG_INFO("xrWeaveSnapWindowRectDXR: %s — a window drag %s",
                 g_weaveSnap.available() ? "RESOLVED" : "unavailable on this runtime",
                 g_weaveSnap.available()
                     ? "will be phase-snapped by the display processor"
                     : "lands on the raw pointer position (identity snap)");
    }

    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        if (XR_SUCCEEDED(xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr))
            && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR; modes[i].next = nullptr;
            }
            if (XR_SUCCEEDED(xr.pfnEnumerateDisplayRenderingModesEXT(
                    xr.session, modeCount, &modeCount, modes.data()))) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
                    if (modes[i].isActive == XR_TRUE) xr.activeRenderingMode = (int32_t)i;
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

    // Size the swapchain to fit the largest atlas across all advertised modes
    // (mirrors macos/main.mm — worst-case-sized, per ADR-010).
    uint32_t w = views.empty() ? 1280 : views[0].recommendedImageRectWidth * 2;
    uint32_t h = views.empty() ? 720  : views[0].recommendedImageRectHeight;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        w = xr.displayPixelWidth; h = xr.displayPixelHeight;
        for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
            uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] * xr.renderingModeScaleX[i] * (double)xr.displayPixelWidth);
            uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] * xr.renderingModeScaleY[i] * (double)xr.displayPixelHeight);
            if (aw > w) w = aw;
            if (ah > h) h = ah;
        }
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // TRANSFER_SRC is what lets the click-through pass downscale-blit the
    // frame's own alpha out of the atlas (linux/clickthrough.cpp) instead of
    // re-rendering the model into a scratch raster. The compositor happens to
    // add this bit unconditionally today (comp_swapchain.c), but relying on
    // that would be relying on someone else's implementation detail.
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;
    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, (long long)selectedFmt);
    return true;
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
            // Runtime / workspace controller switched rendering mode on us —
            // follow it so the per-frame re-assert tracks the display's actual
            // mode instead of fighting it (keeps the app view-config agnostic).
            auto* rmc = (XrEventDataRenderingModeChangedDXR*)&event;
            if (rmc->currentModeIndex < xr.renderingModeCount) {
                xr.currentRenderingMode = rmc->currentModeIndex;
                LOG_INFO("Rendering mode changed by runtime -> %u", rmc->currentModeIndex);
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(xr.session, nullptr, &fs))) return false;
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
                     XrCompositionLayerProjectionView* projViews, uint32_t viewCount) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
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
    xr.swapchain.swapchain = XR_NULL_HANDLE;
    xr.localSpace = XR_NULL_HANDLE;
    xr.session = XR_NULL_HANDLE;
    xr.instance = XR_NULL_HANDLE;
}

// ============================================================================
// App-owned window (handle app) — displayxr::linux_window, X11 or Wayland
// ============================================================================

static const unsigned int kDefaultWindowW = 1920;
static const unsigned int kDefaultWindowH = 1080;

// Create the app-owned window for the viewer.
//
// Two shapes, per INV-1.3:
//   * PANEL-SIZED (MODEL_WINDOW asks for exactly the panel's pixel size) —
//     genuinely FULLSCREEN on the panel's monitor/output (the helper's X11
//     EWMH recipe; on Wayland set_fullscreen on the panel's wl_output once the
//     surface is mapped).
//   * WINDOWED (the default: 1920x1080 centred on the panel) — a client-side
//     header bar, and a drag the app owns on X11 so every move goes through
//     xrWeaveSnapWindowRectDXR first (runtime#1588). Right-button drag moves
//     the window anywhere (the dxr::RmbWindowDrag convention the Windows
//     borderless overlay uses) — the LEFT button stays the model orbit, as on
//     Windows and macOS. DXR_X11_WM_DECORATIONS=1 restores a decorated,
//     WM-dragged X11 window (and gives up the phase snap).
//
// MODEL_WINDOW="WxH+X+Y" overrides the size/position (X,Y absolute
// virtual-desktop px, X11 only; WxH alone re-centres on the panel).
// Returns false when no window could be made; the caller then falls back to
// hosted-NULL (also the CI-safe path).
static bool CreateAppWindow(AppXrSession& xr) {
    if (xr.windowBackend == DxrWindowBackend::Auto) return false;

    unsigned int w = kDefaultWindowW, h = kDefaultWindowH;
    const bool panelKnown = xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0;
    const int prx = xr.displayScreenLeft, pry = xr.displayScreenTop;
    const int prw = (int)xr.displayPixelWidth, prh = (int)xr.displayPixelHeight;
    bool explicitPos = false;
    int px = 0, py = 0;

    // MODEL_WINDOW="WxH+X+Y" override (X,Y absolute virtual-desktop px); WxH
    // alone re-centres on the panel.
    if (const char* wenv = getenv("MODEL_WINDOW")) {
        unsigned int ow = 0, oh = 0; int ox = 0, oy = 0;
        int n = sscanf(wenv, "%ux%u+%d+%d", &ow, &oh, &ox, &oy);
        if (n >= 2 && ow > 0 && oh > 0) {
            w = ow; h = oh;
            if (n >= 4) { explicitPos = true; px = ox; py = oy; }
            LOG_INFO("MODEL_WINDOW override: %ux%u%s", w, h, n >= 4 ? " at an absolute position" : "");
        }
    }
    if (!explicitPos && panelKnown) {
        px = prx + (prw - (int)w) / 2;
        py = pry + (prh - (int)h) / 2;
        LOG_INFO("3D panel (display_info) %dx%d at (%d,%d) — centering %ux%u window at (%d,%d)",
                 prw, prh, prx, pry, w, h, px, py);
    }

    DxrLinuxWindowDesc desc;
    desc.width = w;
    desc.height = h;
    desc.panel_left = prx;
    desc.panel_top = pry;
    desc.panel_width = (uint32_t)prw;
    desc.panel_height = (uint32_t)prh;
    desc.title = "DisplayXR 3D Model Viewer";
    desc.app_id = "com.displayxr.modelviewer";
    desc.transparent = g_transparentCapable;
    desc.x11_header_bar = true;      // the snapped drag needs a client-side bar on X11
    desc.x11_drag_button = 3;        // RMB drags; LMB stays the model orbit
    desc.wayland_drag_button = 3;    // ...the same gesture on Wayland (compositor move)
    desc.keep_above = g_transparentCapable && g_transparentBg;
    desc.transparent_background = g_transparentCapable && g_transparentBg;   // no header bar while transparent
    desc.has_position = explicitPos || panelKnown;
    desc.x = px;
    desc.y = py;
    // Panel-sized = fullscreen on the panel, on Wayland as on X11 (INV-1.3).
    desc.fullscreen_on_wayland = panelKnown && (int)w == prw && (int)h == prh;

    if (!g_window.create(xr.windowBackend, desc)) {
        LOG_WARN("%s window creation failed — using hosted-NULL windowing",
                 DxrLinuxWindow::backend_name(xr.windowBackend));
        return false;
    }
    xr.hasAppWindow = true;
    g_windowTitle = desc.title;
    if (g_transparentCapable && !g_window.is_transparent()) {
        // X11 screen without an ARGB visual: the handle path stays (it is what
        // weaves window-relative); only transparency is lost.
        g_transparentCapable = false;
        g_transparentBg = false;
    }
    uint32_t cw = 0, ch = 0;
    g_window.current_size(&cw, &ch);
    LOG_INFO("Created %ux%u %s window on %s — %s", cw, ch,
             g_window.is_transparent() ? "transparent-capable" : "opaque",
             g_window.connection_description().c_str(),
             (g_window.is_fullscreen() || desc.fullscreen_on_wayland && xr.windowBackend == DxrWindowBackend::Wayland)
                 ? "fullscreen on the panel" : "windowed, header bar + RMB drag");
    return true;
}

// ============================================================================
// Bundled-scene auto-load
// ============================================================================

static std::string ExeDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return std::string(dirname(buf));
}
static bool FileExists(const std::string& p) {
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static void ApplyAutoFitForLoadedScene() {
    float center[3], extent[3];
    const bool ok = g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent);
    if (ok) {
        g_fitCenter[0] = center[0]; g_fitCenter[1] = center[1]; g_fitCenter[2] = center[2];
        // Viewport: the app-owned X window when we have one, else the runtime's
        // presentation surface (the panel). Only its aspect matters.
        const float viewportW = (float)g_windowW, viewportH = (float)g_windowH;
        float sweptW = 0.0f;
        float vh = modelviewer::FitVHeight(extent, viewportW, viewportH, &sweptW);
        if (!(vh > 1e-3f)) vh = kDefaultVirtualDisplayHeightM;
        // --vh wins over the fit: the caller is asserting the scale the asset
        // was authored at; auto-fit's job is to guess one when nobody said.
        if (g_vhOverride > 0.0f) {
            LOG_INFO("Auto-fit: vHeight pinned to %.3f m by --vh (fit would have used %.3f)",
                     g_vhOverride, vh);
            vh = g_vhOverride;
        }
        g_fitVHeight = vh;
        g_fitYaw = 0.0f; g_fitPitch = 0.0f; g_fitZoom = 1.0f;
        const bool haveViewport = (viewportW > 0.0f && viewportH > 0.0f);
        const float aspect = haveViewport ? (viewportW / viewportH) : 0.0f;
        const char* boundBy = !haveViewport
                            ? "height (no viewport)"
                            : modelviewer::FitBoundBy(sweptW, extent[1], extent[2], aspect);
        LOG_INFO("Auto-fit: center=(%.3f,%.3f,%.3f) extent W=%.3f H=%.3f D=%.3f swept-W=%.3f "
                 "viewport=%.0fx%.0f (aspect %.3f) bound-by=%s vHeight=%.3f",
                 center[0], center[1], center[2], extent[0], extent[1], extent[2], sweptW,
                 viewportW, viewportH, aspect, boundBy, vh);
    } else {
        g_fitCenter[0] = g_fitCenter[1] = g_fitCenter[2] = 0.0f;
        g_fitVHeight = (g_vhOverride > 0.0f) ? g_vhOverride : kDefaultVirtualDisplayHeightM;
        g_fitYaw = 0.0f; g_fitPitch = 0.0f; g_fitZoom = 1.0f;
    }
    g_fitValid = ok;

    // Land the live rig on the framed pose (SPACE returns to exactly this).
    g_input.cameraPosX = g_fitCenter[0];
    g_input.cameraPosY = g_fitCenter[1];
    g_input.cameraPosZ = g_fitCenter[2];
    g_input.yaw = g_fitYaw;
    g_input.pitch = g_fitPitch;
    g_input.viewParams.virtualDisplayHeight = g_fitVHeight;
    g_input.viewParams.scaleFactor = g_fitZoom;
    g_input.initialVirtualDisplayHeight = g_fitVHeight;
    g_input.transitioning = false;
}

// A bundled environment.hdr, when present next to the exe, becomes the IBL
// source at startup (issue #70). Absent, the viewer keeps the procedural
// analytic sky — purely additive, and the HUD/log names whichever is active so
// a reference capture is self-documenting.
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

static void TryAutoLoadBundledScene() {
    std::string dir = ExeDir();
    if (dir.empty()) return;
    std::string path = dir + "/sample.glb";
    if (!FileExists(path)) { LOG_INFO("No bundled model at %s (skipping)", path.c_str()); return; }
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
// File-open dialog (O / Ctrl+O) — async zenity picker + X11 event pump
// ============================================================================

// Non-blocking zenity file-selection: fork/exec with a pipe, polled every
// frame so the XR frame loop never stalls. zenity is NOT a hard dependency —
// when absent the child exits 127 and we log a hint. Windows-parity semantics:
// pick a file → validate → g_modelRenderer.loadModel → re-run auto-fit.
static pid_t g_pickerPid = -1;
static int g_pickerFd = -1;
static std::string g_pickerBuf;

static void StartFilePicker() {
    if (g_pickerPid > 0) return; // dialog already open
    int fds[2];
    if (pipe(fds) != 0) { LOG_WARN("file picker: pipe() failed"); return; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); LOG_WARN("file picker: fork() failed"); return; }
    if (pid == 0) {
        // Child: zenity prints the chosen path on stdout.
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        std::string startDir = ExeDir(); // bundled models (sample.glb, Fox.glb) live here
        if (!startDir.empty() && startDir.back() != '/') startDir += '/';
        std::string filenameArg = "--filename=" + startDir;
        const char* argv[] = {"zenity", "--file-selection", "--title=Open 3D model",
                              filenameArg.c_str(),
                              "--file-filter=3D models | *.glb *.gltf *.obj *.stl *.fbx *.usdz *.GLB *.GLTF",
                              "--file-filter=All files | *", nullptr};
        execvp("zenity", const_cast<char* const*>(argv));
        _exit(127); // zenity not installed
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    g_pickerPid = pid;
    g_pickerFd = fds[0];
    g_pickerBuf.clear();
    LOG_INFO("file picker: zenity dialog opened");
}

static void PollFilePicker() {
    if (g_pickerPid <= 0) return;
    // Drain whatever the child has written so far.
    char buf[512];
    ssize_t n;
    while ((n = read(g_pickerFd, buf, sizeof(buf))) > 0) g_pickerBuf.append(buf, (size_t)n);
    int status = 0;
    pid_t r = waitpid(g_pickerPid, &status, WNOHANG);
    if (r != g_pickerPid) return; // still open
    while ((n = read(g_pickerFd, buf, sizeof(buf))) > 0) g_pickerBuf.append(buf, (size_t)n);
    close(g_pickerFd);
    g_pickerFd = -1; g_pickerPid = -1;

    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 127) { LOG_WARN("file picker: zenity not installed (apt install zenity)"); return; }
    if (code != 0) { LOG_INFO("file picker: cancelled"); return; }
    // Trim the trailing newline zenity appends.
    while (!g_pickerBuf.empty() && (g_pickerBuf.back() == '\n' || g_pickerBuf.back() == '\r'))
        g_pickerBuf.pop_back();
    if (g_pickerBuf.empty()) return;
    if (!model_validate_file(g_pickerBuf)) { LOG_WARN("file picker: unsupported file '%s'", g_pickerBuf.c_str()); return; }
    LOG_INFO("Loading model: %s", g_pickerBuf.c_str());
    if (g_modelRenderer.loadModel(g_pickerBuf.c_str())) {
        g_loadedFileName = model_basename(g_pickerBuf);
        ApplyAutoFitForLoadedScene();
        LOG_INFO("Loaded %s", g_loadedFileName.c_str());
    } else {
        LOG_WARN("file picker: load failed for %s", g_pickerBuf.c_str());
    }
}

// ============================================================================
// Input — the Linux transliteration of the Win32 WndProc + displayxr-common's
// UpdateInputState, fed by displayxr::linux_window's event stream (the same on
// X11 and Wayland: X11 keysyms at level 0, content-relative buffer pixels).
// Sensitivities, signs and clamps are copied from input_handler.cpp verbatim
// so all three legs feel identical.
//
//   LMB drag      orbit the model   (yaw -= dx*0.005, pitch -= dy*0.005, |pitch| <= 1.4)
//   LMB dbl-click focus/teleport on the surface under the cursor
//   RMB drag      MOVE THE WINDOW (the helper's; phase-snapped on X11)
//   wheel         zoom / IPD (Shift) / parallax (Ctrl) / perspective (Alt)
//   keys          see the table in the startup banner
//
// WHY RMB MOVES THE WINDOW HERE AND NOT ON WINDOWS. On Windows the OS modal
// move loop gives the display processor a WM_WINDOWPOSCHANGING hook, so a
// normal title-bar drag is phase-snapped for free and the app needs no gesture
// at all. X11 has no such hook and a mutter _NET_WM_MOVERESIZE grab cannot be
// intercepted, so the app must own the drag. Right-button drag is the
// DisplayXR convention for exactly this case (dxr::RmbWindowDrag, the Windows
// borderless overlay + the Unity desktop-avatar sample), and it leaves the
// LEFT button free for the model orbit, which is the behaviour that has to
// match Windows. The header bar's LMB drag, close and minimize, the RMB drag
// itself and F11 are handled INSIDE the window helper; the app only sees the
// RMB press/release (flagged window_drag) for its button state.
// ============================================================================

//! Multiplicative wheel step, matching WM_MOUSEWHEEL's 1.1 / (1/1.1).
static void ApplyWheel(InputState& st, bool up, uint32_t mods) {
    MarkUserInput(st);
    const float factor = up ? 1.1f : (1.0f / 1.1f);
    const bool shift = (mods & DxrModShift) != 0;
    const bool ctrl  = (mods & DxrModCtrl) != 0;
    const bool alt   = (mods & DxrModAlt) != 0;
    if (shift) {
        float v = st.viewParams.steadyIpdFactor * factor;
        if (v < 0.0f) v = 0.0f;
        if (!st.cameraMode && v > 1.0f) v = 1.0f;
        st.viewParams.steadyIpdFactor = v;
        st.viewParams.ipdFactor = v;
        st.viewParams.parallaxFactor = v;
    } else if (ctrl) {
        st.viewParams.parallaxFactor *= factor;
        if (st.viewParams.parallaxFactor < 0.0f) st.viewParams.parallaxFactor = 0.0f;
        if (st.viewParams.parallaxFactor > 1.0f) st.viewParams.parallaxFactor = 1.0f;
    } else if (alt) {
        if (st.cameraMode) {
            st.viewParams.invConvergenceDistance *= factor;
            if (st.viewParams.invConvergenceDistance < 0.1f) st.viewParams.invConvergenceDistance = 0.1f;
            if (st.viewParams.invConvergenceDistance > 10.0f) st.viewParams.invConvergenceDistance = 10.0f;
        } else {
            st.viewParams.perspectiveFactor *= factor;
            if (st.viewParams.perspectiveFactor < 0.1f) st.viewParams.perspectiveFactor = 0.1f;
            if (st.viewParams.perspectiveFactor > 1.0f) st.viewParams.perspectiveFactor = 1.0f;
        }
    } else {
        if (st.cameraMode) {
            st.viewParams.zoomFactor *= factor;
            if (st.viewParams.zoomFactor < 0.1f) st.viewParams.zoomFactor = 0.1f;
            if (st.viewParams.zoomFactor > 10.0f) st.viewParams.zoomFactor = 10.0f;
        } else {
            st.viewParams.scaleFactor *= factor;
            if (st.viewParams.scaleFactor < 0.1f) st.viewParams.scaleFactor = 0.1f;
            if (st.viewParams.scaleFactor > 10.0f) st.viewParams.scaleFactor = 10.0f;
        }
    }
}

static void HandleWindowEvent(const DxrWindowEvent& ev) {
    switch (ev.type) {

    case DxrWindowEvent::Type::KeyDown: {
        const KeySym sym = (KeySym)ev.keysym;
        const bool ctrl = (ev.mods & DxrModCtrl) != 0;
        MarkUserInput(g_input);

        // --- chords first (they shadow the bare key below) ------------------
        // Ctrl+O = open a model (uniform across demos + platforms, incl. the
        // mediaplayer). Strict: Ctrl must be held.
        if ((sym == XK_o || sym == XK_O) && ctrl) { StartFilePicker(); break; }
        // Ctrl+T = transparent background. Must be checked BEFORE the bare
        // switch below or plain-T (eye-tracking mode) swallows it — the same
        // ordering displayxr-common's input_handler.cpp uses on Windows. A
        // request flag, not a direct flip: the render loop stays the single
        // owner of the transition, as on Windows.
        if ((sym == XK_t || sym == XK_T) && ctrl) {
            g_input.transparentBgToggleRequested = true;
            break;
        }

            switch (sym) {
            // Movement (held)
            case XK_w: case XK_W: g_input.keyW = true; break;
            case XK_a: case XK_A: g_input.keyA = true; break;
            case XK_s: case XK_S: g_input.keyS = true; break;
            case XK_d: case XK_D: g_input.keyD = true; break;
            case XK_e: case XK_E: g_input.keyE = true; break;
            case XK_q: case XK_Q: g_input.keyQ = true; break;

            case XK_Escape: LOG_INFO("ESC — exiting"); g_running = false; break;
            case XK_F11:    break; // toggled inside the window helper's pump
            case XK_space:  g_input.resetViewRequested = true; break;
            case XK_Tab:    g_input.hudVisible = !g_input.hudVisible; break;

            // Rendering mode: V cycles, 0-8 jump (routed through dxr::ModeSwitch).
            case XK_v: case XK_V: g_input.cycleRenderingModeRequested = true; break;
            case XK_0: g_input.absoluteRenderingModeRequested = 0; break;
            case XK_1: if (g_input.renderingModeCount > 1) g_input.absoluteRenderingModeRequested = 1; break;
            case XK_2: if (g_input.renderingModeCount > 2) g_input.absoluteRenderingModeRequested = 2; break;
            case XK_3: if (g_input.renderingModeCount > 3) g_input.absoluteRenderingModeRequested = 3; break;
            case XK_4: if (g_input.renderingModeCount > 4) g_input.absoluteRenderingModeRequested = 4; break;
            case XK_5: if (g_input.renderingModeCount > 5) g_input.absoluteRenderingModeRequested = 5; break;
            case XK_6: if (g_input.renderingModeCount > 6) g_input.absoluteRenderingModeRequested = 6; break;
            case XK_7: if (g_input.renderingModeCount > 7) g_input.absoluteRenderingModeRequested = 7; break;
            case XK_8: if (g_input.renderingModeCount > 8) g_input.absoluteRenderingModeRequested = 8; break;

            case XK_t: case XK_T: g_input.eyeTrackingModeToggleRequested = true; break;
            case XK_c: case XK_C: g_input.rigModeToggleRequested = true; break;
            case XK_i: case XK_I: g_input.captureAtlasRequested = true; break;
            case XK_m: case XK_M: g_input.animateToggleRequested = true; break;
            case XK_n: case XK_N: g_input.cycleClipRequested = true; break;
            case XK_k: case XK_K: g_input.playPauseRequested = true; break;

            // 3D-effect strength, in 0.1 steps (VK_OEM_MINUS / VK_OEM_PLUS).
            case XK_minus: case XK_underscore: case XK_KP_Subtract: {
                float v = g_input.viewParams.steadyIpdFactor - 0.1f;
                if (v < 0.1f) v = 0.1f;
                g_input.viewParams.steadyIpdFactor = v;
                g_input.viewParams.ipdFactor = v;
                g_input.viewParams.parallaxFactor = v;
                LOG_INFO("Depth/IPD: %.0f%%", v * 100.0f);
                break;
            }
            case XK_equal: case XK_plus: case XK_KP_Add: {
                float v = g_input.viewParams.steadyIpdFactor + 0.1f;
                if (!g_input.cameraMode && v > 1.0f) v = 1.0f;
                g_input.viewParams.steadyIpdFactor = v;
                g_input.viewParams.ipdFactor = v;
                g_input.viewParams.parallaxFactor = v;
                LOG_INFO("Depth/IPD: %.0f%%", v * 100.0f);
                break;
            }

            // Viewing conditions (issue #70): [ / ] step exposure in quarter
            // stops, G cycles the tone curve, L cycles the lighting mode. All
            // three are logged so a reference capture records its grading.
            case XK_bracketleft: case XK_bracketright: {
                const float step = (sym == XK_bracketright) ? 0.25f : -0.25f;
                g_modelRenderer.setExposureEV(g_modelRenderer.exposureEV() + step);
                LOG_INFO("Exposure: %+.2f EV", g_modelRenderer.exposureEV());
                break;
            }
            case XK_g: case XK_G:
                g_modelRenderer.cycleToneCurve();
                LOG_INFO("Tone curve: %s", g_modelRenderer.toneCurveName());
                break;
            case XK_l: case XK_L:
                if (!ctrl) {
                    g_modelRenderer.cycleLightingMode();
                    LOG_INFO("Lighting: %s (env %s, exposure %+.2f EV, tone %s)",
                             g_modelRenderer.lightingModeName(),
                             g_modelRenderer.environmentName().c_str(),
                             g_modelRenderer.exposureEV(), g_modelRenderer.toneCurveName());
                }
                break;

            // Dynamic-recenter pins: P arms, then X/Y/Z toggle that axis.
            case XK_p: case XK_P:
            case XK_x: case XK_X:
            case XK_y: case XK_Y:
            case XK_z: case XK_Z: {
                // onKey() wants the uppercase letter.
                char k = (char)('A' + (int)((sym >= XK_a) ? (sym - XK_a) : (sym - XK_A)));
                if (g_recenter.onKey(k)) {
                    char lbl[24];
                    g_recenter.hudLabel(lbl, sizeof(lbl));
                    LOG_INFO("Recenter: %s", lbl);
                }
                break;
            }
            default: break;
            }
        break;
    }

    case DxrWindowEvent::Type::KeyUp: {
        switch ((KeySym)ev.keysym) {
        case XK_w: case XK_W: g_input.keyW = false; break;
        case XK_a: case XK_A: g_input.keyA = false; break;
        case XK_s: case XK_S: g_input.keyS = false; break;
        case XK_d: case XK_D: g_input.keyD = false; break;
        case XK_e: case XK_E: g_input.keyE = false; break;
        case XK_q: case XK_Q: g_input.keyQ = false; break;
        default: break;
        }
        break;
    }

    case DxrWindowEvent::Type::Scroll: {
        // One multiplicative step per notch (X11 buttons 4/5, Wayland clicks).
        const int n = ev.scroll_steps;
        for (int i = 0; i < (n > 0 ? n : -n); i++) ApplyWheel(g_input, n > 0, ev.mods);
        break;
    }

    case DxrWindowEvent::Type::ButtonDown: {
        MarkUserInput(g_input);
        g_input.mouseX = ev.x;
        g_input.mouseY = ev.y;
        if (ev.button == 1) {
            // Double-click = focus/teleport. No WM_LBUTTONDBLCLK here, so
            // detect it: same button within 400 ms and 5 px (Win32's default
            // GetDoubleClickTime is 500 ms / SM_CXDOUBLECLK 4 px).
            static uint32_t s_lastClickTime = 0;
            static int s_lastClickX = 0, s_lastClickY = 0;
            const bool dbl = s_lastClickTime != 0 && (ev.time_ms - s_lastClickTime) < 400 &&
                             abs(ev.x - s_lastClickX) <= 5 && abs(ev.y - s_lastClickY) <= 5;
            s_lastClickTime = ev.time_ms;
            s_lastClickX = ev.x; s_lastClickY = ev.y;
            if (dbl) {
                g_input.teleportRequested = true;
                g_input.teleportMouseX = (float)ev.x;
                g_input.teleportMouseY = (float)ev.y;
                s_lastClickTime = 0; // a triple click is not two doubles
                break;
            }
            g_input.leftButton = true;
            g_input.dragging = true;
            g_input.dragStartX = g_input.mouseX;
            g_input.dragStartY = g_input.mouseY;
        } else if (ev.button == 3) {
            g_input.rightButton = true;   // the helper starts the window drag (ev.window_drag)
        } else if (ev.button == 2) {
            g_input.middleButton = true;  // tracked only, as on Windows
        }
        break;
    }

    case DxrWindowEvent::Type::ButtonUp:
        if (ev.button == 1) {
            g_input.leftButton = false;
            g_input.dragging = false;
        } else if (ev.button == 3) {
            g_input.rightButton = false;
        } else if (ev.button == 2) {
            g_input.middleButton = false;
        }
        break;

    case DxrWindowEvent::Type::Motion:
        g_input.mouseX = ev.x;
        g_input.mouseY = ev.y;
        if (g_input.dragging) {
            MarkUserInput(g_input);
            const int dx = g_input.mouseX - g_input.dragStartX;
            const int dy = g_input.mouseY - g_input.dragStartY;
            g_input.yaw   -= dx * 0.005f;
            g_input.pitch -= dy * 0.005f;
            if (g_input.pitch >  1.4f) g_input.pitch =  1.4f;   // gimbal-lock clamp
            if (g_input.pitch < -1.4f) g_input.pitch = -1.4f;
            g_input.dragStartX = g_input.mouseX;
            g_input.dragStartY = g_input.mouseY;
        }
        break;

    case DxrWindowEvent::Type::Resize:
        // The CONTENT size (the bound window/surface, bar excluded): the tile
        // basis, and the snap extent handed to the display processor.
        g_windowW = ev.width;
        g_windowH = ev.height;
        g_weaveSnap.set_extent(ev.width, ev.height);
        break;

    default: break;
    }
}

//! Drain the window once per frame: input, the helper-owned drag / header
//! bar / F11, and the Ctrl+T side effects.
static void PumpWindow(AppXrSession& xr) {
    if (!xr.hasAppWindow) return;
    bool running = true;
    g_window.pump_events(HandleWindowEvent, &running);
    if (!running) {
        LOG_INFO("Window closed — exiting");
        g_running = false;
    }

    // Header bar title: the loaded file, like the other legs.
    std::string title = "3D Model Viewer";
    if (!g_loadedFileName.empty()) title += " \u2014 " + g_loadedFileName;
    if (title != g_windowTitle) {
        g_windowTitle = title;
        g_window.set_title(title.c_str());
    }

    if (g_input.transparentBgToggleRequested) {
        g_input.transparentBgToggleRequested = false;
        if (!g_transparentCapable) {
            // Refuse loudly. The session's transparency was fixed at
            // xrCreateSession; flipping the flag now would clear to alpha 0
            // into an OPAQUE surface, i.e. a black window, not a see-through
            // one. Better an explanation than a convincing-looking bug.
            LOG_WARN("Ctrl+T ignored — this session is not transparent-capable "
                     "(MODEL_TRANSPARENT=0, no 32-bit ARGB visual, or hosted-NULL). "
                     "Restart without MODEL_TRANSPARENT=0 to enable it.");
        } else {
            g_transparentBg = !g_transparentBg;
            LOG_INFO("Transparent background: %s (Ctrl+T)", g_transparentBg ? "ON" : "OFF");
            // The header bar hides while transparent (Windows parity); the
            // content rect is unchanged, so the runtime sees no move.
            g_window.set_transparent_background(g_transparentBg);
            // Windows parity: transparent floats above other apps, opaque
            // returns to the normal z-band (kBorderlessMsg's HWND_TOPMOST /
            // HWND_NOTOPMOST). Skipped while fullscreen, where the WM already
            // owns the stacking.
            if (!g_window.is_fullscreen()) {
                g_window.set_keep_above(g_transparentBg);
            }
            // The input region itself is re-applied (or dropped) by
            // ClickthroughUpdate on the next frame, which is also the only
            // place that sets it — one owner, as on Windows.
        }
    }
}

// ============================================================================
// Rendering-mode switch (V / 0-8) — the Linux transliteration of
// displayxr-common's XrSessionUpdateModeSwitch (xr_session_common.cpp), which
// lives on the Win32-only XrSessionManager. Same 0.18 s SmoothStep ramp, same
// asymmetry: 3D->2D ramps the disparity to zero BEFORE switching, ->3D switches
// first and eases the disparity up. INV-2.4: the app REQUESTS, the runtime owns
// the active mode and reports it back via XrEventDataRenderingModeChangedDXR.
// ============================================================================
static void UpdateModeSwitch(AppXrSession& xr, InputState& state, float dt) {
    if (!g_modeSwitchConfigured) {
        g_modeSwitch.configure(0.18f, dxr::ModeSwitchEasing::SmoothStep);
        g_modeSwitchConfigured = true;
        g_msLastMode = xr.currentRenderingMode;
    }
    const float steady = state.viewParams.steadyIpdFactor;
    auto vcOf = [&](uint32_t m) -> uint32_t {
        return (m < xr.renderingModeCount && xr.renderingModeViewCounts[m] > 0)
                   ? xr.renderingModeViewCounts[m] : 1u;
    };

    // Funnel the V-cycle and the 0-8 absolute requests into one target.
    // Absolute wins if both fired the same frame.
    int32_t target = -1;
    if (state.cycleRenderingModeRequested) {
        state.cycleRenderingModeRequested = false;
        if (xr.renderingModeCount > 0)
            target = (int32_t)((xr.currentRenderingMode + 1) % xr.renderingModeCount);
    }
    if (state.absoluteRenderingModeRequested >= 0) {
        const int32_t a = state.absoluteRenderingModeRequested;
        state.absoluteRenderingModeRequested = -1;
        if ((uint32_t)a < xr.renderingModeCount) target = a;
    }

    if (target >= 0 && xr.session != XR_NULL_HANDLE &&
        xr.pfnRequestDisplayRenderingModeEXT != nullptr) {
        // currentIpd = the disparity actually on screen right now: the last
        // ramp output while one is in flight, else the steady value. The bare
        // modeSwitch.ipd() is 0 until the first ramp ever runs, which would
        // make the FIRST 3D->2D switch a one-time snap.
        const float currentIpd = g_modeSwitch.active() ? g_modeSwitch.ipd() : steady;
        g_modeSwitch.request((uint32_t)target, vcOf((uint32_t)target),
                             xr.currentRenderingMode, vcOf(xr.currentRenderingMode),
                             currentIpd, steady);
        LOG_INFO("Rendering mode -> %u (%s)", (unsigned)target,
                 ((uint32_t)target < xr.renderingModeCount && xr.renderingModeDisplay3D[target])
                     ? "3D" : "2D");
    }

    if (g_modeSwitch.active()) {
        float ipd = steady;
        bool fire = false;
        uint32_t mode = xr.currentRenderingMode;
        g_modeSwitch.update(dt, &ipd, &fire, &mode);
        state.viewParams.ipdFactor = ipd;  // the rig render value this frame
        if (fire && mode != xr.currentRenderingMode && xr.session != XR_NULL_HANDLE &&
            xr.pfnRequestDisplayRenderingModeEXT != nullptr) {
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, mode);
            g_msLastMode = mode;
        }
    } else {
        // Idle: render the tuned steady value (tracks +/- adjustments at once).
        state.viewParams.ipdFactor = steady;
    }
}

//! <Pictures>/DisplayXR/<stem>-<N>, auto-incrementing per (cols x rows). The
//! runtime appends "_atlas...png". Mirrors dxr_capture::MakeCaptureAtlasPrefix,
//! which displayxr-common only compiles on Windows/macOS.
static std::string MakeCaptureAtlasPrefix(const std::string& stem, uint32_t cols, uint32_t rows) {
    const char* home = getenv("HOME");
    std::string dir = (home != nullptr && home[0] != '\0') ? std::string(home) : std::string(".");
    dir += "/Pictures/DisplayXR";
    // Best-effort; the runtime reports if it cannot write there.
    mkdir((std::string(home ? home : ".") + "/Pictures").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    for (int n = 1; n < 10000; n++) {
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "%s/%s-%d_%ux%u", dir.c_str(), stem.c_str(), n, cols, rows);
        std::string candidate(buf);
        if (!FileExists(candidate + "_atlas.png")) return candidate;
    }
    return dir + "/" + stem;
}

// ============================================================================
// Placeholder clear — the "no model loaded yet" frame
// ============================================================================
// ModelRenderer::renderEye is the only thing that writes the atlas, and it is
// skipped when nothing is loaded, so without this the swapchain shows stale
// content. That was survivable while this leg was opaque-only; in transparent
// mode it would park a dark rectangle on the desktop for as long as no model
// is loaded, and the click-through region (derived from the atlas alpha) would
// be built from garbage. windows/main.cpp:2393 does exactly this, for exactly
// this reason.
static VkCommandPool g_clearPool = VK_NULL_HANDLE;

static void ClearAtlasImage(VkDevice dev, VkQueue queue, uint32_t queueFamily,
                            VkImage image, bool transparent) {
    if (dev == VK_NULL_HANDLE || image == VK_NULL_HANDLE) return;
    if (g_clearPool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(dev, &pci, nullptr, &g_clearPool) != VK_SUCCESS) return;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_clearPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    // Fully transparent in transparent mode (the desktop shows through while
    // the user is still picking a file); the viewer's usual slate otherwise.
    VkClearColorValue color = transparent ? VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}
                                          : VkClearColorValue{{0.1f, 0.1f, 0.12f, 1.0f}};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, g_clearPool, 1, &cmd);
}

static void SignalHandler(int) { g_running = false; }

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== DisplayXR 3D Model Viewer (Vulkan, Linux handle app — X11 or Wayland) ===");
    LOG_INFO("  [LMB-drag] Rotate  [Dbl-click] Focus  [Scroll] Zoom  "
             "(+Shift depth, +Ctrl parallax, +Alt perspective)");
    LOG_INFO("  [RMB-drag] Move the window (phase-snapped)  [WASDEQ] Move  [Space] Reset");
    LOG_INFO("  [V] Mode  [0-8] Mode N  [T] Eye tracking  [C] Rig  [-/=] Depth  [I] Capture");
    LOG_INFO("  [M] Auto-Orbit  [N] Clip  [K] Play/Pause  [L] Lighting  [G] Tone  [ [ / ] ] Exposure");
    LOG_INFO("  [Ctrl+O] Load  [F11] Fullscreen  [P then X/Y/Z] Pin recenter axis  [ESC] Quit");

    // Launch contract (shared with Windows/macOS): the first positional
    // argument is a model path, --vh=<metres> pins the virtual display height.
    // The remaining flags of the contract (--src URL download, --transparent,
    // --rect, --pose, --margin) are Windows-only today; they are parsed and
    // reported so a launcher gets a clear message instead of silence.
    std::string cliModelPath;
    // Window platform: --platform=x11|wayland|auto (default auto, a capability
    // probe). Taken out of the argument list first, so the launch-contract
    // parser never sees it (and never mistakes `--platform x11`'s value for a
    // model path).
    DxrWindowBackend requestedBackend = DxrWindowBackend::Auto;
    std::vector<std::string> args(argv + 1, argv + argc);
    {
        std::string err;
        if (!DxrLinuxWindow::take_platform_args(&args, &requestedBackend, &err)) {
            LOG_ERROR("%s", err.c_str());
            return 1;
        }
    }
    if (!args.empty()) {
        const dxr::LaunchArgs la = dxr::ParseLaunchArgs(args);
        for (const std::string& w : la.warnings) LOG_WARN("launch: %s", w.c_str());
        if (!la.ok()) {
            for (const std::string& e : la.errors) LOG_ERROR("launch: %s", e.c_str());
            return 1;
        }
        if (la.hasVh && la.vh > 0.0f) { g_vhOverride = la.vh; LOG_INFO("launch: --vh=%.4f m", la.vh); }
        if (!la.positionalPath.empty()) cliModelPath = la.positionalPath;
        if (!la.src.empty()) {
            if (la.srcKind == dxr::LaunchSrcKind::Url)
                LOG_WARN("launch: --src URL download is not implemented on Linux — ignored");
            else
                cliModelPath = la.src;
        }
        // --transparent: start in transparent mode (the undock contract's
        // flag). On Windows it also creates the window borderless + topmost
        // from frame 0; this window is borderless in every mode already, and
        // _NET_WM_STATE_ABOVE is applied once the window exists (below).
        if (la.transparent) { g_launchTransparent = true; LOG_INFO("launch: --transparent"); }
        if (la.hasRect) LOG_WARN("launch: --rect has no Linux path yet — use MODEL_WINDOW=\"WxH+X+Y\"");
        if (la.hasPose) LOG_WARN("launch: --pose has no Linux path yet — ignored");
    }

    // Dynamic-recenter pins: default hard-pin X+Y+Z (modelviewer parity).
    // P arms and X/Y/Z toggle an axis; DXR_RECENTER_PIN=XYZ|XY|Z|- presets it.
    g_recenter.init(/*x=*/true, /*y=*/true, /*z=*/true);
    { char lbl[24]; g_recenter.hudLabel(lbl, sizeof(lbl)); LOG_INFO("Recenter: %s", lbl); }

    { const char* mode = getenv("SIM_DISPLAY_OUTPUT");
      if (mode) {
          if (strcmp(mode, "sbs") == 0) g_windowW = 2560; // hint only; runtime owns the window
      } }

    AppXrSession xr = {};
    if (!InitializeOpenXR(xr, requestedBackend)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // Handle app: own a window on the 3D panel (display_info queried in
    // InitializeOpenXR gives the panel rect) — X11 or Wayland, whichever the
    // probe chose. Falls back to hosted-NULL when no window system answers
    // (also the CI-safe path — CI never runs this).
    // MODEL_TRANSPARENT — the analogue of the avatar's AVATAR_TRANSPARENT,
    // named after this leg's MODEL_WINDOW. Unlike the avatar (an overlay by
    // nature, transparent by default) the model viewer starts OPAQUE, as the
    // Windows leg does:
    //   unset / empty  transparent-capable, start opaque, Ctrl+T toggles
    //   1 (non-zero)   transparent-capable, start transparent (= --transparent)
    //   0              NOT capable: opaque root visual, transparentBackground-
    //                  Enabled = XR_FALSE — the pre-transparency behaviour.
    {
        const char* te = getenv("MODEL_TRANSPARENT");
        if (te != nullptr && te[0] == '0') {
            g_transparentCapable = false;
            if (g_launchTransparent) LOG_WARN("launch: --transparent overridden by MODEL_TRANSPARENT=0");
            LOG_INFO("MODEL_TRANSPARENT=0 — transparent background disabled for this session");
        } else if ((te != nullptr && te[0] != '\0') || g_launchTransparent) {
            g_transparentBg = true;
        }
    }
    if (!CreateAppWindow(xr)) {
        // hosted-NULL: the runtime owns the window; no ARGB visual, no shape.
        g_transparentCapable = false;
        g_transparentBg = false;
    }

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); g_window.destroy(); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); g_window.destroy(); return 1; }
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        CleanupOpenXR(xr); vkDestroyInstance(vkInstance, nullptr); g_window.destroy(); return 1; }
    std::vector<const char*> devExts; std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage)) {
        CleanupOpenXR(xr); vkDestroyInstance(vkInstance, nullptr); g_window.destroy(); return 1; }
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        CleanupOpenXR(xr); vkDestroyInstance(vkInstance, nullptr); g_window.destroy(); return 1; }
    VkDevice vkDevice = VK_NULL_HANDLE; VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        CleanupOpenXR(xr); vkDestroyInstance(vkInstance, nullptr); g_window.destroy(); return 1; }
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); g_window.destroy(); return 1; }
    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); g_window.destroy(); return 1; }

    std::vector<XrSwapchainImageVulkanKHR> swapchainImages(
        xr.swapchain.imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    { uint32_t count = xr.swapchain.imageCount;
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                              queueFamilyIndex, xr.swapchain.width, xr.swapchain.height))
        LOG_WARN("model renderer init failed");


    // Tile basis: the app window when we own one (window×scaleXY, #729-style);
    // else the full panel (hosted-NULL renders display-sized).
    uint32_t contentW = 0, contentH = 0;
    if (xr.hasAppWindow && g_window.current_size(&contentW, &contentH)) {
        g_windowW = contentW;   // the CONTENT (the bound window/surface, bar excluded)
        g_windowH = contentH;
    } else {
        if (xr.displayPixelWidth > 0) g_windowW = xr.displayPixelWidth;
        if (xr.displayPixelHeight > 0) g_windowH = xr.displayPixelHeight;
    }
    // Render whatever mode the DISPLAY reports active (respects the runtime
    // default, SIM_DISPLAY_OUTPUT / SIM_DISPLAY_FORCE_MODE, and any workspace
    // controller) rather than hard-coding a 2-view mode. Falls back to the first
    // 3D mode (index 1) when the runtime advertises no active mode.
    xr.currentRenderingMode = (xr.activeRenderingMode >= 0)
        ? (uint32_t)xr.activeRenderingMode
        : (xr.renderingModeCount > 1 ? 1 : 0);

    // Environment first: setEnvironment rebakes the IBL cubes, so doing it
    // before the model means frame one is already lit correctly.
    TryAutoLoadBundledEnvironment();
    // A model path on the command line replaces the bundled sample (Windows
    // parity: `model_viewer_handle_vk_win <path.glb>`).
    if (!cliModelPath.empty()) {
        if (!model_validate_file(cliModelPath)) {
            LOG_ERROR("Unsupported model file: %s", cliModelPath.c_str());
        } else if (g_modelRenderer.loadModel(cliModelPath.c_str())) {
            g_loadedFileName = model_basename(cliModelPath);
            LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(),
                     model_filesize_str(cliModelPath).c_str());
            ApplyAutoFitForLoadedScene();
        } else {
            LOG_ERROR("Failed to load %s", cliModelPath.c_str());
        }
    }
    if (!g_modelRenderer.hasModel()) TryAutoLoadBundledScene();

    LOG_INFO("=== Entering main loop ===");
    auto lastTime = std::chrono::high_resolution_clock::now();
    bool modeAsserted = false;

    while (g_running && !xr.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Assert the app's starting rendering mode ONCE, when the session comes
        // up. Deliberately not per frame: from here the runtime owns the active
        // mode (INV-2.4) and reports it through XrEventDataRenderingModeChangedDXR,
        // and a per-frame re-assert would fight the V/0-8 ModeSwitch ramp.
        if (xr.sessionRunning && !modeAsserted && xr.pfnRequestDisplayRenderingModeEXT &&
            xr.session != XR_NULL_HANDLE) {
            modeAsserted = true;
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, xr.currentRenderingMode);
        }

        // Live inputs the key handler needs (mode count gates the 0-8 keys).
        g_input.renderingModeCount = xr.renderingModeCount;
        g_input.nominalViewerZ = xr.nominalViewerZ;

        PollEvents(xr);
        PumpWindow(xr);    // mouse + keys; the helper runs the bar, the drag and F11
        PollFilePicker();  // async zenity result → loadModel + auto-fit

        if (!xr.sessionRunning) { struct timespec ts{0, 50 * 1000 * 1000}; nanosleep(&ts, nullptr); continue; }

        // ── consume the one-shot requests the pump raised ───────────────────
        if (g_input.animateToggleRequested) {
            g_input.animateToggleRequested = false;
            g_input.animateEnabled = !g_input.animateEnabled;
            LOG_INFO("Auto-Orbit: %s", g_input.animateEnabled ? "ON" : "OFF");
        }

        // V / 0-8 through the shared sequencer; it also writes this frame's
        // viewParams.ipdFactor, so it must run before the rig is built.
        UpdateModeSwitch(xr, g_input, dt);

        // T — MANAGED <-> MANUAL eye tracking.
        if (g_input.eyeTrackingModeToggleRequested) {
            g_input.eyeTrackingModeToggleRequested = false;
            if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
                const XrEyeTrackingModeDXR newMode =
                    (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                        ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
                const XrResult etr = xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
                if (XR_SUCCEEDED(etr)) xr.activeEyeTrackingMode = newMode;
                LOG_INFO("Eye tracking mode -> %s (%s)",
                         newMode == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
                         XR_SUCCEEDED(etr) ? "OK" : "unsupported");
            } else {
                LOG_WARN("Eye tracking mode toggle: xrRequestEyeTrackingModeDXR unavailable");
            }
        }

        // N / K — clip playback. Applied BEFORE the camera update so the
        // auto-orbit gate sees this frame's play/pause state (Windows parity).
        const bool cycleClip = g_input.cycleClipRequested;
        const bool playPause = g_input.playPauseRequested;
        g_input.cycleClipRequested = false;
        g_input.playPauseRequested = false;
        if (cycleClip) g_modelRenderer.cycleAnimation();
        if (playPause) g_modelRenderer.togglePaused();
        if (cycleClip || playPause) {
            std::string clip; int ci = 0, cn = 0; float ct = 0, cd = 0; bool playing = false;
            if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing)) {
                if (cycleClip) LOG_INFO("Clip: %s (%d/%d)", clip.c_str(), ci + 1, cn);
                else           LOG_INFO("%s: %s", playing ? "Playing" : "Paused", clip.c_str());
            } else {
                LOG_INFO("No animation in this model");
            }
        }

        // WASDQE / SPACE / C / the pose transition + auto-orbit.
        UpdateCameraMovement(g_input, dt, xr.displayHeightM);
        g_modelRenderer.updateAnimation(dt);

        XrFrameState frameState;
        if (!BeginFrame(xr, frameState)) continue;

        std::vector<XrCompositionLayerProjectionView> projectionViews;
        bool rendered = false;

        if (frameState.shouldRender) {
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            XrPosef cameraPose;
            quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &cameraPose.orientation);
            float rigPos[3]; ComputeRigPosition(rigPos);
            cameraPose.position = {rigPos[0], rigPos[1], rigPos[2]};
            const float rigVH = g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

            const bool useRig = xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;
            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
            if (useRig) {
                displayRig.pose = cameraPose;
                displayRig.virtualDisplayHeight = rigVH;
                displayRig.ipdFactor = g_input.viewParams.ipdFactor;
                displayRig.parallaxFactor = g_input.viewParams.parallaxFactor;
                displayRig.perspectiveFactor = g_input.viewParams.perspectiveFactor;
                locateInfo.next = &displayRig;
            }

            uint32_t runtimeViewCount = xr.maxViewCount > 0 ? xr.maxViewCount : 2;
            if (runtimeViewCount > 8) runtimeViewCount = 8;
            XrView views[8] = {};
            for (uint32_t v = 0; v < runtimeViewCount; v++) views[v].type = XR_TYPE_VIEW;
            XrViewState viewState = {XR_TYPE_VIEW_STATE};

            XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState,
                                        runtimeViewCount, &runtimeViewCount, views);
            if (XR_SUCCEEDED(lr) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                uint32_t m = xr.currentRenderingMode;
                uint32_t modeViewCount = (xr.renderingModeCount > 0 && m < xr.renderingModeCount)
                    ? xr.renderingModeViewCounts[m] : 2u;
                if (modeViewCount < 1) modeViewCount = 1;
                // INV-3.1 clamp: the mode's advertised count is one of three bounds.
                // `runtimeViewCount` now holds what xrLocateViews actually wrote;
                // xr.maxViewCount is the session view configuration's count, which is
                // what the atlas swapchain was worst-case-sized for (arraySize is 1 —
                // the views are tiles in image 0, so tile capacity is the slice bound).
                // Submitting past any of them fails xrEndFrame validation every frame —
                // exactly what a Quad mode did before the PRIMARY_MULTIVIEW_DXR opt-in.
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
                bool display3D = (xr.renderingModeCount > 0) ? xr.renderingModeDisplay3D[m] : true;
                bool monoMode = !display3D;
                uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.renderingModeTileColumns[m] > 0)
                    ? xr.renderingModeTileColumns[m] : (monoMode ? 1u : 2u);
                int eyeCount = monoMode ? 1 : (int)modeViewCount;

                float scaleX = (xr.renderingModeCount > 0 && m < xr.renderingModeCount) ? xr.renderingModeScaleX[m] : 1.0f;
                float scaleY = (xr.renderingModeCount > 0 && m < xr.renderingModeCount) ? xr.renderingModeScaleY[m] : 1.0f;
                uint32_t renderW = (uint32_t)((double)g_windowW * scaleX); if (renderW == 0) renderW = 1;
                uint32_t renderH = (uint32_t)((double)g_windowH * scaleY); if (renderH == 0) renderH = 1;

                // Consume the runtime's render-ready rig views (XR_DXR_view_rig).
                std::vector<Display3DView> eyeViews((size_t)eyeCount);
                bool hasKooima = useRig;
                if (useRig) {
                    std::vector<XrView> srcViews;
                    if (monoMode && modeViewCount >= 1) {
                        XrView cv = views[0];
                        XrVector3f c = {0,0,0}; XrFovf f = {0,0,0,0};
                        for (uint32_t v = 0; v < modeViewCount; v++) {
                            c.x += views[v].pose.position.x; c.y += views[v].pose.position.y; c.z += views[v].pose.position.z;
                            f.angleLeft += views[v].fov.angleLeft; f.angleRight += views[v].fov.angleRight;
                            f.angleUp += views[v].fov.angleUp; f.angleDown += views[v].fov.angleDown;
                        }
                        float inv = 1.0f / (float)modeViewCount;
                        cv.pose.position = {c.x*inv, c.y*inv, c.z*inv};
                        cv.fov = {f.angleLeft*inv, f.angleRight*inv, f.angleUp*inv, f.angleDown*inv};
                        srcViews.assign(1, cv);
                    } else {
                        for (int e = 0; e < eyeCount; e++)
                            srcViews.push_back(views[e < (int)runtimeViewCount ? e : 0]);
                    }
                    for (int eye = 0; eye < eyeCount; eye++) {
                        const XrView& sv = srcViews[eye];
                        float ez = RigLocalEyeZ(cameraPose, sv.pose.position);
                        float near_z = (ez - rigVH > 1e-4f) ? (ez - rigVH) : 1e-4f;
                        float far_z  = ez + 1000.0f * rigVH;
                        mat4_view_from_xr_pose(eyeViews[eye].view_matrix, sv.pose);
                        mat4_from_xr_fov(eyeViews[eye].projection_matrix, sv.fov, near_z, far_z);
                        convert_projection_gl_to_zero_to_one(eyeViews[eye].projection_matrix);
                        eyeViews[eye].fov = sv.fov;
                        eyeViews[eye].eye_world = sv.pose.position;
                        eyeViews[eye].orientation = sv.pose.orientation;
                        eyeViews[eye].near_z = near_z; eyeViews[eye].far_z = far_z;
                    }
                }

                // Double-click focus: ray from the CENTRE physical eyes through
                // the mouse position on the display surface, pick the nearest
                // surface, then smoothly move the virtual display onto it.
                // Mirrors windows/main.cpp:3110 and macos/main.mm:2604.
                if (g_input.teleportRequested && hasKooima && eyeCount > 0) {
                    g_input.teleportRequested = false;
                    const float vpW = (float)(g_windowW > 0 ? g_windowW : 1);
                    const float vpH = (float)(g_windowH > 0 ? g_windowH : 1);
                    const float ndcX = 2.0f * g_input.teleportMouseX / vpW - 1.0f;
                    // X11 pointer y is TOP-down (like Win32), so negate — the
                    // Cocoa leg does not, because its y=0 is at the bottom.
                    const float ndcY = -(2.0f * g_input.teleportMouseY / vpH - 1.0f);

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
                    const float invE = 1.0f / (float)eyeCount;
                    XrPosef cpose;
                    cpose.position = {cpos.x * invE, cpos.y * invE, cpos.z * invE};
                    cpose.orientation = cameraPose.orientation;
                    cfov = {cfov.angleLeft * invE, cfov.angleRight * invE,
                            cfov.angleUp * invE, cfov.angleDown * invE};
                    const float ez = RigLocalEyeZ(cameraPose, cpose.position);
                    const float pickNear = (ez - rigVH > 1.0e-4f) ? (ez - rigVH) : 1.0e-4f;
                    const float pickFar = ez + 1000.0f * rigVH;
                    float pickView[16], pickProj[16];
                    mat4_view_from_xr_pose(pickView, cpose);
                    // GL convention, NO [0,1] remap — the ray is a full line.
                    mat4_from_xr_fov(pickProj, cfov, pickNear, pickFar);

                    XrVector3f rayOriginV, rayDirV;
                    display3d_unproject_ndc_to_ray(ndcX, ndcY, pickView, pickProj,
                                                   &rayOriginV, &rayDirV);
                    float rayOrigin[3] = {rayOriginV.x, rayOriginV.y, rayOriginV.z};
                    float rayDir[3]    = {rayDirV.x, rayDirV.y, rayDirV.z};
                    float hitPos[3];
                    if (g_modelRenderer.pickSurface(rayOrigin, rayDir, hitPos)) {
                        XrPosef fromWorld;
                        fromWorld.orientation = cameraPose.orientation;
                        fromWorld.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};
                        XrPosef target;
                        target.position = {hitPos[0], hitPos[1], hitPos[2]};
                        target.orientation = cameraPose.orientation;
                        g_input.transitionFrom = fromWorld;
                        g_input.transitionTo = target;
                        g_input.transitionT = 0.0f;
                        g_input.transitioning = true;
                        LOG_INFO("Focus on surface (%.3f, %.3f, %.3f)", hitPos[0], hitPos[1], hitPos[2]);
                    }
                } else if (g_input.teleportRequested) {
                    g_input.teleportRequested = false; // consume without a rig
                }

                uint32_t imageIndex;
                if (AcquireSwapchainImage(xr, imageIndex)) {
                    rendered = true;
                    // ADR-041 (runtime #1612): the layer carries EVERY located
                    // view; only [0, eyeCount) are rendered (1 in a 2D mode) and
                    // the tail is aliased onto view 0 after the loop below. Under
                    // PRIMARY_MULTIVIEW_DXR xrEndFrame rejects a shorter layer —
                    // the panel then froze on the last 3D frame.
                    const uint32_t locatedViewCount =
                        (runtimeViewCount > (uint32_t)eyeCount) ? runtimeViewCount : (uint32_t)eyeCount;
                    projectionViews.assign((size_t)locatedViewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                    std::vector<std::array<float,16>> viewMat((size_t)eyeCount), projMat((size_t)eyeCount);
                    std::vector<std::pair<uint32_t,uint32_t>> tileOffsets((size_t)eyeCount);
                    for (int eye = 0; eye < eyeCount; eye++) {
                        int srcView = eye < (int)runtimeViewCount ? eye : 0;
                        if (hasKooima) {
                            memcpy(viewMat[eye].data(), eyeViews[eye].view_matrix, sizeof(float)*16);
                            memcpy(projMat[eye].data(), eyeViews[eye].projection_matrix, sizeof(float)*16);
                            views[srcView].pose.position = eyeViews[eye].eye_world;
                            views[srcView].pose.orientation = cameraPose.orientation;
                        } else {
                            mat4_view_from_xr_pose(viewMat[eye].data(), views[srcView].pose);
                            mat4_from_xr_fov(projMat[eye].data(), views[srcView].fov, 0.01f, 100.0f);
                        }
                        uint32_t tileX = (uint32_t)(eye % (int)tileColumns);
                        uint32_t tileY = (uint32_t)(eye / (int)tileColumns);
                        uint32_t vpX = tileX * renderW, vpY = tileY * renderH;
                        tileOffsets[eye] = {vpX, vpY};
                        auto& pv = projectionViews[eye];
                        pv.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                        pv.subImage.swapchain = xr.swapchain.swapchain;
                        pv.subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                        pv.subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                        pv.subImage.imageArrayIndex = 0;
                        pv.pose = views[srcView].pose;
                        pv.fov = hasKooima ? eyeViews[eye].fov : views[srcView].fov;
                    }
                    // Inactive tail -> view 0's tile, own located pose/fov.
                    DxrAliasInactiveViews(projectionViews.data(), views, locatedViewCount,
                                          (uint32_t)eyeCount);

                    const VkImage targetImage = swapchainImages[imageIndex].image;
                    const VkFormat swapFormat = (VkFormat)xr.swapchain.format;
                    if (!g_modelRenderer.hasModel()) {
                        ClearAtlasImage(vkDevice, graphicsQueue, queueFamilyIndex,
                                        targetImage, g_transparentBg);
                    }
                    if (g_modelRenderer.hasModel()) {
                        for (int eye = 0; eye < eyeCount; eye++)
                            g_modelRenderer.renderEye(targetImage, swapFormat,
                                xr.swapchain.width, xr.swapchain.height,
                                tileOffsets[eye].first, tileOffsets[eye].second,
                                renderW, renderH, viewMat[eye].data(), projMat[eye].data(),
                                g_transparentBg);
                    }
                    // 'I' — snapshot the multi-view atlas. Runtime-owned
                    // readback via xrCaptureAtlasDXR (no app-side staging
                    // texture); skipped for mono (1x1). The prefix carries no
                    // ".png" — the runtime appends "_atlas.png".
                    if (g_input.captureAtlasRequested) {
                        g_input.captureAtlasRequested = false;
                        const uint32_t cols = tileColumns > 0 ? tileColumns : 1u;
                        const uint32_t rows =
                            (xr.renderingModeCount > 0 && xr.renderingModeTileRows[m] > 0)
                                ? xr.renderingModeTileRows[m]
                                : (((uint32_t)eyeCount + cols - 1) / cols);
                        if (!g_modelRenderer.hasModel()) {
                            LOG_WARN("Capture skipped: no model loaded");
                        } else if (cols <= 1 && rows <= 1) {
                            LOG_WARN("Capture skipped: mono (1x1) layout");
                        } else if (xr.pfnCaptureAtlasEXT && xr.session != XR_NULL_HANDLE) {
                            const auto dot = g_loadedFileName.find_last_of('.');
                            std::string stem = (dot == std::string::npos)
                                ? g_loadedFileName : g_loadedFileName.substr(0, dot);
                            if (stem.empty()) stem = "scene";
                            const std::string prefix = MakeCaptureAtlasPrefix(stem, cols, rows);
                            XrAtlasCaptureInfoDXR info = {XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                            info.next = nullptr;
                            info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR;
                            strncpy(info.pathPrefix, prefix.c_str(), XR_ATLAS_CAPTURE_PATH_MAX_DXR - 1);
                            const XrResult cr = xr.pfnCaptureAtlasEXT(xr.session, &info, nullptr);
                            if (XR_SUCCEEDED(cr)) LOG_INFO("Atlas capture requested -> %s_atlas.png", prefix.c_str());
                            else LOG_WARN("xrCaptureAtlasDXR failed: 0x%x", (unsigned)cr);
                        } else {
                            LOG_WARN("Capture skipped: XR_DXR_atlas_capture not available");
                        }
                    }

                    // Click-through punch (Ctrl+T). Must run BEFORE the
                    // release — it reads the atlas the frame just rendered and
                    // hands it back in COLOR_ATTACHMENT_OPTIMAL, which is the
                    // layout the runtime expects at xrReleaseSwapchainImage.
                    // Same placement as windows/main.cpp:3439.
                    if (xr.hasAppWindow) {
                        // LIVE content size, from the window helper — the
                        // rect the runtime renders and the view tiles map
                        // onto (bar excluded; the helper offsets and unions
                        // the bar itself).
                        uint32_t winPxW = 0, winPxH = 0;
                        g_window.current_size(&winPxW, &winPxH);
                        const uint32_t lastEye = (uint32_t)(eyeCount - 1);
                        ClickthroughParams cp;
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
                        cp.lastTileX = tileOffsets[lastEye].first;
                        cp.lastTileY = tileOffsets[lastEye].second;
                        cp.twoViews = eyeCount > 1;
                        cp.window = &g_window;
                        cp.winW = winPxW;
                        cp.winH = winPxH;
                        cp.transparentBg = g_transparentBg;
                        // A WM-decorated X11 window (DXR_X11_WM_DECORATIONS=1)
                        // or a fullscreen one is never shaped: the frame needs
                        // the whole window for move/resize, and a fullscreen
                        // overlay has no desktop beside it to click through to.
                        static const bool s_wmDecorated = [] {
                            const char* e = getenv("DXR_X11_WM_DECORATIONS");
                            return e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
                        }();
                        cp.decorated = g_window.is_fullscreen() ||
                                       (s_wmDecorated && g_window.backend() == DxrWindowBackend::X11);
                        // The header bar is always kept clickable by the
                        // helper; it also counts as "reachable" below.
                        cp.chromeVisible = g_window.header_bar_visible();
                        ClickthroughUpdate(cp);
                    }

                    ReleaseSwapchainImage(xr);
                }
            }
        }

        if (rendered) {
            EndFrame(xr, frameState.predictedDisplayTime,
                     projectionViews.data(), (uint32_t)projectionViews.size());
        } else {
            XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
            ei.displayTime = frameState.predictedDisplayTime;
            ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            ei.layerCount = 0; ei.layers = nullptr;
            xrEndFrame(xr.session, &ei);
        }
    }

    g_modelRenderer.cleanup();
    if (vkDevice) vkDeviceWaitIdle(vkDevice);
    if (vkDevice) {
        ClickthroughDestroy(vkDevice);
        if (g_clearPool != VK_NULL_HANDLE) { vkDestroyCommandPool(vkDevice, g_clearPool, nullptr); g_clearPool = VK_NULL_HANDLE; }
    }
    CleanupOpenXR(xr);
    if (vkDevice) vkDestroyDevice(vkDevice, nullptr);
    if (vkInstance) vkDestroyInstance(vkInstance, nullptr);
    g_window.destroy();   // LAST: the runtime's VkSurfaceKHR borrowed this connection
    LOG_INFO("Clean exit");
    return 0;
}

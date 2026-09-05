// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR 3DGS OpenXR Ext VK - glTF 2.0 PBR model viewer with OpenXR (Vulkan)
 *
 * Renders glTF 2.0 models on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk with the cube/grid renderer replaced by
 * a 3DGS.cpp compute pipeline.  Features a "Load Scene" button as a
 * window-space layer overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>   // DragAcceptFiles / DragQueryFileA / DragFinish (WM_DROPFILES)
#include <shlwapi.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "model_renderer.h"
#include "display3d_view.h"
#include "projection_depth.h"

#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"
#include "vk_overlay_kit.h"   // dxr::CachedLayerUploader (#837 — no per-frame layer upload+wait)
#include "vk_clickthrough_region.h" // dxr::ClickThroughRegion (#833 — transparent-mode punch-through)
#include "win_window_drag.h"        // dxr::RmbWindowDrag (move the borderless overlay)
#include "recenter_control.h"  // dynamic-recenter per-axis pins (P then X/Y/Z; DXR_RECENTER_PIN)
#include "toast.h"             // dxr::ToastState — transient on-screen confirmation
#include "zone_default.h"      // dxr::FullWindowZone — zones-by-default (#63 / INV-5.6)
#include "auto_fit.h"          // dxr::AutoFitVHeight / FitTransition — shared width-aware framing
#include "auto_fit_canvas.h"   // dxr::AutoFitCanvas — the runtime-resolved viewport (shell tile)
// ── The undock launch contract (displayxr-common v2.9.0) ──────────────────
// One grammar + one security policy shared with the splat viewer: a web page
// or a CAD app spawns this viewer transparent, borderless and topmost at a
// given screen rect showing a given asset — with no post-creation style flip.
#include "launch_args.h"       // dxr::ParseLaunchArgsFromCommandLine / LaunchArgs
#include "url_fetch.h"         // dxr::FetchUrlToCache / DefaultCacheDir
#include "view_protocol.h"     // dxr::EnsureViewProtocolRegistered / single-instance

#include <atomic>
#include <cstdarg>   // ToastF varargs
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>   // snprintf (XR_DXR_mcp_tools JSON formatting, #47)
#include <cstdlib>  // strtod
#include <cstring>  // strcmp / strstr / strchr
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "model_viewer_handle_vk_win";

static const wchar_t* WINDOW_CLASS = L"DisplayXRModelViewerClass";
static const wchar_t* WINDOW_TITLE = L"DisplayXR 3D Model Viewer";

// ── Launch contract state ────────────────────────────────────────────────
// Parsed ONCE at the top of WinMain and read-only afterwards, so every
// consumer below (window creation, auto-fit, the --src fetch) sees the same
// validated values. A hostile protocol URL never reaches any of them: an
// entry in `errors` aborts the launch before the window exists.
static dxr::LaunchArgs g_launch;
// Held for the process lifetime when THIS process is the single instance —
// a second launch of the same scheme forwards its URL here by WM_COPYDATA
// instead of opening a second floating window over the desktop.
static HANDLE g_singleInstanceMutex = nullptr;
// Window title. `--title` is a SUFFIX, never a replacement: the base string
// is load-bearing for the repo's AppActivate-driven capture workflow
// (CLAUDE.md § Self-verifying a render).
static std::wstring g_windowTitle;
// `--vh` pins the virtual display height the asset was authored at. Auto-fit
// must then NOT re-derive it — the caller is asserting the asset's real-world
// scale, which is the whole point of undocking a CAD part at 1:1. 0 = no pin.
static std::atomic<float> g_vhOverride{0.0f};
// One --src download at a time (a WM_COPYDATA re-drive while a fetch is in
// flight would otherwise race two workers onto the same toast + load queue).
static std::atomic<bool> g_srcFetchInFlight{false};

// DXR_LAUNCH_QUIET=1 suppresses every launch-time dialog (refusal,
// sibling-not-installed, sibling-launch-failed). Those boxes exist for a
// PERSON: a protocol launch has no console, so without one a rejected link
// looks identical to a crash. Under an automated check the same box is a
// liability — it is modal, it sits on the panel until someone clicks OK, and
// the check that raised it has already exited. So agents and CI set this and
// read the log line + the exit code instead, which are the authority the box
// only ever restated. Never suppresses a log line and never changes an exit
// code. `DXR_LAUNCH_QUIET=0` explicitly disarms it.
static bool LaunchQuiet() {
    wchar_t buf[8] = {};
    const DWORD n = GetEnvironmentVariableW(L"DXR_LAUNCH_QUIET", buf, 8);
    return n > 0 && n < 8 && buf[0] != L'0';
}

// Every launch-path MessageBoxW goes through here so the quiet override
// cannot be forgotten at one call site.
static void LaunchDialog(const wchar_t* text, const wchar_t* caption, UINT icon) {
    if (LaunchQuiet()) {
        LOG_INFO("DXR_LAUNCH_QUIET=1 — dialog suppressed: %ls", text);
        return;
    }
    MessageBoxW(nullptr, text, caption, MB_OK | icon);
}

// HUD overlay fractions. Layer spans full window height so chrome buttons
// can sit at the window top while the info panel anchors to the bottom-left
// (matching the macOS demo's split). The vk_native compositor now uses an
// alpha-blended draw pass for window-space layers, so the empty middle of
// the texture stays invisible. Font sizing is anchored to the legacy
// 0.5-fraction so text doesn't grow with the taller texture.
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 1.0f;
static const float HUD_FONT_BASE_FRACTION = 0.50f;

// ── Top button bar ────────────────────────────────────────────────────────
// All chrome buttons live in ONE full-width window-space layer at the top:
// Open + Mode packed at the left, the Animation pill pinned to the right, and a
// transparent center so the model shows through. This replaces the old split
// (Open/Mode baked into the HUD layer + Animation on its own separate layer) —
// per runtime issue #389: group co-planar controls into a single layer and keep
// the HUD info panel as its own (toggleable) layer. Positions below are absolute
// window-fractions, used both for hit-testing and for placing the pills inside
// the bar texture (the bar layer spans the full window width, so window-x maps
// straight onto bar-texture-x).
static const float OPEN_BTN_X_FRACTION = 0.010f;
static const float OPEN_BTN_WIDTH_FRACTION  = 0.060f;

static const float MODE_BTN_X_FRACTION = 0.075f;
static const float MODE_BTN_WIDTH_FRACTION  = 0.140f;

// Animation pill — right-aligned within the bar. Only drawn/clickable when the
// model has clips. Label = current clip name, or "Paused"; click = next clip
// (same as 'N').
static const float ANIM_BTN_WIDTH_FRACTION  = 0.140f;
static const float ANIM_BTN_MARGIN_FRACTION = 0.010f;
static inline float AnimBtnXFraction() {
    return 1.0f - ANIM_BTN_WIDTH_FRACTION - ANIM_BTN_MARGIN_FRACTION;
}

// Bar swapchain texture (wide + thin) and its window-space layer geometry. The
// layer spans the full window width; its height preserves the texture aspect so
// the pills aren't distorted as the tile is resized. The pills fill ~70% of the
// bar height, vertically centered.
static const uint32_t BTN_BAR_TEX_W = 1920;
static const uint32_t BTN_BAR_TEX_H = 56;
static const uint32_t BTN_BAR_FONT_BASE = BTN_BAR_TEX_H * 14;
static const float    BTN_BAR_Y_FRACTION = 0.008f;
static inline float BtnBarHeightFraction(uint32_t windowW, uint32_t windowH) {
    if (windowW == 0 || windowH == 0) return 0.05f;
    const float windowAR = (float)windowW / (float)windowH;
    const float texAR = (float)BTN_BAR_TEX_W / (float)BTN_BAR_TEX_H;
    return windowAR / texAR;  // layer width fraction = 1.0
}

// ── Toast layer (transient confirmation chip) ────────────────────────────────
// Its own window-space layer, bottom-centre, submitted ONLY while a toast is
// live (dxr::ToastState decides) — so it is a true toggle, not a transparent
// layer, and it is independent of the Tab-toggled HUD info panel: a confirmation
// must be visible at the moment of the action whether or not the HUD is up.
// Geometry comes from dxr::ComputeToastLayerRect (displayxr-common): the chip
// is sized off the window's SHORTER side so it looks identical in this
// landscape window and in the avatar demo's portrait one.
static const uint32_t TOAST_TEX_W = 768;
static const uint32_t TOAST_TEX_H = 96;
static const uint32_t TOAST_FONT_BASE = TOAST_TEX_H * 11;   // ~45px glyphs in a 96px pill
static const float    TOAST_SIZE_FRACTION = 0.60f;          // of the shorter window side
static const float    TOAST_Y_FRACTION = 0.84f;             // near the bottom edge


// ── XR_DXR_view_rig consume-path math (#396 W7) ──────────────────────────────
// View/projection builders for the runtime's render-ready XrView{pose, fov}:
// GL convention, column-major float[16], matching the macOS main.mm helpers
// (per-platform duplication is the accepted pattern for these ~20 lines).

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

// sim_display output mode switching (legacy — replaced by unified rendering mode)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

// Global state
static InputState g_inputState;
// Standalone demo: bare TAB toggles the HUD (displayxr::common defaults to
// SHIFT+TAB so runtime test apps don't shadow the workspace shell's
// focus-cycle binding).
static const bool g_inputInit = [] {
    g_inputState.hudToggleRequiresShift = false;
    return true;
}();
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
// The app window, published as soon as CreateAppWindow() returns. Auto-fit
// needs the *live* client rect (its aspect is what the width rule keys off),
// and the WM_SIZE-maintained statics above still hold the requested creation
// size until the first resize lands. Null before window creation and after
// teardown — callers must fall back to the statics.
static HWND g_appHwnd = nullptr;

// 3DGS state
static ModelRenderer g_modelRenderer;
// Cross-thread scene-load queue: the file dialog runs on the main (message-pump)
// thread, but the actual ModelRenderer::loadScene() submits Vulkan work on the
// graphics queue and so MUST run on the same thread that drives per-frame
// rendering — otherwise concurrent vkQueueSubmit/vkQueueWaitIdle from two
// threads on a single VkQueue is undefined behaviour and crashes some drivers
// (NVIDIA in particular). Main thread posts the picked path here; the render
// thread picks it up between frames.
static std::atomic<bool> g_loadRequested{false};
static std::string g_pendingLoadPath;
// True when the queued path came from a --src download rather than from the
// user. A remote asset must never be able to raise a MODAL dialog on the
// render thread: under the undock contract the window is topmost and shaped,
// the box would stop the scene and sit on the panel until someone clicked it,
// and that someone may be a web page away. Guarded by g_pendingLoadPathMutex
// with the path it belongs to.
static bool g_pendingLoadFromUrl = false;
static std::mutex g_pendingLoadPathMutex;
// 'I' key: capture the multi-view atlas region (cols × rows × renderW × renderH)
// of the swapchain to a PNG in %USERPROFILE%\Pictures\DisplayXR\. Skipped for
// 1×1 (mono) layouts. Helper lives in test_apps/common/atlas_capture*.
static std::atomic<bool> g_captureAtlasRequested{false};
// Ctrl+T: opaque ⇄ transparent background. Always-on session-level
// transparency is wired at xrCreateSession; this flag only flips the
// renderer's output alpha (1 → 1-T) so background-uncovered pixels
// punch through to the desktop.
static std::atomic<bool> g_transparentBg{false};
// #833 punch-through: under opaque present (DXR_PRESENT_OPAQUE) DWM completes
// no blends, so transparent mode must CARVE the uncovered pixels out of the
// window (dxr::ClickThroughRegion) instead of relying on alpha — and a shaped
// WS_EX_NOREDIRECTIONBITMAP window can never paint an OS frame, so Ctrl+T
// also goes borderless (WS_POPUP) while transparent. kBorderlessMsg runs the
// style swap on the window-owning thread; the render thread posts it.
static const UINT kBorderlessMsg = WM_APP + 0x33;
static std::atomic<bool> g_borderless{false};
static dxr::ClickThroughRegion g_punch; // render-thread owned
static dxr::RmbWindowDrag g_windowDrag; // window-thread owned (WndProc)
// Toast band published by the render thread's toast block so the region
// keeps the chip visible (a shaped window clips ANY pixel outside the
// region — including post-weave Local2D layers).
static std::mutex g_toastBandMtx;
static RECT g_toastBand = {0, 0, 0, 0};
static bool g_toastBandLive = false;
static std::string g_loadedFileName;
static std::mutex g_sceneMutex;
// True when the loaded model has animation clips — gates the animation button
// layer + its click hit-test (read on the UI thread, set on load).
static std::atomic<bool> g_hasAnimations{false};

// Animation-button window-space layer resources: created in main() (when the
// HUD swapchain — i.e. window-space layers — is available), used by the render
// thread. The swapchain is app-owned state (displayxr::common's
// XrSessionManager carries no app-named fields, #396 W4) — created via the
// lib's CreateWindowSpaceSwapchain generic, destroyed before CleanupOpenXR.
// Zones-by-default (#63 / INV-5.6): one full-window XrDisplayZoneDXR chained
// on locate + projection every frame. Buys the zones-frame composition rules —
// most visibly, the toast below can be a Local2D layer composited POST-weave,
// so it stops being clipped by the transparency silhouette in Ctrl+T mode.
// Unavailable (old runtime / extension pair absent / headless): both chain
// calls return NULL and every path below degrades to the legacy frame.
static dxr::FullWindowZone g_fwZone;

// Toast layer resources — same shape as the button-bar set below, but the layer
// is only submitted on the frames dxr::ToastState says a message is live.
static dxr::ToastState g_toast;

// Post a toast from a printf-style ASCII string. Every user-visible confirmation
// in this app goes through here so the wording and lifetime stay consistent.
// ASCII in / wide out: the chip rasterizer takes std::wstring, but every message
// we produce is ASCII, so %hs widening is enough and avoids a locale dependency.
static void ToastF(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wchar_t w[160];
    swprintf(w, 160, L"%hs", buf);
    g_toast.Show(w);
}
static SwapchainInfo  g_toastSwapchain;
static bool           g_hasToastSwapchain = false;
static HudRenderer    g_toastHud = {};
static bool           g_toastReady = false;
static VkBuffer       g_toastStaging = VK_NULL_HANDLE;
static VkDeviceMemory g_toastStagingMem = VK_NULL_HANDLE;
static void*          g_toastStagingMapped = nullptr;
static VkCommandPool  g_toastCmdPool = VK_NULL_HANDLE;
static std::vector<XrSwapchainImageVulkanKHR> g_toastSwapImages;

static SwapchainInfo  g_animBtnSwapchain;                // app-owned window-space swapchain
static bool           g_hasAnimBtnSwapchain = false;
static HudRenderer    g_animBtnHud = {};                 // own D3D11 text renderer (256×80)
static bool           g_animBtnReady = false;            // all resources created
static VkBuffer       g_animBtnStaging = VK_NULL_HANDLE;
static VkDeviceMemory g_animBtnStagingMem = VK_NULL_HANDLE;
static void*          g_animBtnStagingMapped = nullptr;
static VkCommandPool  g_animBtnCmdPool = VK_NULL_HANDLE;
static std::vector<XrSwapchainImageVulkanKHR> g_animBtnSwapImages;

// Fallback vHeight when no scene is loaded or auto-fit hits a degenerate
// extent. Matches macOS demo's kDefaultVirtualDisplayHeightM (1.5m).
static constexpr float kFallbackVirtualDisplayHeightM = 1.5f;
// Load-time framing is the shared width-aware rule from displayxr-common
// (dxr::AutoFitVHeight, default dxr::kAutoFitDefaultFill = 80%): the model
// spans at most 80% of the viewport in BOTH axes, so a wide asset is bound by
// width instead of overflowing the sides. There is no separate vertical
// comfort multiplier — the fill fraction IS the headroom.

// Cached auto-fit pose for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kFallbackVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static std::atomic<bool> g_fitValid{false};

// Refit state (displayxr-common common/auto_fit_canvas.h). vHeight is a
// function of (content, viewport): the CONTENT half is cached here so a
// viewport change re-derives the base without re-measuring the model.
static std::atomic<float> g_fitExtentW{0.0f};
static std::atomic<float> g_fitExtentH{0.0f};
static std::atomic<float> g_fitAspect{0.0f};   //!< viewport the current base was derived for
static dxr::AutoFitCanvas g_autoFitCanvas;     //!< runtime-resolved canvas, published post-locate
static dxr::FitTransition g_fitTransition;     //!< render-thread only

// Dynamic-recenter pins. Default X Y Z = the modelviewer's historical hard-pin
// of all three axes onto the animated centroid; DXR_RECENTER_PIN overrides.
static dxr::RecenterControl g_recenter;

// XR_DXR_mcp_tools (#47): late (un)registration of the animation tools —
// list/play/stop_animation exist only while a model with clips is loaded.
// Called from ApplyAutoFitForLoadedScene_locked (the single choke point every
// model load funnels through). Defined with the rest of the MCP block below.
static void UpdateMcpAnimationTools();

// Viewport the auto-fit rule frames against. The demo's display zone is the
// full window (dxr::FullWindowZone), so the viewport is that window's canvas;
// only its aspect matters to dxr::AutoFitVHeight, so metres and pixels mix.
//
// Our own client rect is NOT that canvas under the shell. A shell-launched app
// is composed into a 3D window tile the shell owns; this window is hidden
// (SW_HIDE), is never what the user sees, and the runtime only resizes it to
// the tile later — deferred and async, once the client is placed. The bundled
// model auto-loads during init, long before that, so the rect read here was
// always the creation size. This comment used to acknowledge the ordering ("a
// load can run before the first WM_SIZE") and answer it with the WM_SIZE
// statics, which are the same wrong window.
//
// So: prefer the canvas the runtime RESOLVED (XR_DXR_view_rig's raw channel —
// the shell tile under a workspace, this window's client rect standalone), and
// keep the client rect only as the bootstrap for the fit that runs before the
// first locate. RefitForViewport re-derives once the real canvas arrives.
// (0,0) is still a legal result: AutoFitVHeight degrades to height-only.
//
// Returns true when the dims came from the runtime canvas (metres) rather than
// the client-rect bootstrap (pixels) — the two are not comparable numbers, only
// their aspects are, so anything logging them must say which it got.
static bool GetAutoFitViewportPx(float& outW, float& outH) {
    float fallbackW = (float)g_windowWidth;
    float fallbackH = (float)g_windowHeight;
    RECT client = {};
    if (g_appHwnd && GetClientRect(g_appHwnd, &client) &&
        client.right > client.left && client.bottom > client.top) {
        fallbackW = (float)(client.right - client.left);
        fallbackH = (float)(client.bottom - client.top);
    }
    return g_autoFitCanvas.Viewport(fallbackW, fallbackH, outW, outH);
}

// Compute robust scene bounds (5th–95th percentile per axis) and stage
// new display-rig pose + vHeight on g_inputState. Display orientation is
// kept identity (forward = world −Z): splats have no canonical front, and
// any heuristic (PCA, etc.) can pick the wrong side; the user can rotate
// with mouse drag from a predictable starting pose.
// Caller must hold g_sceneMutex (we read pickData_ from the renderer).
static void ApplyAutoFitForLoadedScene_locked() {
    // Gate the right-justified animation button on whether this model has clips.
    g_hasAnimations.store(g_modelRenderer.hasAnimations());
    float center[3], extent[3];
    // Full model AABB: center for the rig position, extent[0]/extent[1] (FULL
    // sizes, not half-extents) for the width-aware fit.
    bool ok = g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent);
    if (ok) {
        g_fitCenter[0] = center[0];
        g_fitCenter[1] = center[1];
        g_fitCenter[2] = center[2];
        float viewportW = 0.0f, viewportH = 0.0f;
        const bool fromCanvas = GetAutoFitViewportPx(viewportW, viewportH);
        float vh = dxr::AutoFitVHeight(extent[0], extent[1], viewportW, viewportH);
        // Degenerate scene (all splats in a thin slice) — fall back to a
        // sensible vHeight rather than failing the fit. Mirrors macOS:1399.
        if (!(vh > 1e-3f)) vh = kFallbackVirtualDisplayHeightM;
        // --vh wins over the fit. The caller is asserting the scale the asset
        // was authored at (a CAD part undocked at 1:1); auto-fit's job is to
        // guess one when nobody said, not to overrule someone who did.
        const float vhPin = g_vhOverride.load(std::memory_order_relaxed);
        if (vhPin > 0.0f) {
            LOG_INFO("Auto-fit: vHeight pinned to %.3f m by --vh (fit would have used %.3f)",
                     vhPin, vh);
            vh = vhPin;
        }
        g_fitVHeight = vh;
        // Cache the CONTENT half of the fit (extents are model properties) and
        // the viewport this base was derived for, so RefitForViewport can
        // re-derive on an aspect change without re-measuring the model. A load
        // lands the base immediately — the framing IS the load's result.
        g_fitExtentW.store(extent[0], std::memory_order_relaxed);
        g_fitExtentH.store(extent[1], std::memory_order_relaxed);
        g_fitAspect.store((viewportH > 0.0f) ? (viewportW / viewportH) : 0.0f,
                          std::memory_order_relaxed);
        g_fitTransition.start(vh, vh, 0.0f);
        // Anchor at yaw=0 and trust the loader's RUB convention (PLY loader
        // converts RDF+X-mirror → RUB at load time; SPZ is RUB-native and
        // SuperSplat-authored scenes already face −Z at yaw=0). Matches
        // macOS:1407 — the user can drag with LMB if a particular asset's
        // authored orientation is off.
        g_fitYaw = 0.0f;
        // Which axis bound the fit: width wins when the model is wider than
        // the viewport aspect can hold at the height-only vHeight. With no
        // usable viewport the rule degrades to height-only.
        const bool haveViewport = (viewportW > 0.0f && viewportH > 0.0f);
        const float aspect = haveViewport ? (viewportW / viewportH) : 0.0f;
        const char* boundBy = !haveViewport ? "height (no viewport)"
                            : (extent[0] / aspect > extent[1]) ? "width" : "height";
        LOG_INFO("Auto-fit: center=(%.3f, %.3f, %.3f) extent W=%.3f H=%.3f D=%.3f "
                 "viewport=%.3fx%.3f (%s) (aspect %.3f) bound-by=%s fill=%.0f%% vHeight=%.3f yaw=%.0fdeg",
                 center[0], center[1], center[2],
                 extent[0], extent[1], extent[2],
                 viewportW, viewportH,
                 fromCanvas ? "runtime canvas, m" : "client rect, px", aspect, boundBy,
                 dxr::kAutoFitDefaultFill * 100.0f, vh, g_fitYaw * 57.2957795f);
    }
    g_fitValid.store(ok);

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputState.cameraPosX = ok ? g_fitCenter[0] : 0.0f;
    g_inputState.cameraPosY = ok ? g_fitCenter[1] : 0.0f;
    g_inputState.cameraPosZ = ok ? g_fitCenter[2] : 0.0f;
    g_inputState.yaw = ok ? g_fitYaw : 0.0f;
    g_inputState.pitch = 0.0f;
    {
        const float vhPin = g_vhOverride.load(std::memory_order_relaxed);
        g_inputState.viewParams.virtualDisplayHeight =
            (vhPin > 0.0f) ? vhPin
                           : (ok ? g_fitVHeight : kFallbackVirtualDisplayHeightM);
        // Keep the Space-reset target on the pin too, so a reset does not
        // quietly drop back to the fallback on a degenerate-bounds model.
        if (vhPin > 0.0f) g_fitVHeight = vhPin;
    }
    g_inputState.viewParams.scaleFactor = 1.0f;

    // Per-format orientation correction is now done at load time (PLY loader
    // converts RDF+X-mirror → canonical RUB; SPZ loader uses RUB natively).
    // Renderer's ModelRenderer::updateUniforms negates the Y row of proj_mat to
    // match the +Y-up convention. No runtime view-stage flips needed.

    // Route the first post-load frame through the same reset path Space uses,
    // so app-start view params (perspectiveFactor, scaleFactor, etc.) match
    // the Space-reset state.
    g_inputState.resetViewRequested = true;

    // Treat scene load as a fresh user interaction so the auto-orbit idle
    // timer restarts. Without this, an asset loaded after the 10s idle
    // threshold starts rotating immediately on first display.
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
        g_inputState.animationActive = false;
    }

    // Sync the agent-facing animation tools to the newly-loaded model: appear
    // when it has clips, disappear when it doesn't (#47). No-op until the base
    // tools are registered (MCP capability off / older runtime).
    UpdateMcpAnimationTools();
}

// Re-derive the base vHeight when the viewport's ASPECT changes, and animate
// the move. Render thread only.
//
// This is what makes the load-time fit correct under the shell. The model
// auto-loads during init, before the first xrLocateViews, so the only viewport
// available then is this window's own client rect — which under a workspace is
// the hidden creation-size window, not the tile the user sees. The first
// located frame publishes the real canvas, the aspect gate sees it differ from
// the bootstrap one, and the fit lands on the tile. The same path handles a
// live 3D-window resize, which mis-framed identically before.
//
// Only the BASE moves: the render path computes rigVH = virtualDisplayHeight /
// scaleFactor, so the user's zoom stays relative (2x of the old fit becomes 2x
// of the new one) and orbit/pivot are untouched. A viewport change is not a
// request to undo deliberate user state — recentring belongs on Space.
static void RefitForViewport(float dtSeconds) {
    if (!g_fitValid.load(std::memory_order_relaxed)) {
        return;
    }
    // --vh pinned the base: a viewport change must not re-derive it. (The
    // transition below still runs so an in-flight animation lands.)
    if (g_vhOverride.load(std::memory_order_relaxed) > 0.0f) {
        float pinned = 0.0f;
        if (g_fitTransition.update(dtSeconds, &pinned)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.viewParams.virtualDisplayHeight = pinned;
        }
        return;
    }
    const float extW = g_fitExtentW.load(std::memory_order_relaxed);
    const float extH = g_fitExtentH.load(std::memory_order_relaxed);
    if (!(extH > 0.0f)) {
        return;
    }

    float vpW = 0.0f, vpH = 0.0f;
    const bool fromCanvas = GetAutoFitViewportPx(vpW, vpH);
    const float aspect = (vpH > 0.0f) ? (vpW / vpH) : 0.0f;
    if (dxr::AutoFitAspectChanged(g_fitAspect.load(std::memory_order_relaxed), aspect)) {
        const float vh = dxr::AutoFitVHeight(extW, extH, vpW, vpH);
        if (vh > 1e-3f) {
            // Retarget rather than restart: a resize that settles in two steps
            // must not snap back to where it started.
            g_fitTransition.start(g_fitTransition.value(), vh);
            const float prev = g_fitVHeight;
            g_fitAspect.store(aspect, std::memory_order_relaxed);
            g_fitVHeight = vh;  // Space-reset target follows the live viewport
            LOG_INFO("Auto-fit refit: viewport=%.3fx%.3f (%s) aspect=%.3f bound-by=%s "
                     "base %.3f -> %.3f (zoom preserved)",
                     vpW, vpH, fromCanvas ? "runtime canvas, m" : "client rect, px", aspect,
                     (aspect > 0.0f && extW / aspect > extH) ? "width" : "height",
                     prev, vh);
        }
    }

    float animated = 0.0f;
    if (g_fitTransition.update(dtSeconds, &animated)) {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_inputState.viewParams.virtualDisplayHeight = animated;
    }
}

// ============================================================================
// XR_DXR_mcp_tools dispatch (#47) — ported from macos/main.mm
// ============================================================================
// The model viewer exposes its controls as agent tools on the per-process MCP
// server the runtime hosts. appId "modelviewer" matches the `id` field in
// windows/displayxr/model_viewer_handle_vk_win.displayxr.json (INV-10.1). Tool
// names, descriptions, and JSON input schemas are copied verbatim from the
// macOS build so agents behave identically across platforms. The whole path is
// inert when McpToolsResolved() is false (older runtime / MCP capability off).
//
// All handlers below run on the render thread (dispatched from the shared
// PollEvents), the same thread that owns g_modelRenderer and drives
// per-frame rendering — so model loads are safe here, exactly like the queued-
// load drain. g_inputState is shared with the window-message thread and so is
// guarded by g_inputMutex; g_loadedFileName / scene state by g_sceneMutex.

// appId declared + base tools registered. Set by RegisterModelViewerMcpTools.
static bool g_mcpToolsReady = false;
// list/play/stop_animation currently live (late-registered on model load).
static bool g_mcpAnimToolsRegistered = false;

// Minimal JSON helpers — hand-rolled on purpose (matching macos/main.mm): tool
// args are tiny one-level objects, so a JSON dependency isn't warranted.
static std::string McpJsonEscape(const std::string& s) {
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
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract "key":"value" (string) with backslash-escape handling incl. a basic
// \uXXXX → UTF-8 decode (no surrogate pairs — file paths don't need them).
// False when the key is absent or its value is not a string.
static bool McpJsonGetString(const char* json, const char* key, std::string& out) {
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

// Extract "key": <number>. False when absent or not numeric (strtod refuses a
// leading quote, so string values correctly fail).
static bool McpJsonGetNumber(const char* json, const char* key, double& out) {
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

// Extract "key": true|false. False when the key is absent or its value is not
// a JSON boolean — so an omitted argument keeps whatever default the caller
// pre-loaded into `out` (set_transparent_background relies on that to toggle).
static bool McpJsonGetBool(const char* json, const char* key, bool& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (strncmp(c, "true", 4) == 0)  { out = true;  return true; }
    if (strncmp(c, "false", 5) == 0) { out = false; return true; }
    return false;
}

// Late (un)registration of the animation tools. They exist only while a model
// with clips is loaded, so each transition makes the runtime broadcast the MCP
// tool-list change to connected agents. Called from
// ApplyAutoFitForLoadedScene_locked (every load funnels through it) — always on
// the render thread once setup is complete.
static void UpdateMcpAnimationTools() {
    if (!g_mcpToolsReady || !g_pfnRegisterMcpTool || !g_pfnUnregisterMcpTool)
        return;
    const bool want = g_modelRenderer.hasAnimations();
    if (want == g_mcpAnimToolsRegistered) return;

    if (want) {
        XrMCPToolInfoDXR listTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        listTool.name = "list_animations";
        listTool.description =
            "List the loaded model's animation clips: index, name and duration in "
            "seconds, plus the active clip index and whether playback is running. "
            "Only available while a model with animation clips is loaded.";
        listTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r1 = g_pfnRegisterMcpTool(g_xr->session, &listTool);

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
        XrResult r2 = g_pfnRegisterMcpTool(g_xr->session, &playTool);

        XrMCPToolInfoDXR stopTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        stopTool.name = "stop_animation";
        stopTool.description =
            "Pause animation playback, freezing the model at its current pose. "
            "Resume with play_animation.";
        stopTool.inputSchemaJson = "{\"type\":\"object\"}";
        XrResult r3 = g_pfnRegisterMcpTool(g_xr->session, &stopTool);

        g_mcpAnimToolsRegistered = XR_SUCCEEDED(r1) || XR_SUCCEEDED(r2) || XR_SUCCEEDED(r3);
        LOG_INFO("XR_DXR_mcp_tools: animation tools registered (%d clip(s)) [%d %d %d]",
                 g_modelRenderer.animationCount(), r1, r2, r3);
    } else {
        g_pfnUnregisterMcpTool(g_xr->session, "list_animations");
        g_pfnUnregisterMcpTool(g_xr->session, "play_animation");
        g_pfnUnregisterMcpTool(g_xr->session, "stop_animation");
        g_mcpAnimToolsRegistered = false;
        LOG_INFO("XR_DXR_mcp_tools: animation tools unregistered (model has no clips)");
    }
}

// Dispatch one agent tool call to the Windows app state. Runs on the render
// thread (from the shared PollEvents, common v2.1.0). Answers EVERY call —
// success=false + {"error":…} on bad args — because an unanswered call only
// fails to the agent after the runtime's ~5 s timeout. Installed by assigning
// it to XrSessionManager::mcpToolHandler; PollEvents fetches the args and
// submits this return value, so the handler is a pure (tool,args)→resultJson
// map. Matches XrSessionManager::McpToolHandler.
static std::string McpDispatchToolCall(const std::string& toolName,
                                       const std::string& argsJson, bool& ok) {
    const char* a = argsJson.c_str();  // JSON helpers below take a C string
    ok = true;
    std::string result;
    char buf[1024];

    if (toolName == "load_model") {
        std::string path;
        if (!McpJsonGetString(a, "path", path) || path.empty()) {
            ok = false;
            result = "{\"error\":\"missing required string argument 'path'\"}";
        } else if (!model_validate_file(path)) {
            ok = false;
            result = "{\"error\":\"not a readable supported model file: " +
                     McpJsonEscape(path) + "\"}";
        } else {
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            if (!g_modelRenderer.loadModel(path.c_str())) {
                ok = false;
                result = "{\"error\":\"failed to load (corrupt or unsupported): " +
                         McpJsonEscape(path) + "\"}";
            } else {
                g_loadedFileName = model_basename(path);
                LOG_INFO("Model loaded via MCP: %s (%s)", g_loadedFileName.c_str(),
                         model_filesize_str(path).c_str());
                // Re-frames the camera and registers/unregisters the agent
                // animation tools for the new model.
                ApplyAutoFitForLoadedScene_locked();
                snprintf(buf, sizeof(buf),
                         "{\"file\":\"%s\",\"primitives\":%u,\"animation_count\":%d}",
                         McpJsonEscape(g_loadedFileName).c_str(),
                         g_modelRenderer.primitiveCount(),
                         g_modelRenderer.animationCount());
                result = buf;
            }
        }
    } else if (toolName == "get_status") {
        std::string clip; int ci = -1, cn = 0; float ct = 0, cd = 0; bool playing = false;
        const bool hasClip = g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing);
        // Snapshot the shared camera/scene state under their locks.
        float yaw, pitch, px, py, pz, zoom;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            yaw = g_inputState.yaw;
            pitch = g_inputState.pitch;
            px = g_inputState.cameraPosX;
            py = g_inputState.cameraPosY;
            pz = g_inputState.cameraPosZ;
            zoom = g_inputState.viewParams.scaleFactor;
        }
        std::string file;
        {
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            file = g_loadedFileName;
        }
        const float azDeg = fmodf(yaw * 57.29578f, 360.0f);
        const float elDeg = pitch * 57.29578f;
        std::string clipJson = hasClip ? "\"" + McpJsonEscape(clip) + "\"" : "null";
        snprintf(buf, sizeof(buf),
                 "{\"file\":\"%s\",\"loaded\":%s,\"primitives\":%u,"
                 "\"animation_count\":%d,\"active_animation\":%d,"
                 "\"active_animation_name\":%s,\"animation_playing\":%s,"
                 "\"camera\":{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,"
                 "\"position\":[%.3f,%.3f,%.3f],\"zoom\":%.2f},"
                 "\"transparent_background\":%s,"
                 "\"rendering_mode\":%u,\"session_running\":%s}",
                 McpJsonEscape(file).c_str(),
                 g_modelRenderer.hasModel() ? "true" : "false",
                 g_modelRenderer.primitiveCount(),
                 g_modelRenderer.animationCount(),
                 hasClip ? ci : -1, clipJson.c_str(),
                 (hasClip && playing) ? "true" : "false",
                 azDeg, elDeg, px, py, pz, zoom,
                 g_transparentBg.load() ? "true" : "false",
                 g_xr ? g_xr->currentModeIndex : 0u,
                 (g_xr && g_xr->sessionRunning) ? "true" : "false");
        result = buf;
    } else if (toolName == "set_orbit") {
        double az, el, zm;
        bool any = false;
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (McpJsonGetNumber(a, "azimuth_deg", az)) {
            g_inputState.yaw = (float)(az * 0.0174532925);
            any = true;
        }
        if (McpJsonGetNumber(a, "elevation_deg", el)) {
            if (el > 85.0) el = 85.0;
            if (el < -85.0) el = -85.0;
            g_inputState.pitch = (float)(el * 0.0174532925);
            any = true;
        }
        if (McpJsonGetNumber(a, "zoom", zm)) {
            if (zm < 0.1) zm = 0.1;
            if (zm > 10.0) zm = 10.0;
            g_inputState.viewParams.scaleFactor = (float)zm;
            any = true;
        }
        if (!any) {
            ok = false;
            result = "{\"error\":\"provide at least one of azimuth_deg, elevation_deg, zoom\"}";
        } else {
            // Agent input is input: reset the auto-orbit idle timer (mirrors the
            // shared MarkUserInput, which is private to the input handler).
            using namespace std::chrono;
            g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
                high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
            g_inputState.animationActive = false;
            snprintf(buf, sizeof(buf),
                     "{\"azimuth_deg\":%.1f,\"elevation_deg\":%.1f,\"zoom\":%.2f}",
                     g_inputState.yaw * 57.29578f, g_inputState.pitch * 57.29578f,
                     g_inputState.viewParams.scaleFactor);
            result = buf;
        }
    } else if (toolName == "frame_model") {
        if (!g_modelRenderer.hasModel()) {
            ok = false;
            result = "{\"error\":\"no model loaded — call load_model first\"}";
        } else {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.resetViewRequested = true;  // applied by the render loop next frame
            result = "{\"framed\":true}";
        }
    } else if (toolName == "set_transparent_background") {
        // Same code path as Ctrl+T: raise the shared input handler's request
        // flag and let the render loop stay the single place that flips
        // g_transparentBg, logs, toasts, and posts kBorderlessMsg. Omitting
        // 'enabled' toggles; passing it makes the call idempotent — the flag is
        // raised only when the target differs from the live state.
        const bool current = g_transparentBg.load();
        bool want = !current;               // no 'enabled' → toggle, like Ctrl+T
        McpJsonGetBool(a, "enabled", want); // absent/non-boolean keeps the toggle target
        const bool changing = (want != current);
        if (changing) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.transparentBgToggleRequested = true;
        }
        snprintf(buf, sizeof(buf), "{\"transparent_background\":%s,\"changed\":%s}",
                 want ? "true" : "false", changing ? "true" : "false");
        result = buf;
    } else if (toolName == "list_animations") {
        const int n = g_modelRenderer.animationCount();
        std::string clips = "[";
        for (int i = 0; i < n; i++) {
            std::string nm; float dur = 0;
            g_modelRenderer.getAnimationInfo(i, nm, dur);
            snprintf(buf, sizeof(buf), "%s{\"index\":%d,\"name\":\"%s\",\"duration_s\":%.2f}",
                     i ? "," : "", i, McpJsonEscape(nm).c_str(), dur);
            clips += buf;
        }
        clips += "]";
        snprintf(buf, sizeof(buf), ",\"active_index\":%d,\"playing\":%s}",
                 g_modelRenderer.activeAnimation(),
                 (g_modelRenderer.hasAnimations() && !g_modelRenderer.isPaused()) ? "true" : "false");
        result = "{\"animations\":" + clips + buf;
    } else if (toolName == "play_animation") {
        const int n = g_modelRenderer.animationCount();
        int target = -1;
        double idx; std::string nm;
        if (McpJsonGetNumber(a, "index", idx)) {
            target = (int)idx;
            if (target < 0 || target >= n) {
                ok = false;
                snprintf(buf, sizeof(buf), "{\"error\":\"index out of range (0..%d)\"}", n - 1);
                result = buf;
            }
        } else if (McpJsonGetString(a, "name", nm)) {
            for (int i = 0; i < n && target < 0; i++) {
                std::string c; float d;
                g_modelRenderer.getAnimationInfo(i, c, d);
                if (c == nm) target = i;
            }
            if (target < 0) {
                ok = false;
                result = "{\"error\":\"no clip named '" + McpJsonEscape(nm) +
                         "' — see list_animations\"}";
            }
        } else {
            target = g_modelRenderer.activeAnimation();  // resume the active clip
            if (target < 0) target = 0;
        }
        if (ok) {
            if (n == 0) {
                ok = false;
                result = "{\"error\":\"the loaded model has no animation clips\"}";
            } else {
                if (target != g_modelRenderer.activeAnimation())
                    g_modelRenderer.setActiveAnimation(target);
                g_modelRenderer.setPaused(false);
                std::string c; float d = 0;
                g_modelRenderer.getAnimationInfo(target, c, d);
                snprintf(buf, sizeof(buf),
                         "{\"playing\":\"%s\",\"index\":%d,\"duration_s\":%.2f}",
                         McpJsonEscape(c).c_str(), target, d);
                result = buf;
            }
        }
    } else if (toolName == "stop_animation") {
        g_modelRenderer.setPaused(true);
        std::string c; float d = 0;
        const int active = g_modelRenderer.activeAnimation();
        if (active >= 0) g_modelRenderer.getAnimationInfo(active, c, d);
        snprintf(buf, sizeof(buf), "{\"playing\":false,\"paused_clip\":%s%s%s}",
                 active >= 0 ? "\"" : "", active >= 0 ? McpJsonEscape(c).c_str() : "null",
                 active >= 0 ? "\"" : "");
        result = buf;
    } else {
        ok = false;
        result = "{\"error\":\"unhandled tool\"}";
    }

    return result;
}

// Declare the app identity + register the base agent tools (#47). Called once
// after xrCreateSession, on the main thread (before the render thread spawns).
// The animation tools are NOT registered here — they appear only once a model
// with clips loads (UpdateMcpAnimationTools). Failure is non-fatal by design:
// the MCP capability gate may simply be off; the viewer runs identically
// without an agent surface.
static void RegisterModelViewerMcpTools(XrSessionManager& xr) {
    if (!McpToolsResolved()) {
        LOG_INFO("XR_DXR_mcp_tools: not available — no agent surface");
        return;
    }
    XrMCPAppInfoDXR mcpAppInfo = {XR_TYPE_MCP_APP_INFO_DXR};
    strncpy(mcpAppInfo.appId, "modelviewer", sizeof(mcpAppInfo.appId) - 1);
    XrResult ar = g_pfnSetMcpAppInfo(xr.session, &mcpAppInfo);
    if (XR_FAILED(ar)) {
        LOG_INFO("XR_DXR_mcp_tools: appId not accepted (%d) — no agent surface", ar);
        return;
    }

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
    XrResult t1 = g_pfnRegisterMcpTool(xr.session, &loadTool);

    XrMCPToolInfoDXR statusTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    statusTool.name = "get_status";
    statusTool.description =
        "Read the viewer's live state: loaded model file and primitive count, "
        "animation clip count + active clip + playing flag, camera orbit "
        "(azimuth/elevation in degrees, world position, zoom factor), the "
        "transparent-background flag, active rendering-mode index, and whether "
        "the XR session is running.";
    statusTool.inputSchemaJson = "{\"type\":\"object\"}";
    XrResult t2 = g_pfnRegisterMcpTool(xr.session, &statusTool);

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
    XrResult t3 = g_pfnRegisterMcpTool(xr.session, &orbitTool);

    XrMCPToolInfoDXR frameTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    frameTool.name = "frame_model";
    frameTool.description =
        "Reset the camera to the loaded model's auto-fit framed pose (same as "
        "pressing Space): centers the model with comfortable headroom and "
        "restores zoom to 1. Requires a model to be loaded.";
    frameTool.inputSchemaJson = "{\"type\":\"object\"}";
    XrResult t4 = g_pfnRegisterMcpTool(xr.session, &frameTool);

    XrMCPToolInfoDXR transparencyTool = {XR_TYPE_MCP_TOOL_INFO_DXR};
    transparencyTool.name = "set_transparent_background";
    transparencyTool.description =
        "Turn the transparent background on or off — the agent equivalent of "
        "Ctrl+T: with it on, the model is composited over the desktop instead "
        "of the viewer's opaque background, and the window goes borderless. "
        "Omit 'enabled' to toggle the current state. The flip is applied by the "
        "render thread on the next frame, so the returned value is the "
        "REQUESTED state — read the settled one back with get_status.";
    transparencyTool.inputSchemaJson =
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\",\"description\":\"Target state; omit to toggle.\"}}}";
    XrResult t5 = g_pfnRegisterMcpTool(xr.session, &transparencyTool);

    g_mcpToolsReady = true;
    // Install the app dispatcher on the shared PollEvents hook (common v2.1.0):
    // PollEvents fetches the call args, invokes this, and submits the result —
    // so the model viewer no longer forks PollEvents to route its tool calls.
    xr.mcpToolHandler = McpDispatchToolCall;
    LOG_INFO("XR_DXR_mcp_tools: appId=modelviewer load_model=%d get_status=%d "
             "set_orbit=%d frame_model=%d set_transparent_background=%d",
             t1, t2, t3, t4, t5);

    // Sync the animation tools with whatever model is already loaded (the
    // bundled sample may have loaded before this call, depending on ordering).
    UpdateMcpAnimationTools();
}

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

static void ToggleFullscreen(HWND hwnd) {
    // Same rule as kBorderlessMsg below: a style swap must never change whether
    // this window is on screen. Under the DisplayXR workspace the runtime hides
    // the HWND at xrCreateSession, so preserve the WS_VISIBLE / WS_MINIMIZE bits
    // and only touch the z-order while the window is actually on screen.
    const LONG cur = GetWindowLong(hwnd, GWL_STYLE);
    const LONG keep = cur & (WS_VISIBLE | WS_MINIMIZE);
    const bool onScreen = (cur & WS_VISIBLE) != 0 && (cur & WS_MINIMIZE) == 0;
    HWND zOrder = onScreen ? HWND_TOP : nullptr;
    UINT swpFlags = SWP_FRAMECHANGED | (onScreen ? 0u : SWP_NOZORDER);
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE,
            (LONG)((g_savedWindowStyle & ~(DWORD)(WS_VISIBLE | WS_MINIMIZE)) | (DWORD)keep));
        SetWindowPos(hwnd, zOrder,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            swpFlags);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        g_savedWindowStyle = (DWORD)cur;
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, (LONG)(WS_POPUP | (DWORD)keep));
        SetWindowPos(hwnd, zOrder,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            swpFlags);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

static bool PointInFractionRect(int mouseX, int mouseY, int windowW, int windowH,
                                float xf, float yf, float wf, float hf) {
    if (windowW <= 0 || windowH <= 0) return false;
    float fx = (float)mouseX / (float)windowW;
    float fy = (float)mouseY / (float)windowH;
    return (fx >= xf && fx <= xf + wf && fy >= yf && fy <= yf + hf);
}

// All three buttons share the top bar's vertical band [BTN_BAR_Y, +barHeight];
// each owns its own x-column. Keeps hit-testing aligned with the rendered pills.
static bool IsClickOnLoadButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        OPEN_BTN_X_FRACTION, BTN_BAR_Y_FRACTION,
        OPEN_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

static bool IsClickOnModeButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        MODE_BTN_X_FRACTION, BTN_BAR_Y_FRACTION,
        MODE_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

static bool IsClickOnAnimButton(int mouseX, int mouseY, int windowW, int windowH) {
    // Only live when the model actually has clips (else the top-right corner
    // stays a normal scene-rotate region).
    if (!g_hasAnimations.load()) return false;
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        AnimBtnXFraction(), BTN_BAR_Y_FRACTION,
        ANIM_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

// Atlas capture is runtime-owned via xrCaptureAtlasDXR (XR_DXR_atlas_capture).
// App-side helpers (filename numbering + flash overlay) live in
// common/atlas_capture* — see dxr_capture::MakeCaptureAtlasPrefix /
// TriggerCaptureFlash / PostFlashRequest.

// A bundled environment.hdr, when present next to the exe, becomes the IBL
// source at startup (issue #70). Absent, the viewer keeps the procedural
// analytic sky — purely additive, and the HUD names whichever is active so a
// reference capture is self-documenting.
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

// Standalone (own window) vs. running as a client of the workspace shell's
// external multi-compositor — signalled by the current rendering mode not being
// requestable (the shell owns the mode). Same predicate the foreground-clip
// path uses; kept in one place so the two can't drift.
static bool IsStandaloneSession(const XrSessionManager* xr) {
    return (xr->renderingModeCount == 0) ||
           (xr->currentModeIndex < xr->renderingModeCount &&
            xr->renderingModeIsRequestable[xr->currentModeIndex]);
}

// Idle auto-orbit gate. The turntable exists to show off a *static* asset
// sitting in its own opaque window; there are two states where it works against
// the content instead:
//   • Standalone transparent background (Ctrl+T) — the model is punched through
//     onto the user's desktop as a floating object, and a floating object that
//     spins by itself reads as a glitch, not as an idle screensaver. Under the
//     workspace shell there is no punch-through illusion to break (the app is a
//     framed tile in a composed scene), so a static asset keeps orbiting there.
//   • A clip is playing — the asset already carries its own motion. Orbiting on
//     top of it double-animates the scene and fights whatever framing the clip
//     was authored for. This one holds everywhere, workspace included.
// This is a live gate, NOT a state change: 'M' still owns animateEnabled, and
// the turntable resumes (after a fresh 10 s idle countdown — the caller holds
// lastInputTimeSec at "now" while held) as soon as the condition clears.
static bool AutoOrbitSuppressed(const XrSessionManager* xr) {
    if (g_hasAnimations.load() && !g_modelRenderer.isPaused()) return true;
    if (g_transparentBg.load() && IsStandaloneSession(xr)) return true;
    return false;
}

static void TryAutoLoadBundledEnvironment() {
    char exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = strrchr(exePath, '/');
    if (!lastSlash) return;
    *(lastSlash + 1) = '\0';
    const std::string path = std::string(exePath) + "environment.hdr";
    if (!PathFileExistsA(path.c_str())) {
        LOG_INFO("No bundled environment.hdr (using the procedural analytic sky)");
        return;
    }
    if (g_modelRenderer.setEnvironment(path.c_str())) {
        LOG_INFO("Environment: %s", g_modelRenderer.environmentName().c_str());
    }
}

// Load a scene at startup. With an explicit path (first CLI arg) load that;
// otherwise fall back to the bundled sample.glb next to the exe.
static void TryAutoLoadBundledScene(const std::string& overridePath = std::string()) {
    std::string path;
    if (!overridePath.empty()) {
        if (!model_validate_file(overridePath)) {
            LOG_WARN("CLI model '%s' invalid/missing — falling back to bundled sample",
                     overridePath.c_str());
        } else {
            path = overridePath;
        }
    }
    if (path.empty()) {
        char exePath[MAX_PATH] = {0};
        if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return;
        // Strip basename
        char *lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) lastSlash = strrchr(exePath, '/');
        if (!lastSlash) return;
        *(lastSlash + 1) = '\0';
        path = std::string(exePath) + "sample.glb";
        if (!PathFileExistsA(path.c_str())) {
            LOG_INFO("No bundled scene at %s (skipping auto-load)", path.c_str());
            return;
        }
        if (!model_validate_file(path)) return;
    }
    LOG_INFO("Auto-loading scene: %s", path.c_str());
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    if (g_modelRenderer.loadModel(path.c_str())) {
        g_loadedFileName = model_basename(path);
        LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(), model_filesize_str(path).c_str());
        ApplyAutoFitForLoadedScene_locked();
    } else {
        LOG_WARN("Auto-load failed for %s", path.c_str());
    }
}

// Hand a picked path off to the render thread for scene load. Validates the
// extension first; on failure pops a MessageBox and returns false. Used by
// both the Win32 GetOpenFileNameA path and the #228 spatial picker result
// drained in the main loop.
static bool QueueSceneLoad(HWND hwnd, const std::string& path, bool fromUrl = false) {
    if (!model_validate_file(path)) {
        MessageBoxA(hwnd, "Invalid model file. Supported formats: .glb, .gltf", "Load Error", MB_OK | MB_ICONERROR);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
        g_pendingLoadPath = path;
        g_pendingLoadFromUrl = fromUrl;
    }
    g_loadRequested.store(true, std::memory_order_release);
    LOG_INFO("Queued model load: %s", path.c_str());
    return true;
}

// ============================================================================
// --src: fetch a remote asset into the per-user cache, then load it
// ============================================================================
// The launch contract lets a page hand the viewer a URL. The download is
// SYNCHRONOUS (WinHTTP) so it runs on a detached worker; the loaded result
// crosses back to the render thread through the SAME g_pendingLoadPath queue
// Ctrl+O, drag-drop and the spatial picker use — loading from the worker would
// put a second thread on the single VkQueue.

// Everything model_loader.cpp dispatches on. url_fetch.h resolves the cache
// file's extension against this list (URL path → Content-Type → magic bytes),
// so an extension NOT here is refused rather than saved under a guess.
static const std::vector<std::string>& SrcAllowedExtensions() {
    static const std::vector<std::string> kExts = {
        ".glb", ".gltf", ".stl", ".obj", ".fbx", ".usdz", ".usd", ".usda", ".usdc"};
    return kExts;
}

//! Percent-encode every byte that is not an unreserved URL character, so the
//! result can be embedded in a query value the parser will decode verbatim.
static std::string PercentEncodeComponent(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// Re-apply the launch policy to the FINAL URL after redirects. Re-running the
// real parser (rather than re-implementing "https or loopback") is the point:
// there is exactly one copy of the policy, and a redirect chain cannot walk a
// protocol launch out of it. A protocol launch is re-checked through the
// protocol form, which is what carries the stricter `fromProtocol` rules.
static bool SrcUrlAllowed(const std::string& finalUrl, bool fromProtocol) {
    if (fromProtocol) {
        const std::string probe =
            "displayxr-view://open?src=" + PercentEncodeComponent(finalUrl) + "&v=1";
        const dxr::LaunchArgs a = dxr::ParseLaunchArgs({probe});
        return a.ok() && a.srcKind == dxr::LaunchSrcKind::Url;
    }
    const dxr::LaunchArgs a = dxr::ParseLaunchArgs({std::string("--src=") + finalUrl});
    return a.ok() && a.srcKind == dxr::LaunchSrcKind::Url;
}

static void StartSrcFetch(HWND hwnd, const std::string& url, bool fromProtocol,
                          uint64_t maxBytes, bool noCache) {
    if (g_srcFetchInFlight.exchange(true)) {
        LOG_WARN("--src: a download is already in flight — ignoring %s", url.c_str());
        return;
    }
    std::thread([hwnd, url, fromProtocol, maxBytes, noCache]() {
        dxr::UrlFetchOptions opt;
        opt.cacheDir = dxr::DefaultCacheDir(L"ModelViewer");
        opt.allowedExtensions = SrcAllowedExtensions();
        opt.maxBytes = maxBytes;
        opt.noCache = noCache;
        opt.urlAllowed = [fromProtocol](const std::string& finalUrl) {
            return SrcUrlAllowed(finalUrl, fromProtocol);
        };
        // ~4 Hz. The toast chip re-rasterizes on every Show(), and this runs
        // inside the read loop — a per-chunk toast would cost more than the
        // download.
        auto lastToast = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        opt.progress = [&lastToast](uint64_t done, uint64_t total) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastToast < std::chrono::milliseconds(250)) return;
            lastToast = now;
            if (total > 0) {
                ToastF("Downloading... %d%%", (int)((done * 100ull) / total));
            } else {
                ToastF("Downloading... %llu KB", (unsigned long long)(done / 1024));
            }
        };

        LOG_INFO("--src: fetching %s (max %llu bytes%s%s)", url.c_str(),
                 (unsigned long long)maxBytes, noCache ? ", no-cache" : "",
                 fromProtocol ? ", protocol policy" : "");
        const dxr::UrlFetchResult r = dxr::FetchUrlToCache(url, opt);
        if (!r.ok) {
            LOG_ERROR("--src: download failed: %s", r.error.c_str());
            ToastF("Download failed - %s", r.error.c_str());
            g_srcFetchInFlight.store(false);
            return;
        }
        const std::string path = dxr::NarrowPathForFopen(dxr::Utf8FromWide(r.path));
        LOG_INFO("--src: %s -> %s (%llu bytes, %s)", url.c_str(), path.c_str(),
                 (unsigned long long)r.bytes, r.fromCache ? "cache hit" : "downloaded");
        if (!model_validate_file(path)) {
            // Toast, never MessageBox: a modal dialog from a worker thread
            // under a topmost shaped window is a trap the user cannot see.
            LOG_ERROR("--src: cached file is not a loadable model: %s", path.c_str());
            ToastF("Unsupported asset - %s", model_basename(path).c_str());
            g_srcFetchInFlight.store(false);
            return;
        }
        ToastF("%s  %s", r.fromCache ? "Cached" : "Downloaded",
               model_basename(path).c_str());
        QueueSceneLoad(hwnd, path, /*fromUrl=*/true);
        g_srcFetchInFlight.store(false);
    }).detach();
}

// Clamp a requested window rect INTO the 3D panel's desktop rect — never snap
// TO it. A caller that asked for 800x800 at (300,300) gets exactly that size;
// only the origin moves, and only far enough to bring the window fully onto
// the panel. Gated on XR_DXR_display_info v18's isPanelConfirmed: an
// unconfirmed rect is just the primary monitor's (sim_display, or a platform
// whose panel-origin plumbing is still open), and clamping onto the WRONG
// monitor is worse than not clamping at all. An all-zero rect is "unresolved".
static void ClampRectIntoPanel(int32_t& x, int32_t& y, int32_t w, int32_t h) {
    if (!g_displayPanelConfirmed) return;
    const XrRect2Di& r = g_displayDesktopRect;
    if (r.extent.width <= 0 || r.extent.height <= 0) return;
    const int32_t left = r.offset.x;
    const int32_t top = r.offset.y;
    const int32_t right = left + r.extent.width;
    const int32_t bottom = top + r.extent.height;
    const int32_t nx = (w >= r.extent.width) ? left
                                             : (std::min)((std::max)(x, left), right - w);
    const int32_t ny = (h >= r.extent.height) ? top
                                              : (std::min)((std::max)(y, top), bottom - h);
    if (nx != x || ny != y) {
        LOG_INFO("Undock rect clamped into panel rect (%d,%d %dx%d): (%d,%d) -> (%d,%d)",
                 left, top, r.extent.width, r.extent.height, x, y, nx, ny);
    }
    x = nx;
    y = ny;
}

// Apply a launch that arrived AFTER startup (WM_COPYDATA from a second
// process the single-instance guard turned away). Only the things a running,
// possibly-shaped window may legally change: position/size (a SetWindowPos
// move is not a style change, so transparency rule 2 holds), the vHeight pin,
// and the asset. Window-owning thread only.
static void ApplyForwardedLaunch(HWND hwnd, const dxr::LaunchArgs& a) {
    if (a.hasRect) {
        int32_t x = a.rectX, y = a.rectY;
        ClampRectIntoPanel(x, y, a.rectW, a.rectH);
        // The rect names the CONTENT area, exactly as it does at creation: a
        // popup's client rect IS its window rect, and a framed window's is
        // inflated by the non-client area. Applying the raw rect to a framed
        // window would make the same URL frame the model differently
        // depending on whether it opened a new window or moved an old one.
        RECT want = {x, y, x + a.rectW, y + a.rectH};
        if (!g_borderless.load()) {
            AdjustWindowRect(&want, (DWORD)GetWindowLong(hwnd, GWL_STYLE), FALSE);
        }
        LOG_INFO("Forwarded launch: moving to (%d,%d %dx%d) [content (%d,%d %dx%d)]",
                 (int)want.left, (int)want.top, (int)(want.right - want.left),
                 (int)(want.bottom - want.top), x, y, a.rectW, a.rectH);
        SetWindowPos(hwnd, nullptr, want.left, want.top, want.right - want.left,
                     want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (a.hasVh) {
        LOG_INFO("Forwarded launch: vHeight pinned to %.3f m", a.vh);
        g_vhOverride.store(a.vh, std::memory_order_relaxed);
        g_fitVHeight = a.vh;
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_inputState.viewParams.virtualDisplayHeight = a.vh;
        g_inputState.viewParams.scaleFactor = 1.0f;
    }
    if (a.srcKind == dxr::LaunchSrcKind::Url) {
        StartSrcFetch(hwnd, a.src, a.fromProtocol, a.maxBytes, a.noCache);
    } else if (a.srcKind == dxr::LaunchSrcKind::LocalPath) {
        QueueSceneLoad(hwnd, dxr::NarrowPathForFopen(a.src));
    } else if (!a.positionalPath.empty()) {
        QueueSceneLoad(hwnd, dxr::NarrowPathForFopen(a.positionalPath));
    }
}

// Open a file dialog and load a .ply or .spz scene (called from main thread).
//
// Path A — workspace + Tier 1 picker available:
//     xrRequestFilePickerDXR fires async. The completion event is drained
//     by PollEvents (common/xr_session_common.cpp) into xr.filePickerLast*;
//     the main loop dispatches to QueueSceneLoad on result arrival.
//
// Path B — workspace mode but no controller / no Tier 1 picker, OR running
// outside a workspace (standalone window), OR running on a non-DisplayXR
// OpenXR runtime: xrRequestFilePickerDXR either returns
// XR_FILE_PICKER_FALLBACK_TIER0_DXR (workspace fallback) or the PFN is
// null (extension absent). Either way fall through to GetOpenFileNameA
// and keep the existing standalone UX.
static void OpenLoadDialog(HWND hwnd) {
    // Already showing a spatial picker — second click on Open is a
    // no-op. Without this guard the prior "filePickerInFlight" check
    // would skip Path A and fall through to GetOpenFileNameA, opening
    // BOTH the spatial picker AND a flat Win32 dialog stacked on top.
    if (g_xr != nullptr && g_xr->filePickerInFlight) {
        LOG_INFO("[#228] OpenLoadDialog: spatial picker already in flight, ignoring");
        return;
    }

    // Path A: spatial picker, when available + not already in flight.
    if (g_xr != nullptr && g_xr->pfnRequestFilePickerEXT != nullptr &&
        !g_xr->filePickerInFlight) {
        XrFilePickerInfoDXR info = {XR_TYPE_FILE_PICKER_INFO_DXR};
        info.mode = XR_FILE_PICKER_MODE_OPEN_DXR;
        strncpy(info.title, "Load 3D Model",
                sizeof(info.title) - 1);
        info.filterCount = 3;
        strncpy(info.filters[0].description, "3D Models",
                sizeof(info.filters[0].description) - 1);
        strncpy(info.filters[0].extensions, "*.glb;*.gltf;*.stl;*.obj;*.fbx;*.usdz;*.usd;*.usda;*.usdc",
                sizeof(info.filters[0].extensions) - 1);
        strncpy(info.filters[1].description, "Binary glTF",
                sizeof(info.filters[1].description) - 1);
        strncpy(info.filters[1].extensions, "*.glb",
                sizeof(info.filters[1].extensions) - 1);
        strncpy(info.filters[2].description, "glTF",
                sizeof(info.filters[2].description) - 1);
        strncpy(info.filters[2].extensions, "*.gltf",
                sizeof(info.filters[2].extensions) - 1);

        XrAsyncRequestIdDXR rid = 0;
        XrResult r = g_xr->pfnRequestFilePickerEXT(g_xr->session, &info, &rid);
        if (r == XR_SUCCESS) {
            g_xr->filePickerInFlight = true;
            g_xr->filePickerRequestId = rid;
            LOG_INFO("[#228] xrRequestFilePickerDXR -> rc=0x%x requestId=%llu",
                r, (unsigned long long)rid);
            return; // wait for completion event in the main loop
        }
        // r == XR_FILE_PICKER_FALLBACK_TIER0_DXR or an error → fall through.
        LOG_INFO("[#228] xrRequestFilePickerDXR -> rc=0x%x (falling back to Win32)", r);
    }

    // Path B: existing Win32 file dialog (unchanged behavior).
    OPENFILENAMEA ofn = {};
    char filePath[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "3D Models (glb;gltf;stl;obj;fbx;usd*)\0*.glb;*.gltf;*.stl;*.obj;*.fbx;*.usdz;*.usd;*.usda;*.usdc\0glTF (*.glb;*.gltf)\0*.glb;*.gltf\0STL (*.stl)\0*.stl\0OBJ (*.obj)\0*.obj\0FBX (*.fbx)\0*.fbx\0USD (*.usd*)\0*.usdz;*.usd;*.usda;*.usdc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Load 3D Model";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        QueueSceneLoad(hwnd, std::string(filePath));
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // #833: RMB-drag moves the borderless punch-through window (Unity
    // desktop-avatar convention). Consumed BEFORE the input handler so a
    // window-drag doesn't double as camera input. Opaque-element gating is
    // free: shaped-out pixels never deliver mouse messages.
    if (g_windowDrag.handleMessage(hwnd, msg, wParam, lParam, g_borderless.load())) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        // UpdateInputState above already set leftButton/dragging=true. For
        // button clicks (which post a message to run a modal dialog or change
        // mode), clear that drag state — otherwise the modal eats the
        // matching WM_LBUTTONUP and subsequent mouse motion is interpreted as
        // a scene drag.
        if (IsClickOnLoadButton(mx, my, g_windowWidth, g_windowHeight)) {
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_inputState.leftButton = false;
                g_inputState.dragging = false;
            }
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        if (IsClickOnModeButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            // Mode button = cycle request (V-key equivalent). Main loop
            // reads runtime's current mode and computes the target.
            g_inputState.cycleRenderingModeRequested = true;
            return 0;
        }
        if (IsClickOnAnimButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            // Animation button = next clip (N-key equivalent).
            g_inputState.cycleClipRequested = true;
            return 0;
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_USER + 1:
        OpenLoadDialog(hwnd);
        return 0;

    case WM_DROPFILES: {
        // Drop a model or an .hdr environment onto the window. macOS has had
        // this since the Cocoa entry point was written; Windows never wired it,
        // even though model_validate_file() already accepts .hdr expressly so
        // "the existing open dialog / drag-drop paths can deliver one". The
        // whole load path is reused as-is: QueueSceneLoad hands the path to the
        // render thread under g_pendingLoadPathMutex, exactly as Ctrl+O and the
        // #228 spatial picker do, so a drop is indistinguishable downstream.
        HDROP drop = (HDROP)wParam;
        const UINT count = DragQueryFileA(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            char path[MAX_PATH] = {};
            if (DragQueryFileA(drop, 0, path, MAX_PATH) > 0) {
                if (count > 1)
                    LOG_INFO("Drop: %u files, loading the first (%s)", count, path);
                QueueSceneLoad(hwnd, std::string(path));
            }
        }
        DragFinish(drop);
        return 0;
    }

    case WM_COPYDATA: {
        // A second launch of displayxr-view: hit the single-instance mutex and
        // handed its URL here rather than opening a second floating window.
        // The payload is UNTRUSTED (any page on an allowed origin can produce
        // it), so it goes through the same parser + policy the original launch
        // did — never straight into SetWindowPos or a load.
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lParam;
        if (!cds || cds->dwData != dxr::kViewProtocolCopyDataId) break;
        std::string url;
        if (cds->lpData && cds->cbData > 0) {
            const char* p = (const char*)cds->lpData;
            url.assign(p, strnlen(p, cds->cbData));
        }
        if (url.empty()) {
            LOG_WARN("WM_COPYDATA: empty displayxr-view payload ignored");
            return 0;
        }
        LOG_INFO("WM_COPYDATA: forwarded launch %s", url.c_str());
        const dxr::LaunchArgs a = dxr::ParseLaunchArgs({url});
        for (const std::string& w : a.warnings) LOG_WARN("forwarded launch: %s", w.c_str());
        if (!a.ok()) {
            for (const std::string& e : a.errors) LOG_ERROR("forwarded launch: %s", e.c_str());
            ToastF("Refused - %s", a.errors.front().c_str());
            return 0;
        }
        ApplyForwardedLaunch(hwnd, a);
        return 0;
    }

    case dxr_capture::kFlashUserMsg:
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;

    case WM_NCHITTEST:
        // Shaped borderless mode: the OS only delivers hits inside the
        // region; claim them for normal app input (outside reaches the
        // desktop natively).
        if (g_borderless.load()) return HTCLIENT;
        break;

    case kBorderlessMsg: {
        // Ctrl+T style swap, on the window-owning thread. Client rect is
        // preserved across the swap (the avatar recipe); shaping happens on
        // the render thread once coverage lands.
        const bool borderless = wParam != 0;
        RECT client = {};
        GetClientRect(hwnd, &client);
        POINT tl = {0, 0};
        ClientToScreen(hwnd, &tl);
        // Ctrl+T must never change whether this window is on screen. Under the
        // DisplayXR workspace the runtime hides this HWND at xrCreateSession
        // (its pixels reach the panel through the composed atlas); writing
        // WS_VISIBLE here un-hid it and HWND_TOPMOST then put it on top of the
        // shell. Keep the WS_VISIBLE / WS_MINIMIZE bits exactly as they are,
        // and only touch the z-order while the window is actually on screen.
        const LONG cur = GetWindowLong(hwnd, GWL_STYLE);
        const LONG keep = cur & (WS_VISIBLE | WS_MINIMIZE);
        const bool onScreen = (cur & WS_VISIBLE) != 0 && (cur & WS_MINIMIZE) == 0;
        const DWORD style = (borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW) | (DWORD)keep;
        SetWindowLong(hwnd, GWL_STYLE, (LONG)style);
        RECT want = {tl.x, tl.y, tl.x + client.right, tl.y + client.bottom};
        AdjustWindowRect(&want, style, FALSE);
        // Transparent mode floats above other apps (the avatar behavior):
        // clicks punched through to a window behind activate it, but the
        // scene stays on top. Ctrl+T off returns to the normal z-band.
        HWND zOrder = onScreen ? (borderless ? HWND_TOPMOST : HWND_NOTOPMOST) : nullptr;
        UINT swpFlags = SWP_FRAMECHANGED | SWP_NOACTIVATE | (onScreen ? 0u : SWP_NOZORDER);
        SetWindowPos(hwnd, zOrder, want.left, want.top,
                     want.right - want.left, want.bottom - want.top,
                     swpFlags);
        g_borderless.store(borderless);
        return 0;
    }

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        // Ctrl+O = open a model (uniform across demos + platforms, incl. the
        // mediaplayer). Strict: Ctrl must be held. WM_USER+1 runs the picker on
        // the message loop. (Was bare L — retired for the uniform chord.)
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        // I key = capture multi-view atlas
        if (wParam == 'I' || wParam == 'i') {
            g_captureAtlasRequested.store(true);
        }
        // Viewing conditions (issue #70): [ / ] step exposure in quarter stops,
        // G cycles the tone curve. Both show in the HUD, so a screen capture
        // records the grading it was taken under. (T is eye-tracking.)
        if (wParam == VK_OEM_4 || wParam == VK_OEM_6) {   // '[' and ']'
            const float step = (wParam == VK_OEM_6) ? 0.25f : -0.25f;
            g_modelRenderer.setExposureEV(g_modelRenderer.exposureEV() + step);
            LOG_INFO("Exposure: %+.2f EV", g_modelRenderer.exposureEV());
            return 0;
        }
        if (wParam == 'G') {
            g_modelRenderer.cycleToneCurve();
            LOG_INFO("Tone curve: %s", g_modelRenderer.toneCurveName());
            return 0;
        }
        // Dynamic-recenter pins: P arms, then X/Y/Z toggle that axis' pin.
        // onKey consumes P (arm) and X/Y/Z (while armed); otherwise it falls
        // through untouched (X/Y/Z are not bound elsewhere).
        if (wParam == 'P' || wParam == 'X' || wParam == 'Y' || wParam == 'Z') {
            const bool wasArmed = g_recenter.armed();
            if (g_recenter.onKey((char)wParam)) {
                char lbl[24];
                g_recenter.hudLabel(lbl, sizeof(lbl));
                LOG_INFO("Recenter: %s", lbl);
                // Confirm on screen. The arm prompt stays up for the whole arm
                // window so the toast and the armed state expire together; a
                // completed toggle uses the default (shorter) toast lifetime.
                wchar_t w[64];
                swprintf(w, 64, L"Recenter  %hs", lbl);
                g_toast.Show(w, wasArmed ? dxr::ToastState::kDefaultSeconds
                                         : dxr::RecenterControl::kArmSeconds);
                return 0;
            }
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// `borderless` = the undock contract's --transparent: create the window
// WS_POPUP + topmost from the FIRST frame instead of flipping styles after
// creation. Transparency rule 2 (#833): never change window styles while the
// window is shaped — Ctrl+T can only do it because it un-shapes first, and a
// startup flip would make the undocked window flash a frame it never wanted.
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height, int x, int y,
                            bool borderless) {
    LOG_INFO("Creating application window (%dx%d) at (%d,%d) style=%s", width, height, x, y,
             borderless ? "WS_POPUP topmost (undock)" : "WS_OVERLAPPEDWINDOW");

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null background brush + WS_EX_NOREDIRECTIONBITMAP (below) are required
    // by the runtime's transparent-window bridge (DComp + KMT shared texture).
    // Both must be set even when the demo defaults to opaque, because session
    // transparency is wired at xrCreateSession time and cannot be toggled later.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return nullptr;
        }
    }

    // A popup has no non-client area, so client rect == window rect: do NOT
    // grow the requested rect. An undock caller placed the window to match a
    // rect it measured on its own page — inflating it by a title bar and
    // borders would put the model somewhere else than where the page expects.
    // NOT WS_VISIBLE: a visible top-level window is ACTIVATED at creation, and
    // undocking must not pull focus off the page that spawned us. The window
    // is shown later with SW_SHOWNOACTIVATE, exactly where the framed path
    // already shows itself — which also spares the user an empty frame for
    // the couple of seconds OpenXR + Vulkan init takes.
    const DWORD style = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rect = { 0, 0, width, height };
    if (!borderless) {
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    }

    // WS_EX_TOPMOST while borderless: the undocked model floats over whatever
    // spawned it (the avatar recipe) — a click that punches through activates
    // the window behind without ever covering the asset.
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | (borderless ? WS_EX_TOPMOST : 0u);

    // INV-1.3 (runtime#715): (x, y) is the 3D panel top-left in virtual-screen
    // pixels (XrDisplayDesktopPositionDXR) — open the window on the panel, not
    // the primary monitor. (0,0) = primary/unknown, a safe create position.
    // With --rect it is instead the caller's requested origin.
    HWND hwnd = CreateWindowEx(exStyle, WINDOW_CLASS,
        g_windowTitle.empty() ? WINDOW_TITLE : g_windowTitle.c_str(),
        style,
        x, y,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    // Accept dropped models / .hdr environments (WM_DROPFILES). This sets the
    // WS_EX_ACCEPTFILES *extended* style, so it survives the Ctrl+T borderless
    // swap, which rewrites GWL_STYLE only.
    DragAcceptFiles(hwnd, TRUE);

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

// Wall time inside ModelRenderer::renderEye, summed over the frame's views.
//
// The fps line above cannot answer "what did this cost": the session is
// vsync-locked, so a change that eats real GPU time still reports a flat 16.67
// ms right up until it blows the budget entirely. renderEye ends in
// vkQueueWaitIdle, so its wall time IS that view's GPU pass - which makes this
// the honest number for a before/after (MSAA, a new lobe) at a workload that
// still fits in the frame. Published only under DXR_MODELVIEWER_PERF_LOG.
static std::atomic<double> g_sceneMsAccum{0.0};
static std::atomic<int>    g_sceneMsFrames{0};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;
    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;
    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
        // DXR_MODELVIEWER_PERF_LOG=1 puts what the HUD shows into the log too.
        // The HUD is a separate composited layer, so it is not in an atlas
        // capture and cannot be read back headlessly - which makes a claim
        // about a change's frame-time cost (MSAA, say) unverifiable without
        // someone at the panel. One line per second, opt-in, off by default.
        static const bool perfLog = [] {
            const char* e = std::getenv("DXR_MODELVIEWER_PERF_LOG");
            return e && *e && *e != '0';
        }();
        if (perfLog) {
            const int    n  = g_sceneMsFrames.exchange(0);
            const double ms = g_sceneMsAccum.exchange(0.0);
            LOG_INFO("perf: %.1f fps, %.2f ms/frame, scene %.3f ms/frame (%d frames)",
                     stats.fps, 1000.0f / stats.fps, (n > 0) ? (ms / n) : 0.0, n);
        }
    }
}

// Render a simple "no scene" placeholder by clearing to dark gray
static void RenderPlaceholder(VkDevice device, VkQueue queue, VkCommandPool cmdPool,
                               VkImage image, uint32_t width, uint32_t height) {
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Transparent mode punches through on alpha, and the click-through region
    // is derived from it — so an opaque slate here would park a dark rectangle
    // over the desktop for as long as no model is loaded (exactly the window
    // an undock launch spends downloading its --src). Clear to fully
    // transparent instead; the toast band stays in-region and carries the
    // progress. Opaque mode keeps the slate it always had.
    VkClearColorValue clearColor = g_transparentBg.load()
        ? VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}
        : VkClearColorValue{{0.1f, 0.1f, 0.12f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    // Transition to COLOR_ATTACHMENT
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkDevice vkDevice,
    VkQueue graphicsQueue,
    uint32_t queueFamilyIndex,
    VkInstance vkInstance,
    VkPhysicalDevice physDevice,
    std::vector<VkImage>* swapchainVkImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    VkBuffer hudStagingBuffer,
    void* hudStagingMapped,
    VkCommandPool hudCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages,
    VkCommandPool loadBtnCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* loadBtnSwapchainImages,
    uint32_t loadBtnWidth,
    uint32_t loadBtnHeight)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Command pool for placeholder rendering
    VkCommandPool renderCmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &renderCmdPool);
    }

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool animateToggle = false;
        bool loadReq = false;
        bool cycleClip = false;
        bool playPause = false;
        // Toast-worthy state changes latched under the input lock, posted after
        // it is released (ToastState takes its own lock — no nesting).
        bool bgToggled = false, bgNow = false;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
        }
        // Pitch is used as-is (#396 W7): shared input_handler.cpp mutates pitch
        // with `-= dy` (cube_handle convention), and model_renderer now flips
        // Vulkan-Y at the RASTER stage (negative VkViewport.height) instead of
        // reflecting the view matrix — a raster flip doesn't invert rotational
        // handedness, so no app-side negation is needed anymore.
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            resetRequested = g_inputState.resetViewRequested;
            animateToggle = g_inputState.animateToggleRequested;
            loadReq = g_inputState.loadRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.teleportRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            // ModeSwitch consumes the V/0-8 flags off inputSnapshot (captured
            // above); clear them on the shared state so they fire exactly once.
            g_inputState.cycleRenderingModeRequested = false;
            g_inputState.absoluteRenderingModeRequested = -1;
            g_inputState.eyeTrackingModeToggleRequested = false;
            if (g_inputState.transparentBgToggleRequested) {
                g_inputState.transparentBgToggleRequested = false;
                bool now = !g_transparentBg.load();
                g_transparentBg.store(now);
                LOG_INFO("Transparent background: %s (Ctrl+T)", now ? "ON" : "OFF");
                bgToggled = true;
                bgNow = now;
                // #833: transparent mode is borderless + region punch-through
                // (see kBorderlessMsg); the style swap runs on the window
                // thread. Un-shaping on OFF happens in the render loop below.
                PostMessage(hwnd, kBorderlessMsg, now ? 1 : 0, 0);
            }
            g_inputState.animateToggleRequested = false;
            g_inputState.loadRequested = false;
            cycleClip = g_inputState.cycleClipRequested;
            g_inputState.cycleClipRequested = false;
            playPause = g_inputState.playPauseRequested;
            g_inputState.playPauseRequested = false;
            if (animateToggle) {
                g_inputState.animateEnabled = !g_inputState.animateEnabled;
                inputSnapshot.animateEnabled = g_inputState.animateEnabled;
            }
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // ── Confirmations for state the user just changed ────────────────────
        // Outside the input lock. Auto-orbit and reset are toasted here rather
        // than at the keypress because the app decides the RESULTING state.
        if (bgToggled) {
            ToastF("Background  %s", bgNow ? "TRANSPARENT" : "OPAQUE");
        }
        if (animateToggle) {
            // Auto-orbit starts on an idle countdown, so without a toast the
            // key looks inert for a second or two. Name the held state too —
            // "ON" with nothing ever rotating (transparent / clip playing) is
            // the same inert-looking key with a more confusing label.
            if (inputSnapshot.animateEnabled && AutoOrbitSuppressed(xr)) {
                ToastF("Auto-Orbit  ON (held: %s)",
                       g_hasAnimations.load() ? "clip playing" : "transparent");
            } else {
                ToastF("Auto-Orbit  %s", inputSnapshot.animateEnabled ? "ON" : "OFF");
            }
        }
        if (resetRequested) {
            ToastF("View reset");
        }

        // Request main thread to open file dialog when L key or Load button was pressed.
        if (loadReq) {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        }

        // Drain a queued scene load (set by OpenLoadDialog on the main
        // thread). We must run loadScene here because it submits Vulkan work
        // on the graphics queue, and that queue is exclusively driven by this
        // (render) thread for per-frame submissions — see g_pendingLoadPath.
        if (g_loadRequested.exchange(false, std::memory_order_acquire)) {
            std::string path;
            bool loadFromUrl = false;
            {
                std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
                path = std::move(g_pendingLoadPath);
                g_pendingLoadPath.clear();
                loadFromUrl = g_pendingLoadFromUrl;
                g_pendingLoadFromUrl = false;
            }
            if (!path.empty()) {
                LOG_INFO("Loading model: %s", path.c_str());
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                if (g_modelRenderer.loadModel(path.c_str())) {
                    g_loadedFileName = model_basename(path);
                    LOG_INFO("Scene loaded: %s (%s)", g_loadedFileName.c_str(),
                        model_filesize_str(path).c_str());
                    ApplyAutoFitForLoadedScene_locked();
                    ToastF("Loaded  %s", g_loadedFileName.c_str());
                } else {
                    LOG_ERROR("Failed to load scene: %s", path.c_str());
                    // A file the USER picked still gets the modal box it
                    // always got. A file a URL supplied gets a toast: this
                    // runs on the render thread, so the box stops the scene,
                    // and under the undock contract it would sit on a topmost
                    // shaped window that whoever sent the link cannot dismiss.
                    ToastF("Load failed - %s", model_basename(path).c_str());
                    if (!loadFromUrl && !LaunchQuiet()) {
                        MessageBoxA(hwnd,
                            "Failed to load scene file.\nThe file may be corrupt or unsupported.",
                            "Load Error", MB_OK | MB_ICONERROR);
                    }
                }
            }
        }

        // Rendering mode requests (V/mode-button=cycle, 0-8=absolute) through the
        // shared ModeSwitch sequencer: eases viewParams.ipdFactor around the switch
        // and fires xrRequestDisplayRenderingModeDXR on the right frame. Ramped ipd
        // lands on inputSnapshot.viewParams.ipdFactor (what the render path reads).
        // Runtime owns current mode via xr->currentModeIndex.
        XrSessionUpdateModeSwitch(*xr, inputSnapshot, perfStats.deltaTime);

        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeDXR newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                    ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
                    XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
            }
        }

        UpdatePerformanceStats(perfStats);
        // Clip playback (N=next, K=play/pause). Render-thread only, like the
        // updateAnimation call below; apply before it so this frame reflects it.
        // Applied BEFORE the camera update so the auto-orbit gate below sees
        // this frame's play/pause state, not the previous frame's.
        if (cycleClip) g_modelRenderer.cycleAnimation();
        if (playPause) g_modelRenderer.togglePaused();

        // Auto-orbit gate (see AutoOrbitSuppressed). Drop the turntable for this
        // frame without touching the user's 'M' state, and hold the idle clock
        // at "now" so the 10 s countdown restarts when the gate clears — leaving
        // it stale would snap the orbit on the instant the user exits
        // transparent mode or pauses the clip.
        const bool orbitEnabledByUser = inputSnapshot.animateEnabled;
        const bool orbitSuppressed = orbitEnabledByUser && AutoOrbitSuppressed(xr);
        if (orbitSuppressed) {
            using namespace std::chrono;
            inputSnapshot.animateEnabled = false;
            inputSnapshot.lastInputTimeSec = (double)duration_cast<microseconds>(
                high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
        }
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);
        // Report the clip state AFTER applying, so the toast names the clip the
        // user actually landed on rather than echoing the keypress. A static
        // model has no clips — say so instead of flashing an empty chip.
        if (cycleClip || playPause) {
            std::string clip; int ci = 0, cn = 0; float ct = 0, cd = 0; bool playing = false;
            std::lock_guard<std::mutex> lk(g_sceneMutex);
            if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing)) {
                if (cycleClip) {
                    ToastF("Clip  %s  (%d/%d)", clip.c_str(), ci + 1, cn);
                } else {
                    ToastF("%s  %s", playing ? "Playing" : "Paused", clip.c_str());
                }
            } else {
                ToastF("No animation in this model");
            }
        }
        // Advance node/TRS animation once per frame (no-op for static models).
        g_modelRenderer.updateAnimation(perfStats.deltaTime);

        // Re-derive the base against the viewport the runtime actually resolved
        // (published from the previous frame's locate) and advance the move.
        // Runs before the reset block so a Space in the same frame wins.
        RefitForViewport(perfStats.deltaTime);
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot.viewParams.virtualDisplayHeight =
                g_inputState.viewParams.virtualDisplayHeight;
        }

        // On Space-reset: shared UpdateCameraMovement returns to (0,0,0) + default
        // vHeight. For the splat demo, restore the per-scene auto-fit pose instead.
        if (resetRequested && g_fitValid.load()) {
            inputSnapshot.cameraPosX = g_fitCenter[0];
            inputSnapshot.cameraPosY = g_fitCenter[1];
            inputSnapshot.cameraPosZ = g_fitCenter[2];
            inputSnapshot.yaw = g_fitYaw;
            inputSnapshot.viewParams.virtualDisplayHeight = g_fitVHeight;
            // Land any in-flight refit on the reset target so the animation
            // cannot drag the base back off it over the next frames.
            g_fitTransition.start(g_fitVHeight, g_fitVHeight, 0.0f);
        }

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            // Pose slerp and auto-orbit mutate yaw/pitch each frame — copy back.
            g_inputState.yaw = inputSnapshot.yaw;
            g_inputState.pitch = inputSnapshot.pitch;
            g_inputState.transitioning = inputSnapshot.transitioning;
            g_inputState.transitionT = inputSnapshot.transitionT;
            g_inputState.animationActive = inputSnapshot.animationActive;
            // Held idle clock (gate above) — persist it, otherwise the shared
            // state keeps the stale timestamp and the countdown never restarts.
            if (orbitSuppressed) g_inputState.lastInputTimeSec = inputSnapshot.lastInputTimeSec;
            if (resetRequested) {
                g_inputState.viewParams = inputSnapshot.viewParams;
                // Auto-orbit always on; reset only clears the in-flight
                // transition. The shared UpdateCameraMovement may set
                // animateEnabled=false on Space — re-assert true here. Except
                // under DXR_MODELVIEWER_DETERMINISTIC: loading a scene requests a
                // reset (TryAutoLoadBundledScene), so an unconditional true here
                // restarted the orbit on every launch and no two captures framed
                // the model the same way.
                g_inputState.animateEnabled = !DeterministicCaptureRequested();
                g_inputState.transitioning = false;
            }
        }

        // Bind the virtual-display rig to a moving/skinned subject: center the
        // convergence plane on the smoothed skeleton centroid so the subject
        // stays framed and at the ZDP as it animates. Position-only — yaw/pitch
        // (orbit) and vHeight (zoom) stay user-driven. No-op for static models
        // (getAnimatedAnchor returns false). Applied to inputSnapshot only, so
        // g_inputState keeps the user's intended pose for when no model is bound.
        //
        // Per-axis pins (P then X/Y/Z; DXR_RECENTER_PIN): a pinned axis tracks the
        // anchor, an unpinned axis returns to the fit centre. The user's WASDEQ
        // offset (cameraPos - fitCentre, applied by UpdateCameraMovement above)
        // adds on top of both, so pinning never disables movement. Default XYZ +
        // zero offset == the historical hard-pin (no behaviour change).
        {
            float anchor[3];
            if (g_fitValid.load() && g_modelRenderer.getAnimatedAnchor(anchor)) {
                const dxr::RecenterPins pins = g_recenter.pins();
                const float offX = inputSnapshot.cameraPosX - g_fitCenter[0];
                const float offY = inputSnapshot.cameraPosY - g_fitCenter[1];
                const float offZ = inputSnapshot.cameraPosZ - g_fitCenter[2];
                inputSnapshot.cameraPosX = (pins.x ? anchor[0] : g_fitCenter[0]) + offX;
                inputSnapshot.cameraPosY = (pins.y ? anchor[1] : g_fitCenter[1]) + offY;
                inputSnapshot.cameraPosZ = (pins.z ? anchor[2] : g_fitCenter[2]) + offZ;
            }
        }

        // Shared poll (common v2.1.0): standard events + XR_DXR_mcp_tools
        // dispatch via xr.mcpToolHandler (installed in RegisterModelViewerMcpTools).
        // The app used to fork this as PollEventsModelViewer solely to swap the
        // MCP handler — the v2.1.0 hook makes that fork unnecessary.
        PollEvents(*xr);

        // #228 Tier 1: drain a spatial-picker result if one arrived this
        // tick. PollEvents wrote the path + result code onto the session
        // manager; we route it through the same QueueSceneLoad path the
        // Win32 GetOpenFileNameA branch uses. The render thread picks the
        // queued path up via g_pendingLoadPath.
        if (xr->filePickerHasResult) {
            xr->filePickerHasResult = false;
            if (xr->filePickerLastResult == XR_FILE_PICKER_RESULT_SUCCESS_DXR &&
                xr->filePickerLastPath[0] != '\0') {
                QueueSceneLoad(hwnd, std::string(xr->filePickerLastPath));
            } else if (xr->filePickerLastResult == XR_FILE_PICKER_RESULT_CANCELLED_DXR) {
                LOG_INFO("[#228] User cancelled spatial picker — no scene load");
            } else {
                // PICKER_FAILED / INVALID_PATH — log and silently drop.
                // Don't auto-fall-back to Win32: the user already cancelled
                // out of the spatial flow, surfacing another dialog would
                // feel like a bug. They can click Load again if needed.
                LOG_WARN("[#228] Spatial picker delivered result=%d (no load)",
                    (int)xr->filePickerLastResult);
            }
        }

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                // Sized to runtime's max possible view count (sim_display Quad mode = 4).
                // Active mode's view count drives how many slots are actually filled and submitted.
                XrCompositionLayerProjectionView projectionViews[8] = {};
                bool rendered = false;
                bool hudSubmitted = false;
                bool loadBtnSubmitted = false;

                // Aspect-preserving HUD layer footprint (fixes demo-gs#8).
                // The HUD swapchain has a fixed pixel aspect (hudWidth × hudHeight,
                // sized once at session create). When the workspace tile is
                // resized to a different aspect, the runtime stretches the
                // swapchain per-axis to fit the layer rect — which distorts
                // glyphs and button shapes. Fix: pick layer-rect fractions
                // (layerFracW × layerFracH, in HWND fractions) that match the
                // swapchain aspect so both axes stretch by the same factor
                // (uniform scaling, no distortion). Same pattern as the runtime
                // test apps (test_apps/cube_handle_d3d11_win/main.cpp ~L800).
                // Prefer layerFracH = 1.0 (full window height, keeps the info
                // panel anchored to the window bottom); on extremely tall tiles
                // where that would push layerFracW past 1.0, clamp width and
                // shrink height instead.
                const float hudAR = (hudHeight > 0)
                    ? (float)hudWidth / (float)hudHeight : 1.0f;
                const float windowAR = (windowW > 0 && windowH > 0)
                    ? (float)windowW / (float)windowH : 1.0f;
                float layerFracH = 1.0f;
                float layerFracW = hudAR / windowAR;
                if (layerFracW > 1.0f) {
                    layerFracW = 1.0f;
                    layerFracH = windowAR / hudAR;
                }

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};

                        // Clean +Y-up world camera pose (no Y negation — the ModelRenderer
                        // now flips Vulkan-Y via a negative viewport, not a view/world
                        // reflection; see model_renderer.cpp).
                        XrPosef cameraPose;
                        {
                            XMVECTOR camOri = XMQuaternionRotationRollPitchYaw(
                                inputSnapshot.pitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 cq;
                            XMStoreFloat4(&cq, camOri);
                            cameraPose.orientation = {cq.x, cq.y, cq.z, cq.w};
                        }
                        cameraPose.position = {inputSnapshot.cameraPosX,
                            inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                        const float rigVH = inputSnapshot.viewParams.virtualDisplayHeight
                            / inputSnapshot.viewParams.scaleFactor;

                        // XR_DXR_view_rig (#396 W7): chain the display rig so the runtime
                        // owns the off-axis Kooima + window resolve, returning render-ready
                        // XrView{pose, fov}. The raw channel carries display-space eyes for HUD.
                        const bool useRig =
                            g_hasViewRigExt && xr->displayWidthM > 0 && xr->displayHeightM > 0;
                        XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
                        XrViewDisplayRawDXR viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
                        if (useRig) {
                            displayRig.pose = cameraPose;
                            displayRig.virtualDisplayHeight = rigVH;
                            displayRig.ipdFactor = inputSnapshot.viewParams.ipdFactor;
                            displayRig.parallaxFactor = inputSnapshot.viewParams.parallaxFactor;
                            displayRig.perspectiveFactor = inputSnapshot.viewParams.perspectiveFactor;
                            locateInfo.next = &displayRig;
                            viewState.next = &viewRigRaw;
                        }

                        // Zones-by-default (#63): lazy one-time caps probe, then a
                        // zone-scoped locate — the full-window zone with the rig
                        // chained THROUGH it. Requires the rig (a zone locate
                        // without a rig descriptor has no framing to scope), so
                        // gate on useRig; NULL chain = legacy locate unchanged.
                        {
                            static bool s_zoneTried = false;
                            if (!s_zoneTried && g_hasDisplayZonesExt && useRig) {
                                s_zoneTried = true;
                                dxr::FullWindowZoneInit(g_fwZone, xr->instance, xr->session);
                            }
                            if (useRig) {
                                const XrDisplayZoneDXR* zc = dxr::FullWindowZoneLocateChain(
                                    g_fwZone, windowW, windowH, &displayRig);
                                if (zc != nullptr) locateInfo.next = zc;
                            }
                        }

                        // Over-allocate to the runtime's max possible view_count (sim_display
                        // reports 4 for Quad mode; LeiaSR reports 2). Hardcoding 2 here used
                        // to fail with XR_ERROR_SIZE_INSUFFICIENT under sim_display.
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        // HUD eye readout. Under the rig, rawViews[] carries render-ready
                        // WORLD eyes, so the display-space eyes come from the raw channel
                        // (XrViewDisplayRawDXR); without the rig, the fill from the common
                        // LocateViews call above stands.
                        // The same raw channel carries the canvas the runtime
                        // RESOLVED for this locate — the shell's 3D window tile
                        // under a workspace, this window's client rect
                        // standalone. That, not our own (hidden, creation-size)
                        // window, is the viewport the auto-fit must frame
                        // against; RefitForViewport picks it up next tick.
                        if (useRig) {
                            g_autoFitCanvas.PublishFromRaw(
                                viewRigRaw.canvasSizeMeters.width,
                                viewRigRaw.canvasSizeMeters.height,
                                viewRigRaw.canvasRectPx.extent.width,
                                viewRigRaw.canvasRectPx.extent.height);
                        }

                        if (useRig && viewRigRaw.eyeCountOutput > 0) {
                            for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                                xr->eyePositions[v][0] = viewRigRaw.rawEyes[v].x;
                                xr->eyePositions[v][1] = viewRigRaw.rawEyes[v].y;
                                xr->eyePositions[v][2] = viewRigRaw.rawEyes[v].z;
                            }
                        }

                        bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[xr->currentModeIndex]);

                        // View count for the active rendering mode (1=mono, 2=stereo, 4=quad).
                        // Sized off the runtime's per-mode advertisement so the eye-loop and
                        // per-view buffers (rawEyes / stereoViews / viewMat / projectionViews)
                        // all line up with what xrEndFrame expects.
                        uint32_t activeViewCount = (xr->renderingModeCount > 0)
                            ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2u;
                        if (activeViewCount == 0) activeViewCount = 1u;
                        if (activeViewCount > 8) activeViewCount = 8u;
                        const int eyeCount = monoMode ? 1 : (int)activeViewCount;

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (cols × renderW, rows × renderH) are what gets written to
                        // the swapchain and snapshotted by the 'I' key. Swapchain
                        // creation already sized for the largest atlas, so no clamp.
                        // Falls back to the global recommendedViewScale (and 1.0 for
                        // mono) if the runtime didn't advertise per-mode info.
                        float scaleX, scaleY;
                        uint32_t cols, rows;
                        if (xr->renderingModeCount > 0) {
                            uint32_t mode = xr->currentModeIndex;
                            scaleX = xr->renderingModeScaleX[mode];
                            scaleY = xr->renderingModeScaleY[mode];
                            cols   = xr->renderingModeTileColumns[mode] ? xr->renderingModeTileColumns[mode] : 1u;
                            rows   = xr->renderingModeTileRows[mode]    ? xr->renderingModeTileRows[mode]    : 1u;
                        } else if (monoMode) {
                            scaleX = 1.0f; scaleY = 1.0f; cols = 1u; rows = 1u;
                        } else {
                            scaleX = xr->recommendedViewScaleX;
                            scaleY = xr->recommendedViewScaleY;
                            cols = 2u; rows = 1u;  // legacy SBS default
                        }
                        uint32_t renderW = (uint32_t)((double)windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;

                        // --- Consume the runtime's render-ready XrView{pose, fov} (#396 W7) ---
                        // The runtime owns the off-axis Kooima (window resolve included —
                        // it tracks resize via GetClientRect runtime-side); the app keeps
                        // only the clip policy (fov is clip-independent). near = ez - vH,
                        // far = ez + 1000*vH (opaque recede band; transparent mode's
                        // foreground-only look is the clipFar shader cull below, not a
                        // projection clamp), ez = RigLocalEyeZ (== the display-space eye Z
                        // display3d resolved). The view matrix is the plain clean-frame
                        // mat4_view_from_xr_pose — ModelRenderer owns the Vulkan Y-down
                        // flip via a negative viewport. GL projection → [0,1] depth remap
                        // kept (mesh uses the depth buffer).
                        Display3DView stereoViews[8];
                        bool useAppProjection = useRig;
                        if (useRig) {
                            // Mono: collapse the active views to their centroid (pose + fov).
                            // Clamp to the count the runtime actually wrote (macOS clamps
                            // modeViewCount to runtimeViewCount the same way).
                            uint32_t monoN = activeViewCount > viewCount ? viewCount : activeViewCount;
                            XrView srcViews[8];
                            if (monoMode && monoN >= 1) {
                                XrView cv = rawViews[0];
                                XrVector3f c = {0, 0, 0};
                                XrFovf f = {0, 0, 0, 0};
                                for (uint32_t v = 0; v < monoN; v++) {
                                    c.x += rawViews[v].pose.position.x;
                                    c.y += rawViews[v].pose.position.y;
                                    c.z += rawViews[v].pose.position.z;
                                    f.angleLeft  += rawViews[v].fov.angleLeft;
                                    f.angleRight += rawViews[v].fov.angleRight;
                                    f.angleUp    += rawViews[v].fov.angleUp;
                                    f.angleDown  += rawViews[v].fov.angleDown;
                                }
                                float inv = 1.0f / (float)monoN;
                                cv.pose.position = {c.x * inv, c.y * inv, c.z * inv};
                                cv.fov = {f.angleLeft * inv, f.angleRight * inv,
                                          f.angleUp * inv, f.angleDown * inv};
                                srcViews[0] = cv;
                            } else {
                                for (int e = 0; e < eyeCount; e++)
                                    srcViews[e] = rawViews[e < (int)viewCount ? e : 0];
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                const XrView& sv = srcViews[eye];
                                float ez = RigLocalEyeZ(cameraPose, sv.pose.position);
                                float near_z = (ez - rigVH > 1.0e-4f) ? (ez - rigVH) : 1.0e-4f;
                                float far_z  = ez + 1000.0f * rigVH;
                                mat4_view_from_xr_pose(stereoViews[eye].view_matrix, sv.pose);
                                mat4_from_xr_fov(stereoViews[eye].projection_matrix, sv.fov, near_z, far_z);
                                // GL ([-1,1] clip-z) → Vulkan [0,1] depth for the mesh's depth buffer.
                                convert_projection_gl_to_zero_to_one(stereoViews[eye].projection_matrix);
                                stereoViews[eye].fov = sv.fov;
                                stereoViews[eye].eye_world = sv.pose.position;
                                stereoViews[eye].orientation = sv.pose.orientation;
                                stereoViews[eye].eye_display = {0.0f, 0.0f, ez};
                                stereoViews[eye].near_z = near_z;
                                stereoViews[eye].far_z = far_z;
                            }
                        }

                        // Double-click focus: center-eye ray through mouse, pick nearest
                        // surface, smoothly re-pose the virtual display to face back
                        // along the ray.
                        if (inputSnapshot.teleportRequested && useRig) {
                            float ndcX = 2.0f * inputSnapshot.teleportMouseX / (float)windowW - 1.0f;
                            float ndcY = -(2.0f * inputSnapshot.teleportMouseY / (float)windowH - 1.0f);

                            // Center-eye pick view reconstructed from the render-ready rig
                            // views: average the active eye poses + fovs into a symmetric
                            // center frustum in the clean +Y-up world frame the model lives
                            // in (no Y flip — the pick ray must match the world, not the
                            // Vulkan raster). GL projection (no [0,1] remap) since the ray
                            // is a full line.
                            XrVector3f cpos = {0, 0, 0};
                            XrFovf cfov = {0, 0, 0, 0};
                            for (int e = 0; e < eyeCount; e++) {
                                cpos.x += stereoViews[e].eye_world.x;
                                cpos.y += stereoViews[e].eye_world.y;
                                cpos.z += stereoViews[e].eye_world.z;
                                cfov.angleLeft  += stereoViews[e].fov.angleLeft;
                                cfov.angleRight += stereoViews[e].fov.angleRight;
                                cfov.angleUp    += stereoViews[e].fov.angleUp;
                                cfov.angleDown  += stereoViews[e].fov.angleDown;
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
                            std::lock_guard<std::mutex> sceneLock(g_sceneMutex);
                            if (g_modelRenderer.pickSurface(rayOrigin, rayDir, hitPos)) {
                                // Both endpoints stored in the clean +Y-up WORLD frame
                                // (the same frame as inputSnapshot.cameraPosX/Y/Z and the
                                // model) so the slerp interpolates consistently. (#396 W7:
                                // the camera pose is no longer Y-negated/pitch-flipped for
                                // rendering — the renderer flips at the raster stage — so
                                // cameraPose.orientation IS the world orientation.)
                                XrPosef fromWorld;
                                fromWorld.orientation = cameraPose.orientation;
                                fromWorld.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                                XrPosef target;
                                target.position = {hitPos[0], hitPos[1], hitPos[2]};
                                target.orientation = cameraPose.orientation;  // preserve current orientation — translate-only
                                std::lock_guard<std::mutex> inputLock(g_inputMutex);
                                g_inputState.transitionFrom = fromWorld;
                                g_inputState.transitionTo = target;
                                g_inputState.transitionT = 0.0f;
                                g_inputState.transitioning = true;
                                LOG_INFO("Focus on splat (%.3f, %.3f, %.3f)",
                                    hitPos[0], hitPos[1], hitPos[2]);
                            }
                        }

                        rendered = true;
                        // eyeCount already computed above from active mode's view count

                        // Mono center eye
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                            monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                            monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;

                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        // Foreground-only clip: in transparent mode, cull splats
                        // behind the virtual display plane so only popping-out
                        // content shows. Suppressed under the shell's external
                        // multi-compositor (non-controller workspace session,
                        // where the per-app transparent bridge is bypassed) —
                        // signalled by renderingModeIsRequestable being false.
                        bool foregroundClip = g_transparentBg.load() && IsStandaloneSession(xr);

                        // Build per-eye view/projection matrices (column-major float[16]).
                        // Sized to the runtime's max view count so Quad mode (4 views) fits.
                        float viewMat[8][16], projMat[8][16];
                        float clipFar[8] = {0};  // per-eye view-space far cull (0 = off)
                        for (int eye = 0; eye < eyeCount; eye++) {
                            if (useAppProjection) {
                                int srcEye = monoMode ? 0 : eye;
                                memcpy(viewMat[eye], stereoViews[srcEye].view_matrix, sizeof(float) * 16);
                                memcpy(projMat[eye], stereoViews[srcEye].projection_matrix, sizeof(float) * 16);
                                // eye_display.z = eye->display-plane forward distance,
                                // same world units as the shader's p_view.z.
                                if (foregroundClip) {
                                    float cf = stereoViews[srcEye].eye_display.z;
                                    clipFar[eye] = (cf > 0.2f) ? cf : 0.0f;  // never cull at/behind near
                                }
                            } else {
                                // Fallback: use DirectXMath mono matrices, store as column-major
                                XMMATRIX v = monoMode ? monoViewMatrix :
                                    XMMatrixLookAtRH(XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position),
                                        XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position) + XMVectorSet(0,0,-1,0),
                                        XMVectorSet(0,1,0,0));
                                XMMATRIX p = monoMode ? monoProjMatrix : xr->projMatrices[0];
                                // XMMatrix is row-major; transpose to get column-major for shader
                                XMMATRIX vT = XMMatrixTranspose(v);
                                XMMATRIX pT = XMMatrixTranspose(p);
                                XMStoreFloat4x4((XMFLOAT4X4*)viewMat[eye], vT);
                                XMStoreFloat4x4((XMFLOAT4X4*)projMat[eye], pT);
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            VkFormat colorFormat = (VkFormat)xr->swapchain.format;

                            bool hasGsScene;
                            {
                                std::lock_guard<std::mutex> lock(g_sceneMutex);
                                hasGsScene = g_modelRenderer.hasModel();
                            }

                            if (hasGsScene) {
                                const auto sceneT0 = std::chrono::high_resolution_clock::now();
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    // Row-major eye placement in the atlas; for 2×1 SBS
                                    // this is (0, renderW) at row 0; for mono (cols=1)
                                    // it collapses to (0, 0).
                                    uint32_t col = (uint32_t)eye % cols;
                                    uint32_t row = (uint32_t)eye / cols;
                                    uint32_t vpX = col * renderW;
                                    uint32_t vpY = row * renderH;
                                    g_modelRenderer.renderEye(
                                        (*swapchainVkImages)[imageIndex], colorFormat,
                                        xr->swapchain.width, xr->swapchain.height,
                                        vpX, vpY, renderW, renderH,
                                        viewMat[eye], projMat[eye],
                                        g_transparentBg.load(), clipFar[eye]);
                                }
                                g_sceneMsAccum.store(
                                    g_sceneMsAccum.load(std::memory_order_relaxed) +
                                    std::chrono::duration<double, std::milli>(
                                        std::chrono::high_resolution_clock::now() - sceneT0).count(),
                                    std::memory_order_relaxed);
                                g_sceneMsFrames.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, renderCmdPool,
                                    (*swapchainVkImages)[imageIndex], xr->swapchain.width, xr->swapchain.height);
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasDXR
                            // (XR_DXR_atlas_capture, W6 of #396). The runtime owns
                            // the readback — no app-side staging texture. Works for
                            // any multi-view layout the runtime advertises; skipped
                            // for mono (1×1). Filename auto-increments. The prefix
                            // has no ".png"; the runtime appends "_atlas.png".
                            if (g_captureAtlasRequested.exchange(false)) {
                                if (!hasGsScene) {
                                    LOG_WARN("Capture skipped: no model loaded");
                                    // Without this the I key is silently inert —
                                    // no flash, no file, no explanation.
                                    ToastF("Capture skipped - no model loaded");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                    ToastF("Capture skipped - mono (1x1) layout");
                                } else if (xr->pfnCaptureAtlasEXT &&
                                           xr->session != XR_NULL_HANDLE) {
                                    std::string sceneName;
                                    {
                                        std::lock_guard<std::mutex> lock(g_sceneMutex);
                                        sceneName = g_loadedFileName;
                                    }
                                    // Strip extension from model filename
                                    // (e.g. "sample.glb" → "sample").
                                    auto dot = sceneName.find_last_of('.');
                                    std::string stem = (dot == std::string::npos)
                                        ? sceneName : sceneName.substr(0, dot);
                                    if (stem.empty()) stem = "scene";
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoDXR info = {XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR;
                                    strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);
                                    XrResult cr = xr->pfnCaptureAtlasEXT(xr->session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::PostFlashRequest(hwnd);
                                        // Complements the flash: the flash says
                                        // "something happened", the toast says WHAT
                                        // and, more usefully, WHERE it landed.
                                        ToastF("Captured  %s_atlas.png", prefix.c_str());
                                    } else {
                                        LOG_WARN("xrCaptureAtlasDXR failed: 0x%x", (unsigned)cr);
                                        ToastF("Capture failed (0x%x)", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_DXR_atlas_capture not available");
                                    ToastF("Capture unavailable - no atlas-capture ext");
                                }
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t col = (uint32_t)eye % cols;
                                uint32_t row = (uint32_t)eye / cols;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(col * renderW), (int32_t)(row * renderH)};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                if (useRig) {
                                    // Render-ready rig views: submit the per-eye world pose
                                    // (mono = the collapsed centroid) + the rig fov.
                                    int srcEye = monoMode ? 0 : eye;
                                    projectionViews[eye].pose.position = stereoViews[srcEye].eye_world;
                                    projectionViews[eye].pose.orientation = cameraPose.orientation;
                                    projectionViews[eye].fov = stereoViews[srcEye].fov;
                                } else {
                                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                                    projectionViews[eye].fov = monoMode ? rawViews[0].fov : rawViews[eye].fov;
                                }
                            }

                            // #833 punch-through: while Ctrl+T transparent AND
                            // borderless, shape the window from view 0's alpha
                            // (fence-pipelined, ~2-frame lag — no waits). The
                            // button-bar band stays in-region. Leaving the mode
                            // un-shapes; the framed style returns via
                            // kBorderlessMsg.
                            if (g_transparentBg.load() && g_borderless.load()) {
                                static bool s_punchInit = false;
                                if (!s_punchInit) {
                                    s_punchInit = g_punch.init(vkDevice, physDevice, queueFamilyIndex);
                                }
                                if (s_punchInit) {
                                    // Chrome is hidden in transparent mode (see
                                    // the bar block) — only the live toast band
                                    // stays in-region.
                                    RECT chrome[1];
                                    uint32_t nChrome = 0;
                                    {
                                        // Previous frame's toast band — the region
                                        // lags a couple frames anyway.
                                        std::lock_guard<std::mutex> tb(g_toastBandMtx);
                                        if (g_toastBandLive) chrome[nChrome++] = g_toastBand;
                                    }
                                    // Union the LAST view's tile too — a view-0-only
                                    // region clips the other views' parallax edges
                                    // once content moves off ZDP.
                                    const uint32_t lastV = (uint32_t)(eyeCount - 1);
                                    g_punch.update(graphicsQueue, (*swapchainVkImages)[imageIndex],
                                                   renderW, renderH, hwnd, windowW, windowH, chrome, nChrome,
                                                   (lastV % cols) * renderW, (lastV / cols) * renderH,
                                                   eyeCount > 1);
                                }
                            } else if (g_punch.shaped()) {
                                g_punch.disable(hwnd);
                            }
                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render the HUD info-panel window-space layer. Body-only
                        // now (chrome buttons moved to the top-bar layer below).
                        // The TAB toggle hides the body via the `drawBody` flag;
                        // the layer footprint stays the aspect-locked left strip.
                        // Only render/acquire the HUD swapchain when the panel is
                        // visible — when hidden the layer is dropped entirely (true
                        // toggle), so we must NOT acquire its image this frame.
                        if (rendered && hud && xr->hasHudSwapchain && hudSwapchainImages &&
                            inputSnapshot.hudVisible) {
                            {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_DXR_win32_window_binding: ACTIVE (Vulkan + glTF)" :
                                    L"XR_DXR_win32_window_binding: NOT AVAILABLE";

                                // Scene info
                                std::wstring sceneText = L"\n--- Model ---";
                                {
                                    std::lock_guard<std::mutex> lock(g_sceneMutex);
                                    if (g_modelRenderer.hasModel()) {
                                        std::wstring fname(g_loadedFileName.begin(), g_loadedFileName.end());
                                        sceneText += L"\nLoaded: " + fname;
                                    } else {
                                        sceneText += L"\nNo scene loaded (Ctrl+O or click Load)";
                                    }
                                }
                                modeText += sceneText;

                                // Per-view extent for HUD display — same formula as the
                                // render path (window × view_scale of the current mode).
                                float dispScaleX, dispScaleY;
                                if (xr->renderingModeCount > 0) {
                                    uint32_t mode = xr->currentModeIndex;
                                    dispScaleX = xr->renderingModeScaleX[mode];
                                    dispScaleY = xr->renderingModeScaleY[mode];
                                } else {
                                    dispScaleX = xr->recommendedViewScaleX;
                                    dispScaleY = xr->recommendedViewScaleY;
                                }
                                uint32_t dispRenderW = (uint32_t)((double)windowW * dispScaleX);
                                uint32_t dispRenderH = (uint32_t)((double)windowH * dispScaleY);
                                if (dispRenderW == 0) dispRenderW = 1;
                                if (dispRenderH == 0) dispRenderH = 1;
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(xr->currentModeIndex, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeNames[xr->currentModeIndex] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[xr->currentModeIndex] : true,
                                    xr->renderingModeCount > 0 ? xr->renderingModeIsRequestable[xr->currentModeIndex] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                float fwdY =  sinf(inputSnapshot.pitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                {
                                    char pinBuf[24];
                                    g_recenter.hudLabel(pinBuf, sizeof(pinBuf));
                                    wchar_t pinW[32];
                                    swprintf(pinW, 32, L"\nRecenter: %hs", pinBuf);
                                    cameraText += pinW;
                                }
                                float hudM2v = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    inputSnapshot.viewParams.perspectiveFactor, inputSnapshot.viewParams.scaleFactor);
                                {
                                    wchar_t vhBuf[128];  // fits the longest "held:" label
                                    int depthPct = (int)(inputSnapshot.viewParams.ipdFactor * 100.0f + 0.5f);
                                    // orbitEnabledByUser, not the snapshot flag —
                                    // the gate clears the latter for the frame, and
                                    // reporting that as "OFF" would blame the M key
                                    // for a hold the content asked for.
                                    const wchar_t* orbitLbl =
                                        !orbitEnabledByUser ? L"OFF"
                                        : orbitSuppressed
                                            ? (g_hasAnimations.load() ? L"ON (held: clip playing)"
                                                                      : L"ON (held: transparent)")
                                            : (inputSnapshot.animationActive ? L"ON (running)"
                                                                             : L"ON (idle countdown)");
                                    swprintf(vhBuf, 128, L"\nvHeight: %.3f  m2v: %.3f\nDepth/IPD: %d%%  Auto-Orbit: %s",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v, depthPct, orbitLbl);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = L"[WASDEQ] Move | [LMB-drag] Rotate | [Scroll] Zoom\n"
                                    L"[DblClick] Focus | [-/=] Depth | [Space] Reset | [N] Clip | [K] Play/Pause\n"
                                    L"[M] Auto-Orbit | [V] Mode | [Ctrl+O] Load | [Tab] HUD | [ESC] Quit\n"
                                    L"[P then X/Y/Z] Pin recenter axis";

                                // Chrome buttons no longer live here — they are a
                                // separate full-width top-bar window-space layer
                                // (see the button-bar block below). This layer is
                                // the info panel only, toggled by Tab via drawBody.
                                //
                                // #837 overlay kit: rasterize + upload only when the
                                // content changed. The panel is all live readouts, so
                                // the hash rides a 250 ms bucket (4 Hz refresh) plus
                                // the stable strings; the upload submits with a fence
                                // and never waits (the old per-frame vkQueueWaitIdle
                                // drained the frame's queued GPU work on the iGPU).
                                static dxr::CachedLayerUploader s_hudUp;
                                static bool s_hudUpInit = false;
                                static bool s_hudUploadedOnce = false;
                                if (!s_hudUpInit) {
                                    s_hudUpInit = s_hudUp.init(vkDevice, physDevice,
                                        queueFamilyIndex, hudWidth, hudHeight);
                                }
                                uint64_t hh = dxr::HashBytes(sessionText.data(),
                                    sessionText.size() * sizeof(wchar_t));
                                hh = dxr::HashBytes(modeText.data(), modeText.size() * sizeof(wchar_t), hh);
                                hh = dxr::HashBytes(dispText.data(), dispText.size() * sizeof(wchar_t), hh);
                                const uint64_t bucket = GetTickCount64() / 250;
                                hh = dxr::HashBytes(&bucket, sizeof(bucket), hh);

                                if (s_hudUpInit && s_hudUp.needsUpload(hh)) {
                                    uint32_t hudImageIndex;
                                    if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                        uint32_t srcRowPitch = 0;
                                        const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                            cameraText, stereoText, helpText, {},
                                            /*drawBody=*/true,
                                            /*bodyAtBottom=*/true);
                                        if (pixels) {
                                            VkImage hudImg = (*hudSwapchainImages)[hudImageIndex].image;
                                            if (s_hudUp.upload(graphicsQueue, hudImg, pixels,
                                                    srcRowPitch, hudWidth * 4, hudHeight, hh)) {
                                                s_hudUploadedOnce = true;
                                            }
                                            UnmapHud(*hud);
                                        }
                                        ReleaseHudSwapchainImage(*xr);
                                    }
                                }
                                hudSubmitted = s_hudUploadedOnce;
                            }
                        }

                    }
                }

                // ── Top button bar: ONE full-width window-space layer holding all
                //    chrome buttons — Open + Mode packed left, Animation pinned
                //    right, transparent center. Always submitted (decoupled from
                //    the Tab-toggled HUD panel); the Animation pill is only added
                //    when the model has clips. Reuses the window-space-layer
                //    machinery (own swapchain / text renderer / staging) widened
                //    to a bar — see runtime issue #389. ──
                XrCompositionLayerWindowSpaceDXR barLayer = {};
                bool barLayerReady = false;
                // #833: chrome is hidden in transparent/punch-through mode by
                // design (product call 2026-08-03) — the model floats clean
                // over the live desktop; Ctrl+T back brings the buttons back.
                // (Also sidesteps leia-plugin's WSUI-content-under-compose
                // bug, filed separately.)
                if (g_animBtnReady && g_hasAnimBtnSwapchain && !g_transparentBg.load()) {
                    const float mxf = (g_windowWidth > 0)
                        ? (float)inputSnapshot.mouseX / (float)g_windowWidth : 0.0f;
                    const float myf = (g_windowHeight > 0)
                        ? (float)inputSnapshot.mouseY / (float)g_windowHeight : 0.0f;
                    const float barY = BTN_BAR_Y_FRACTION;
                    const float barH = BtnBarHeightFraction(windowW, windowH);
                    // Bar layer spans the full window width, so a button at
                    // window-x-fraction xf maps straight onto bar-texture-x. Pills
                    // fill ~70% of the bar height, vertically centered.
                    const float pillY = (float)BTN_BAR_TEX_H * 0.15f;
                    const float pillH = (float)BTN_BAR_TEX_H * 0.70f;
                    auto makeBtn = [&](float xf, float wf, const std::wstring& label) {
                        HudButton b;
                        b.label = label;
                        b.x = xf * (float)BTN_BAR_TEX_W;
                        b.y = pillY;
                        b.width = wf * (float)BTN_BAR_TEX_W;
                        b.height = pillH;
                        b.hovered = (mxf >= xf && mxf <= xf + wf &&
                                     myf >= barY && myf <= barY + barH);
                        return b;
                    };
                    std::vector<HudButton> barButtons;
                    barButtons.push_back(makeBtn(OPEN_BTN_X_FRACTION, OPEN_BTN_WIDTH_FRACTION, L"Open…"));
                    std::wstring modeLabel = L"Mode";
                    if (xr->renderingModeCount > 0 &&
                        xr->currentModeIndex < xr->renderingModeCount &&
                        xr->renderingModeNames[xr->currentModeIndex]) {
                        const char* nm = xr->renderingModeNames[xr->currentModeIndex];
                        modeLabel = L"Mode: " + std::wstring(nm, nm + strlen(nm));
                    }
                    // Surface workspace mode-lock so the user knows clicking Mode
                    // is a no-op in a locked workspace.
                    if (xr->renderingModeCount > 0 &&
                        xr->currentModeIndex < xr->renderingModeCount &&
                        !xr->renderingModeIsRequestable[xr->currentModeIndex]) {
                        modeLabel += L" [locked]";
                    }
                    barButtons.push_back(makeBtn(MODE_BTN_X_FRACTION, MODE_BTN_WIDTH_FRACTION, modeLabel));
                    if (g_hasAnimations.load()) {
                        std::wstring animLabel = L"Anim";
                        {
                            std::string clip; int ci, cn; float ct, cd; bool playing;
                            std::lock_guard<std::mutex> lk(g_sceneMutex);
                            if (g_modelRenderer.getPlaybackInfo(clip, ci, cn, ct, cd, playing))
                                animLabel = playing ? std::wstring(clip.begin(), clip.end()) : L"Paused";
                        }
                        barButtons.push_back(makeBtn(AnimBtnXFraction(), ANIM_BTN_WIDTH_FRACTION, animLabel));
                    }

                    // #837 overlay kit: the bar is static except for hover state
                    // and label changes (mode/anim) — rasterize + upload only when
                    // those change; the last-released image keeps serving the
                    // layer. Upload submits with a fence, never waits.
                    static dxr::CachedLayerUploader s_barUp;
                    static bool s_barUpInit = false;
                    static bool s_barUploadedOnce = false;
                    if (!s_barUpInit) {
                        s_barUpInit = s_barUp.init(vkDevice, physDevice, queueFamilyIndex,
                            BTN_BAR_TEX_W, BTN_BAR_TEX_H);
                    }
                    uint64_t bh = 1469598103934665603ull;
                    for (const HudButton& b : barButtons) {
                        bh = dxr::HashBytes(b.label.data(), b.label.size() * sizeof(wchar_t), bh);
                        bh = dxr::HashBytes(&b.hovered, sizeof(b.hovered), bh);
                    }

                    if (s_barUpInit && s_barUp.needsUpload(bh)) {
                        uint32_t pitch = 0;
                        const void* px = RenderHudAndMap(g_animBtnHud, &pitch,
                            L"", L"", L"", L"", L"", L"", L"", L"",
                            barButtons, /*drawBody=*/false, /*bodyAtBottom=*/true);
                        uint32_t idx = 0;
                        if (px && AcquireWindowSpaceImage(g_animBtnSwapchain, idx)) {
                            if (s_barUp.upload(graphicsQueue, g_animBtnSwapImages[idx].image, px,
                                    pitch, BTN_BAR_TEX_W * 4, BTN_BAR_TEX_H, bh)) {
                                s_barUploadedOnce = true;
                            } else {
                                static bool s_warned = false;
                                if (!s_warned) { s_warned = true; LOG_WARN("bar: kit upload FAILED"); }
                            }
                            UnmapHud(g_animBtnHud);
                            ReleaseWindowSpaceImage(g_animBtnSwapchain);
                        } else {
                            static bool s_warned2 = false;
                            if (!s_warned2) { s_warned2 = true; LOG_WARN("bar: px=%d acquire failed", px != nullptr); }
                            if (px) UnmapHud(g_animBtnHud);
                        }
                    }
                    {
                        static bool s_logged = false;
                        if (!s_logged) { s_logged = true; LOG_INFO("bar: kit init=%d uploadedOnce=%d", (int)s_barUpInit, (int)s_barUploadedOnce); }
                    }

                    if (s_barUploadedOnce) {
                        barLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
                        barLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        barLayer.subImage.swapchain = g_animBtnSwapchain.swapchain;
                        barLayer.subImage.imageRect.offset = {0, 0};
                        barLayer.subImage.imageRect.extent = {(int32_t)BTN_BAR_TEX_W, (int32_t)BTN_BAR_TEX_H};
                        barLayer.subImage.imageArrayIndex = 0;
                        barLayer.x = 0.0f;
                        barLayer.y = BTN_BAR_Y_FRACTION;
                        barLayer.width = 1.0f;
                        barLayer.height = barH;
                        barLayer.disparity = 0.0f;
                        barLayerReady = true;
                    }
                }

                // ── Toast layer ──────────────────────────────────────────────
                // Only built + submitted while a message is live; once it
                // expires the layer is simply absent from the submission.
                //
                // Two shapes, one uploaded image (#63): on a zones frame the
                // chip is a LOCAL2D layer — composited post-weave, so it stays
                // intact over the transparency silhouette in Ctrl+T mode. On
                // the legacy fallback it stays a window-space layer (stamped
                // into the atlas; silhouette-clipped in transparent mode — the
                // pre-#63 behaviour, kept for old runtimes).
                XrCompositionLayerWindowSpaceDXR toastLayer = {};
                XrCompositionLayerLocal2DDXR toastLayer2D = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
                bool toastLayerReady = false;       // window-space shape valid
                bool toastLayer2DReady = false;     // Local2D shape valid
                {
                    std::wstring toastText;
                    float toastAlpha = 1.0f;
                    {
                        std::lock_guard<std::mutex> tb(g_toastBandMtx);
                        g_toastBandLive = false; // republished below while live
                    }
                    if (g_toastReady && g_hasToastSwapchain &&
                        g_toast.Snapshot(toastText, toastAlpha)) {
                        // #837 overlay kit: re-rasterize only when the text or the
                        // (quantized, 1/32 steps) fade alpha changes — the fade
                        // re-uploads a handful of times instead of every frame,
                        // and the steady chip costs nothing. Fence, no wait.
                        static dxr::CachedLayerUploader s_toastUp;
                        static bool s_toastUpInit = false;
                        static bool s_toastUploadedOnce = false;
                        if (!s_toastUpInit) {
                            s_toastUpInit = s_toastUp.init(vkDevice, physDevice, queueFamilyIndex,
                                TOAST_TEX_W, TOAST_TEX_H);
                        }
                        const uint32_t alphaQ = (uint32_t)(toastAlpha * 32.0f + 0.5f);
                        uint64_t th = dxr::HashBytes(toastText.data(),
                            toastText.size() * sizeof(wchar_t));
                        th = dxr::HashBytes(&alphaQ, sizeof(alphaQ), th);

                        if (s_toastUpInit && s_toastUp.needsUpload(th)) {
                            uint32_t pitch = 0;
                            const void* px = RenderToastStandalone(g_toastHud, &pitch,
                                toastText, toastAlpha);
                            uint32_t idx = 0;
                            if (px && AcquireWindowSpaceImage(g_toastSwapchain, idx)) {
                                if (s_toastUp.upload(graphicsQueue, g_toastSwapImages[idx].image, px,
                                        pitch, TOAST_TEX_W * 4, TOAST_TEX_H, th)) {
                                    s_toastUploadedOnce = true;
                                }
                                UnmapHud(g_toastHud);
                                ReleaseWindowSpaceImage(g_toastSwapchain);
                            } else if (px) {
                                UnmapHud(g_toastHud);
                            }
                        }

                        if (s_toastUploadedOnce) {
                            const dxr::ToastLayerRect tr = dxr::ComputeToastLayerRect(
                                windowW, windowH, (float)TOAST_TEX_W / (float)TOAST_TEX_H,
                                TOAST_SIZE_FRACTION, TOAST_Y_FRACTION);
                            // #833: publish the chip's client-px band so the
                            // punch-through region keeps it visible — the shape
                            // clips ANY pixel outside the region, including
                            // post-weave Local2D layers (this is not the #63
                            // WSUI-vs-Local2D clipping, which stays fixed).
                            {
                                std::lock_guard<std::mutex> tb(g_toastBandMtx);
                                g_toastBand.left = (LONG)(tr.x * (float)windowW);
                                g_toastBand.top = (LONG)(tr.y * (float)windowH);
                                g_toastBand.right = (LONG)((tr.x + tr.width) * (float)windowW + 1.0f);
                                g_toastBand.bottom = (LONG)((tr.y + tr.height) * (float)windowH + 1.0f);
                                g_toastBandLive = true;
                            }
                            if (g_fwZone.available) {
                                // Local2D takes a PIXEL rect in the window.
                                int32_t rx = (int32_t)(tr.x * (float)windowW + 0.5f);
                                int32_t ry = (int32_t)(tr.y * (float)windowH + 0.5f);
                                int32_t rw = (int32_t)(tr.width  * (float)windowW + 0.5f);
                                int32_t rh = (int32_t)(tr.height * (float)windowH + 0.5f);
                                if (rw < 2) rw = 2;
                                if (rh < 2) rh = 2;
                                toastLayer2D.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                                toastLayer2D.subImage.swapchain = g_toastSwapchain.swapchain;
                                toastLayer2D.subImage.imageRect.offset = {0, 0};
                                toastLayer2D.subImage.imageRect.extent = {(int32_t)TOAST_TEX_W, (int32_t)TOAST_TEX_H};
                                toastLayer2D.subImage.imageArrayIndex = 0;
                                toastLayer2D.rect.offset = {rx, ry};
                                toastLayer2D.rect.extent = {rw, rh};
                                toastLayer2DReady = true;
                            } else {
                                toastLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
                                toastLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                                toastLayer.subImage.swapchain = g_toastSwapchain.swapchain;
                                toastLayer.subImage.imageRect.offset = {0, 0};
                                toastLayer.subImage.imageRect.extent = {(int32_t)TOAST_TEX_W, (int32_t)TOAST_TEX_H};
                                toastLayer.subImage.imageArrayIndex = 0;
                                toastLayer.x = tr.x;
                                toastLayer.y = tr.y;
                                toastLayer.width = tr.width;
                                toastLayer.height = tr.height;
                                toastLayer.disparity = 0.0f;   // screen depth — UI chrome sits at ZDP
                                toastLayerReady = true;
                            }
                        }
                    }
                }

                // Submit frame
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                if (submitViewCount == 0) submitViewCount = 1;
                if (submitViewCount > 8) submitViewCount = 8;  // matches projectionViews[8] sizing
                if (rendered) {
                    // Always go through the window-space-layers path so the top
                    // button bar (an extra layer) shows. The HUD info-panel layer
                    // is gated by `submitHud = hudSubmitted`: when the panel is
                    // toggled off it was never rendered/acquired this frame, so we
                    // drop it entirely (true toggle, not a transparent layer).
                    // SOURCE_ALPHA on the projection layer: displayxr::common
                    // defaults projectionLayerFlags to 0, so pass the bit
                    // explicitly (the vendored copy hardcoded it; required for
                    // the Ctrl+T transparent-background path).
                    // Extra window-space layers, submitted in order: the top
                    // button bar, then (legacy fallback only) the window-space
                    // toast on top of it.
                    XrCompositionLayerWindowSpaceDXR uiLayers[2] = {};
                    uint32_t uiLayerCount = 0;
                    if (barLayerReady) uiLayers[uiLayerCount++] = barLayer;
                    if (toastLayerReady) uiLayers[uiLayerCount++] = toastLayer;
                    // Zones frame (#63): the same XrDisplayZoneDXR instance the
                    // locate used chains on the projection layer, and the toast
                    // rides as a Local2D extra layer (composited post-weave, on
                    // top of everything). Both are NULL/empty on the legacy
                    // fallback, collapsing to the exact pre-#63 submission.
                    const XrCompositionLayerBaseHeader* extraLayers[1] = {};
                    uint32_t extraLayerCount = 0;
                    if (toastLayer2DReady)
                        extraLayers[extraLayerCount++] = (const XrCompositionLayerBaseHeader*)&toastLayer2D;
                    EndFrameWithWindowSpaceLayers(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, layerFracW, layerFracH, 0.0f, submitViewCount,
                        uiLayerCount ? uiLayers : nullptr, uiLayerCount,
                        0, 0, -1, -1, /*submitHud=*/hudSubmitted,
                        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                        dxr::FullWindowZoneSubmitChain(g_fwZone),
                        extraLayerCount ? extraLayers : nullptr, extraLayerCount);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    if (renderCmdPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vkDevice, renderCmdPool, nullptr);

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

// Global crash handler
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    const char* excName = "UNKNOWN";
    switch (exInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:      excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    excName = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         excName = "IN_PAGE_ERROR"; break;
        case EXCEPTION_GUARD_PAGE:            excName = "GUARD_PAGE"; break;
    }
    LOG_ERROR("!!! UNHANDLED EXCEPTION: %s (0x%08X) at address 0x%p !!!",
        excName, exInfo->ExceptionRecord->ExceptionCode,
        exInfo->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    (void)lpCmdLine;  // superseded by ParseLaunchArgsFromCommandLine (GetCommandLineW)

    // The undock launch contract (displayxr-common): the legacy positional
    // model path, the --transparent/--rect/--src/--vh flags, and a
    // displayxr-view: protocol URL all land in ONE validated struct, under one
    // security policy shared with the splat viewer. Parsed from
    // GetCommandLineW rather than WinMain's ANSI lpCmdLine, so non-ASCII paths
    // survive. Logging is not up yet — errors/warnings are reported below.
    g_launch = dxr::ParseLaunchArgsFromCommandLine();
    // The model loaders fopen a narrow path, so convert once here (same
    // lossiness the old lpCmdLine trim always had — no worse).
    std::string cliModelPath = dxr::NarrowPathForFopen(g_launch.positionalPath);

    SetUnhandledExceptionFilter(CrashHandler);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== DisplayXR 3D Model Viewer (Vulkan) ===");

    // ── Launch contract: report, refuse, route, then configure ───────────
    for (const std::string& w : g_launch.warnings) LOG_WARN("launch: %s", w.c_str());
    // An undocked viewer must run its OWN in-process compositor. A protocol handler
    // inherits the browser's environment (XRT_FORCE_MODE=ipc), which would make this
    // process an IPC client that is not the panel owner: 2D whenever the browser holds
    // the panel, no drag phase-snap. Must run before xrCreateInstance loads the runtime.
    // Inherited IPC routing cannot be overridden in place (the runtime DLL's dynamic CRT
    // snapshots the environment at process start), so re-launch once with a scrubbed
    // block and let the child run. The in-place override below stays as the fallback.
    if (dxr::ReexecWithCleanRuntimeEnvIfNeeded(g_launch)) {
        LOG_WARN("launch: inherited IPC routing -> re-launched with a clean environment; this instance exits");
        ShutdownLogging();
        return 0;
    }
    if (dxr::ForceInProcessRuntimeForUndock(g_launch))
        LOG_WARN("launch: inherited IPC routing overridden -> XRT_FORCE_MODE=native (undock is standalone)");
    if (!g_launch.ok()) {
        for (const std::string& e : g_launch.errors) LOG_ERROR("launch: %s", e.c_str());
        // A protocol launch has no console and no parent reading stderr — the
        // refusal has to be visible, or a page whose URL was rejected sees
        // nothing at all and cannot tell "refused" from "crashed". A CLI
        // launch gets the log plus the exit code.
        if (g_launch.fromProtocol) {
            LaunchDialog(dxr::WideFromUtf8(g_launch.errors.front()).c_str(),
                         L"DisplayXR 3D Model Viewer — launch refused", MB_ICONERROR);
        }
        ShutdownLogging();
        return 2;
    }
    LOG_INFO("launch: transparent=%d rect=%s(%d,%d %dx%d) vh=%s(%.4f) src='%s' kind=%d "
             "type='%s' title='%s' protocol=%d max-bytes=%llu no-cache=%d dpr=%s(%.3f)",
             (int)g_launch.transparent, g_launch.hasRect ? "yes" : "no",
             g_launch.rectX, g_launch.rectY, g_launch.rectW, g_launch.rectH,
             g_launch.hasVh ? "yes" : "no", g_launch.vh,
             g_launch.src.c_str(), (int)g_launch.srcKind,
             g_launch.type.c_str(), g_launch.title.c_str(), (int)g_launch.fromProtocol,
             (unsigned long long)g_launch.maxBytes, (int)g_launch.noCache,
             g_launch.hasDpr ? "yes" : "no", g_launch.dpr);
    if (!g_launch.positionalPath.empty())
        LOG_INFO("launch: positional model path '%s'", cliModelPath.c_str());

    // Self-register the displayxr-view: scheme on EVERY launch, protocol or
    // not. HKCU, because the installers run elevated and an HKCU write there
    // lands in the elevating admin's hive. Idempotent, and it leaves a LIVE
    // sibling that owns the scheme alone (type forwarding covers that).
    {
        const bool reg = dxr::EnsureViewProtocolRegistered(dxr::ThisExePath(),
                                                           L"DisplayXR 3D Model Viewer");
        LOG_INFO("displayxr-view: protocol association %s", reg ? "OK" : "FAILED");
    }

    if (g_launch.fromProtocol) {
        // (a) Not our type — one scheme serves every viewer, so hand the
        //     IDENTICAL URL to the sibling that owns this asset kind. One
        //     scheme is what makes the browser ask the user only once.
        if (!g_launch.type.empty() && g_launch.type != "model") {
            const std::wstring sibling =
                dxr::FindSiblingViewer(L"GaussianSplat", L"gaussian_splatting_handle_vk_win.exe");
            if (sibling.empty()) {
                LOG_ERROR("launch: type='%s' is not ours and no sibling viewer is installed",
                          g_launch.type.c_str());
                LaunchDialog(
                            L"This link opens a Gaussian splat scene.\n\n"
                            L"Please install the DisplayXR Gaussian Splat viewer to open it.",
                            L"DisplayXR 3D Model Viewer", MB_ICONINFORMATION);
                ShutdownLogging();
                return 3;
            }
            const bool launched =
                dxr::LaunchViewerWithUrl(sibling, dxr::WideFromUtf8(g_launch.protocolUrl));
            LOG_INFO("launch: forwarded type='%s' to %ls (%s)", g_launch.type.c_str(),
                     sibling.c_str(), launched ? "OK" : "FAILED");
            if (!launched) {
                LaunchDialog(L"Could not start the DisplayXR Gaussian Splat viewer.",
                             L"DisplayXR 3D Model Viewer", MB_ICONERROR);
                ShutdownLogging();
                return 3;
            }
            ShutdownLogging();
            return 0;
        }

        // (b) Single instance: a second undock of the same viewer moves and
        //     re-points the window that is already floating instead of
        //     stacking another one over the desktop.
        g_singleInstanceMutex = dxr::AcquireSingleInstanceOrForward(
            L"Local\\DisplayXR.ModelViewer.Undock", WINDOW_CLASS, g_launch.protocolUrl);
        if (!g_singleInstanceMutex) {
            LOG_INFO("launch: handed the URL to the running instance — exiting");
            ShutdownLogging();
            return 0;
        }
    }

    // Transparent/borderless/topmost FROM FRAME 0 (transparency rule 2: never
    // change window styles while shaped). These are stores rather than atomic
    // initialisers because g_launch does not exist until this function runs;
    // nothing reads either flag before here. Ctrl+T remains the live toggle.
    if (g_launch.transparent) {
        g_transparentBg.store(true);
        g_borderless.store(true);
        LOG_INFO("launch: --transparent — creating the window borderless + topmost");
    }
    // --title is a SUFFIX. The base title is load-bearing for the repo's
    // AppActivate capture workflow, so it never gets replaced.
    g_windowTitle = WINDOW_TITLE;
    if (!g_launch.title.empty()) {
        g_windowTitle += L" - " + dxr::WideFromUtf8(g_launch.title);
    }
    if (g_launch.hasVh) {
        g_vhOverride.store(g_launch.vh, std::memory_order_relaxed);
        LOG_INFO("launch: --vh=%.4f m pins the virtual display height (auto-fit will not "
                 "re-derive it)", g_launch.vh);
    }
    // --src=<local path> is just another way to say the positional path.
    // --src=<url> suppresses the bundled auto-load: the download replaces it,
    // and auto-loading the helmet first would flash a model the caller never
    // asked for over the desktop.
    bool suppressAutoLoad = false;
    if (g_launch.srcKind == dxr::LaunchSrcKind::LocalPath) {
        cliModelPath = dxr::NarrowPathForFopen(g_launch.src);
        LOG_INFO("launch: --src local path '%s'", cliModelPath.c_str());
    } else if (g_launch.srcKind == dxr::LaunchSrcKind::Url) {
        suppressAutoLoad = true;
        cliModelPath.clear();
    }

    // Dynamic-recenter pins: default hard-pin X+Y+Z (modelviewer's historical
    // behaviour); DXR_RECENTER_PIN=XYZ|XY|Z|- overrides for headless testing.
    g_recenter.init(/*x=*/true, /*y=*/true, /*z=*/true);

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Initialize OpenXR BEFORE creating the window, so the window can be
    // created directly at the 3D panel's desktop position (INV-1.3,
    // XrDisplayDesktopPositionDXR, runtime#715). Nothing in the OpenXR /
    // Vulkan init below needs the HWND until CreateSession.
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Placement. Default: the panel's top-left at the historical 1280x720
    // (INV-1.3). With --rect: exactly the rect the caller measured, in physical
    // virtual-screen pixels — this exe is PerMonitorV2
    // (model_viewer_openxr_ext_vk.manifest), so no DPI scaling is applied to
    // what it was handed.
    int32_t winX = g_displayDesktopLeft;
    int32_t winY = g_displayDesktopTop;
    int32_t winW = (int32_t)g_windowWidth;
    int32_t winH = (int32_t)g_windowHeight;
    if (g_launch.hasRect) {
        winX = g_launch.rectX;
        winY = g_launch.rectY;
        winW = g_launch.rectW;
        winH = g_launch.rectH;
        LOG_INFO("Undock rect requested: (%d,%d %dx%d)%s%.3f", winX, winY, winW, winH,
                 g_launch.hasDpr ? "  dpr=" : "  dpr=n/a ", g_launch.hasDpr ? g_launch.dpr : 0.0f);
        ClampRectIntoPanel(winX, winY, winW, winH);
        g_windowWidth = (UINT)winW;
        g_windowHeight = (UINT)winH;
    }
    LOG_INFO("Undock rect final: (%d,%d %dx%d) panel-confirmed=%d panel-rect=(%d,%d %dx%d)",
             winX, winY, winW, winH, (int)g_displayPanelConfirmed,
             g_displayDesktopRect.offset.x, g_displayDesktopRect.offset.y,
             g_displayDesktopRect.extent.width, g_displayDesktopRect.extent.height);

    HWND hwnd = CreateAppWindow(hInstance, winW, winH, winX, winY, g_launch.transparent);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        CleanupOpenXR(xr);
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }
    // Publish for GetAutoFitViewportPx — the bundled scene auto-loads further
    // down in init, before the message loop, and its framing needs the real
    // client rect.
    g_appHwnd = hwnd;

    // Try to load sim_display_set_output_mode
    {
        HMODULE rtModule = GetModuleHandleA("openxr_displayxr.dll");
        if (!rtModule) rtModule = GetModuleHandleA("openxr_displayxr");
        if (rtModule) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)GetProcAddress(rtModule, "sim_display_set_output_mode");
        }
        LOG_INFO("sim_display output mode: %s", g_pfnSetOutputMode ? "available" : "not available");
    }

    // Get Vulkan graphics requirements
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family found");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(xr, physDevice, queueFamilyIndex, vkDevice, graphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // XR_DXR_mcp_tools (#47): declare appId "modelviewer" + register the base
    // agent tools now that the session exists. Runs on the main thread before
    // the render thread spawns and before the bundled scene auto-loads, so the
    // animation tools sync correctly. No-op when the extension / MCP capability
    // is unavailable.
    RegisterModelViewerMcpTools(xr);

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);

        // Extract VkImage handles for render thread access
    }
    std::vector<VkImage> swapchainVkImages(swapchainImages.size());
    for (uint32_t i = 0; i < (uint32_t)swapchainImages.size(); i++) {
        swapchainVkImages[i] = swapchainImages[i].image;
    }

    // Initialize model renderer with the OpenXR Vulkan device
    {
        uint32_t renderW = xr.swapchain.width;   // Full width — mono uses entire swapchain
        uint32_t renderH = xr.swapchain.height;
        if (!g_modelRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                               queueFamilyIndex, renderW, renderH)) {
            LOG_WARN("model renderer init failed - scene rendering will not be available");
        } else {
            // Environment first: setEnvironment rebakes the IBL cubes, so doing
            // it before the model means frame one is already lit correctly.
            if (DeterministicCaptureRequested()) {
                g_inputState.animateEnabled = false;
                LOG_INFO("Deterministic capture: auto-orbit disabled (DXR_MODELVIEWER_DETERMINISTIC)");
            }
            TryAutoLoadBundledEnvironment();
            if (suppressAutoLoad) {
                LOG_INFO("--src is a URL — skipping the bundled auto-load; the download "
                         "below supplies the scene");
            } else {
                TryAutoLoadBundledScene(cliModelPath);
            }
        }
    }

    // Initialize HUD renderer
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    uint32_t hudFontBaseHeight = (uint32_t)(xr.swapchain.height * HUD_FONT_BASE_FRACTION);
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight, hudFontBaseHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain
    std::vector<XrSwapchainImageVulkanKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u Vulkan images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create HUD staging buffer
    VkBuffer hudStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hudStagingMemory = VK_NULL_HANDLE;
    void* hudStagingMapped = nullptr;
    VkCommandPool hudCmdPool = VK_NULL_HANDLE;

    if (hudOk) {
        VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = (VkDeviceSize)hudWidth * hudHeight * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &hudStagingBuffer) != VK_SUCCESS) {
            LOG_WARN("Failed to create HUD staging buffer");
            hudOk = false;
        }

        if (hudOk) {
            VkMemoryRequirements memReqs;
            vkGetBufferMemoryRequirements(vkDevice, hudStagingBuffer, &memReqs);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags &
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }

            if (memTypeIndex == UINT32_MAX) {
                LOG_WARN("No suitable memory type for HUD staging buffer");
                hudOk = false;
            } else {
                VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = memTypeIndex;
                vkAllocateMemory(vkDevice, &allocInfo, nullptr, &hudStagingMemory);
                vkBindBufferMemory(vkDevice, hudStagingBuffer, hudStagingMemory, 0);
                vkMapMemory(vkDevice, hudStagingMemory, 0, bufInfo.size, 0, &hudStagingMapped);
            }
        }

        if (hudOk) {
            VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &hudCmdPool) != VK_SUCCESS) {
                LOG_WARN("Failed to create HUD command pool");
                hudOk = false;
            }
        }

        if (hudOk) {
            LOG_INFO("HUD Vulkan resources created (%ux%u)", hudWidth, hudHeight);
        }
    }

    // ── Top button-bar window-space layer resources ──────────────────────────
    // Own swapchain + text renderer + staging + cmd pool for the full-width top
    // button bar (Open + Mode + Animation in one layer). Reuses the
    // g_animBtnSwapchain / g_animBtn* slots (named before the buttons were
    // unified into a bar). Only when window-space layers are available.
    if (hudOk && xr.hasHudSwapchain) {
        if (InitializeHudRenderer(g_animBtnHud, BTN_BAR_TEX_W, BTN_BAR_TEX_H, BTN_BAR_FONT_BASE) &&
            CreateWindowSpaceSwapchain(xr, g_animBtnSwapchain, BTN_BAR_TEX_W, BTN_BAR_TEX_H)) {
            g_hasAnimBtnSwapchain = true;
            uint32_t c = g_animBtnSwapchain.imageCount;
            g_animBtnSwapImages.resize(c, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(g_animBtnSwapchain.swapchain, c, &c,
                (XrSwapchainImageBaseHeader*)g_animBtnSwapImages.data());

            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = (VkDeviceSize)BTN_BAR_TEX_W * BTN_BAR_TEX_H * 4;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            bool ok = vkCreateBuffer(vkDevice, &bi, nullptr, &g_animBtnStaging) == VK_SUCCESS;
            if (ok) {
                VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vkDevice, g_animBtnStaging, &mr);
                VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physDevice, &mp);
                uint32_t mt = UINT32_MAX;
                for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (mp.memoryTypes[i].propertyFlags &
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
                if (mt != UINT32_MAX) {
                    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
                    vkAllocateMemory(vkDevice, &ai, nullptr, &g_animBtnStagingMem);
                    vkBindBufferMemory(vkDevice, g_animBtnStaging, g_animBtnStagingMem, 0);
                    vkMapMemory(vkDevice, g_animBtnStagingMem, 0, bi.size, 0, &g_animBtnStagingMapped);
                } else ok = false;
            }
            if (ok) {
                VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pci.queueFamilyIndex = queueFamilyIndex;
                ok = vkCreateCommandPool(vkDevice, &pci, nullptr, &g_animBtnCmdPool) == VK_SUCCESS;
            }
            g_animBtnReady = ok;
            LOG_INFO("Animation-button layer resources %s", ok ? "created" : "FAILED");
        } else {
            LOG_WARN("Animation-button layer init failed — button will not show");
        }
    }

    // ── Toast window-space layer resources ───────────────────────────────────
    // Same shape as the button bar above (own swapchain + text renderer +
    // staging + cmd pool), sized to the chip. Failure is non-fatal: without it
    // the toast simply doesn't draw and the HUD's Recenter line still reports.
    if (hudOk && xr.hasHudSwapchain) {
        if (InitializeHudRenderer(g_toastHud, TOAST_TEX_W, TOAST_TEX_H, TOAST_FONT_BASE) &&
            CreateWindowSpaceSwapchain(xr, g_toastSwapchain, TOAST_TEX_W, TOAST_TEX_H)) {
            g_hasToastSwapchain = true;
            uint32_t c = g_toastSwapchain.imageCount;
            g_toastSwapImages.resize(c, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(g_toastSwapchain.swapchain, c, &c,
                (XrSwapchainImageBaseHeader*)g_toastSwapImages.data());

            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = (VkDeviceSize)TOAST_TEX_W * TOAST_TEX_H * 4;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            bool ok = vkCreateBuffer(vkDevice, &bi, nullptr, &g_toastStaging) == VK_SUCCESS;
            if (ok) {
                VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vkDevice, g_toastStaging, &mr);
                VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physDevice, &mp);
                uint32_t mt = UINT32_MAX;
                for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (mp.memoryTypes[i].propertyFlags &
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
                if (mt != UINT32_MAX) {
                    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
                    vkAllocateMemory(vkDevice, &ai, nullptr, &g_toastStagingMem);
                    vkBindBufferMemory(vkDevice, g_toastStaging, g_toastStagingMem, 0);
                    vkMapMemory(vkDevice, g_toastStagingMem, 0, bi.size, 0, &g_toastStagingMapped);
                } else ok = false;
            }
            if (ok) {
                VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pci.queueFamilyIndex = queueFamilyIndex;
                ok = vkCreateCommandPool(vkDevice, &pci, nullptr, &g_toastCmdPool) == VK_SUCCESS;
            }
            g_toastReady = ok;
            LOG_INFO("Toast layer resources %s", ok ? "created" : "FAILED");
        } else {
            LOG_WARN("Toast layer init failed — toasts will not show");
        }
    }

    // Undocking must not steal focus from whatever spawned us — the browser
    // tab stays active while the model appears beside it. SW_SHOWNOACTIVATE
    // shows without activating; the window is already WS_VISIBLE|WS_EX_TOPMOST
    // from creation, so it is on top without being foreground.
    if (g_launch.transparent) {
        // Twice on purpose: the FIRST ShowWindow of a process started with
        // SW_SHOWDEFAULT is overridden by the launcher's STARTUPINFO, which
        // would activate the window and defeat the whole point. The second
        // call is the one that is honoured verbatim.
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(hwnd, nCmdShow);
    }
    UpdateWindow(hwnd);

    // --src=<url>: start the download now that the toast layer exists, so the
    // progress chip has somewhere to land. Detached worker; the loaded path
    // crosses back through the render thread's load queue.
    if (g_launch.srcKind == dxr::LaunchSrcKind::Url) {
        StartSrcFetch(hwnd, g_launch.src, g_launch.fromProtocol, g_launch.maxBytes,
                      g_launch.noCache);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom  DblClick=Focus");
    LOG_INFO("          -/= Depth  Space=Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          L=Load  Tab=HUD  F11=Fullscreen  ESC=Quit");
    LOG_INFO("");

    g_inputState.viewParams.virtualDisplayHeight = kFallbackVirtualDisplayHeightM;
    g_inputState.renderingModeCount = xr.renderingModeCount;
    // Adopt the display's active mode rather than forcing mode 1, so a 4-view
    // Quad display renders 4 tiles instead of a hard-coded 2. Gated on that mode
    // being 3D: a display commonly reports its 2D mode active at startup (it
    // stays 2D until something asks for 3D), and adopting it verbatim opens this
    // 3D demo in mono — verified on macOS as a 1-tile atlas where the build
    // before produced 2. Falls back to mode 1 (first 3D mode) otherwise. The
    // main loop's dispatch re-asserts it; the runtime event keeps it current.
    const bool activeIs3D =
        xr.currentModeIndex < xr.renderingModeCount &&
        xr.renderingModeDisplay3D[xr.currentModeIndex];
    g_inputState.absoluteRenderingModeRequested =
        activeIs3D ? (int)xr.currentModeIndex : 1;
    g_inputState.hudVisible = false;     // hidden by default; toggle with Tab
    // Auto-orbit on after 10 s idle — unless a deterministic capture asked for it
    // off. This runs AFTER the renderer-init block that honours
    // DXR_MODELVIEWER_DETERMINISTIC, so an unconditional true here silently undid
    // the pin and every capture came out at a different rotation.
    g_inputState.animateEnabled = !DeterministicCaptureRequested();
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
    }

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, vkDevice, graphicsQueue,
        queueFamilyIndex, vkInstance, physDevice,
        &swapchainVkImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudStagingBuffer, hudStagingMapped, hudCmdPool,
        hudOk ? &hudSwapImages : nullptr,
        (VkCommandPool)VK_NULL_HANDLE, (std::vector<XrSwapchainImageVulkanKHR>*)nullptr,
        (uint32_t)0, (uint32_t)0);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    g_modelRenderer.cleanup();

    if (hudCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, hudCmdPool, nullptr);
    if (hudStagingBuffer != VK_NULL_HANDLE) {
        vkUnmapMemory(vkDevice, hudStagingMemory);
        vkDestroyBuffer(vkDevice, hudStagingBuffer, nullptr);
    }
    if (hudStagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, hudStagingMemory, nullptr);
    if (hudOk) CleanupHudRenderer(hudRenderer);

    // Animation-button layer resources.
    if (g_animBtnCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, g_animBtnCmdPool, nullptr);
    if (g_animBtnStaging != VK_NULL_HANDLE) {
        if (g_animBtnStagingMapped) vkUnmapMemory(vkDevice, g_animBtnStagingMem);
        vkDestroyBuffer(vkDevice, g_animBtnStaging, nullptr);
    }
    if (g_animBtnStagingMem != VK_NULL_HANDLE) vkFreeMemory(vkDevice, g_animBtnStagingMem, nullptr);
    if (g_animBtnReady) CleanupHudRenderer(g_animBtnHud);

    // App-owned animation-button swapchain: destroy before CleanupOpenXR tears
    // the session down (used to live in the vendored XrSessionManager cleanup).
    if (g_animBtnSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_animBtnSwapchain.swapchain);
        g_animBtnSwapchain.swapchain = XR_NULL_HANDLE;
        g_hasAnimBtnSwapchain = false;
    }

    // Toast layer resources (same teardown order as the button bar above).
    if (g_toastCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, g_toastCmdPool, nullptr);
    if (g_toastStaging != VK_NULL_HANDLE) {
        if (g_toastStagingMapped) vkUnmapMemory(vkDevice, g_toastStagingMem);
        vkDestroyBuffer(vkDevice, g_toastStaging, nullptr);
    }
    if (g_toastStagingMem != VK_NULL_HANDLE) vkFreeMemory(vkDevice, g_toastStagingMem, nullptr);
    if (g_toastReady) CleanupHudRenderer(g_toastHud);
    if (g_toastSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_toastSwapchain.swapchain);
        g_toastSwapchain.swapchain = XR_NULL_HANDLE;
        g_hasToastSwapchain = false;
    }

    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    g_appHwnd = nullptr;
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    // Held for the process lifetime so a second protocol launch forwards here
    // instead of opening a second window; released last, after the window is
    // gone, so the next instance cannot find a dying HWND to SendMessage.
    if (g_singleInstanceMutex) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}

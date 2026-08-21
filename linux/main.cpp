// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux Vulkan OpenXR glTF/PBR model viewer (handle app, X11 window).
 *
 * The Linux arm of displayxr-demo-modelviewer. Renders glTF/STL/OBJ/FBX/USD
 * models on tracked 3D displays via OpenXR + Vulkan, reusing the vendor-neutral
 * model_common/ModelRenderer PBR pipeline shared with the macOS (macos/main.mm)
 * and Windows (windows/main.cpp) entry points.
 *
 * WINDOWING — HANDLE app (matching the binary name and the other platforms):
 * the app owns a decorated X11 window centered on the 3D panel and passes it
 * via XR_DXR_xlib_window_binding, so the runtime weaves window-relative
 * (runtime#729/#730) and the app receives keyboard input. The window defaults
 * to 1920x1080 centered on the panel (XR_DXR_display_info desktop rect, else
 * Xrandr); MODEL_WINDOW="WxH+X+Y" overrides. When no X server (or window
 * creation fails) the app falls back to the previous hosted-NULL path — which
 * also keeps this compiling and startable on the build-green CI runner.
 *
 * INPUT — O / Ctrl+O opens a zenity file-selection dialog (async fork/exec, no
 * hard dependency: absent zenity logs and no-ops) and loads the chosen model
 * via the shared ModelRenderer. The camera is otherwise the fixed framed
 * default; it consumes XR_DXR_view_rig render-ready views when the runtime
 * offers them, matching the macOS render path.
 */

#include <vulkan/vulkan.h>

// X11 window + input (handle app) — before the OpenXR platform header so the
// xlib binding struct sees the real Display/Window types.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_xlib_window_binding.h>

#include <array>
#include <chrono>
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
#include "display3d_view.h"
#include "projection_depth.h"
#include "model_renderer.h"
#include "recenter_control.h"  // dynamic-recenter per-axis pins (DXR_RECENTER_PIN on Linux)
#include "auto_fit.h"          // dxr::AutoFitVHeight — shared width-aware load-time framing
#include "model_loader.h"

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

// Camera / fit state (fixed framing — no live input on Linux build-green).
static ViewParams g_viewParams;
static float g_camPos[3] = {0, 0, 0};   // = the fit centre (fixed camera; no user offset)
static float g_camYaw = 0.0f, g_camPitch = 0.0f;

// Dynamic-recenter pins. Default X Y Z matches the Windows modelviewer's hard
// pin. No keyboard on this build-green target — control via DXR_RECENTER_PIN.
static dxr::RecenterControl g_recenter;

// Rig position for this frame: a pinned axis tracks the smoothed animated
// centroid; an unpinned axis stays at the fixed fit centre (g_camPos). No user
// offset on Linux (fixed camera). No-op for static models (anchor invalid).
static void ComputeRigPosition(float out[3]) {
    out[0] = g_camPos[0];
    out[1] = g_camPos[1];
    out[2] = g_camPos[2];
    float anchor[3];
    if (g_modelRenderer.getAnimatedAnchor(anchor)) {
        const dxr::RecenterPins pins = g_recenter.pins();
        if (pins.x) out[0] = anchor[0];
        if (pins.y) out[1] = anchor[1];
        if (pins.z) out[2] = anchor[2];
    }
}

static constexpr float kDefaultVirtualDisplayHeightM = 1.5f;
// Load-time framing is the shared width-aware rule from displayxr-common
// (dxr::AutoFitVHeight, default 80% fill in BOTH axes) — no separate vertical
// comfort multiplier; the fill fraction IS the headroom.

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
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerZ = 0.5f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
    int32_t displayScreenLeft = 0;     // 3D-panel top-left in virtual-desktop px (INV-1.3)
    int32_t displayScreenTop = 0;

    // App-owned X11 window (handle app). Null display = hosted-NULL fallback.
    Display* xDisplay = nullptr;
    Window xWindow = 0;
    unsigned int xWinW = 0, xWinH = 0;

    PFN_xrRequestDisplayRenderingModeDXR pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateDisplayRenderingModesEXT = nullptr;

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

static bool InitializeOpenXR(AppXrSession& xr) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(e.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(e.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) xr.hasViewRigExt = true;
    }
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasViewRigExt) enabled.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    LOG_INFO("XR_DXR_view_rig: %s", xr.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");
    // NOTE: no XR_EXT_*_window_binding — hosted-NULL (runtime self-creates the
    // window). Swap for XR_DXR_xlib_window_binding when the Phase-3 windowing
    // arm lands (see file header).

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

    // Handle app: pass the app-owned X11 window via XR_DXR_xlib_window_binding
    // so the runtime weaves window-relative (runtime#729/#730). Falls back to
    // hosted-NULL (no window binding) when CreateAppWindow didn't run.
    XrXlibWindowBindingCreateInfoDXR xlibBinding = {XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR};
    xlibBinding.next = &vkBinding;
    xlibBinding.xDisplay = xr.xDisplay;
    xlibBinding.window = xr.xWindow;
    xlibBinding.transparentBackgroundEnabled = XR_FALSE;
    const bool useAppWindow = (xr.xDisplay != nullptr && xr.xWindow != 0);

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = useAppWindow ? (const void*)&xlibBinding : (const void*)&vkBinding;
    si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));
    LOG_INFO("Session created (%s)", useAppWindow ? "app-owned window, handle app" : "hosted-NULL");

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
    // Tear down the app-owned X11 window after the runtime has released it.
    if (xr.xWindow != 0 && xr.xDisplay != nullptr) XDestroyWindow(xr.xDisplay, xr.xWindow);
    if (xr.xDisplay != nullptr) XCloseDisplay(xr.xDisplay);
    xr.xWindow = 0;
    xr.xDisplay = nullptr;
}

// ============================================================================
// App-owned X11 window (handle app)
// ============================================================================

static const unsigned int kDefaultWindowW = 1920;
static const unsigned int kDefaultWindowH = 1080;
static Atom g_wmDeleteAtom = None;

// Find the target panel rect (virtual-desktop px). Prefer the RandR PRIMARY
// output; else the largest connected NON-eDP/LVDS output (the 3D display is an
// external panel, not the laptop's built-in). Same helper as the avatar demo.
static bool GetPanelRect(Display* dpy, Window root, int& x, int& y, int& w, int& h) {
    XRRScreenResources* res = XRRGetScreenResources(dpy, root);
    if (res == nullptr) return false;

    auto tryOutput = [&](RROutput out) -> bool {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, out);
        if (oi == nullptr) return false;
        bool ok = false;
        if (oi->connection == RR_Connected && oi->crtc != 0) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci != nullptr && ci->width > 0 && ci->height > 0) {
                x = ci->x; y = ci->y; w = (int)ci->width; h = (int)ci->height;
                ok = true;
            }
            if (ci != nullptr) XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
        return ok;
    };

    bool found = false;
    RROutput primary = XRRGetOutputPrimary(dpy, root);
    if (primary != 0 && tryOutput(primary)) {
        found = true;
    }
    if (!found) {
        long bestArea = 0;
        for (int i = 0; i < res->noutput; i++) {
            XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (oi == nullptr) continue;
            const bool isBuiltin = oi->name != nullptr &&
                (strncasecmp(oi->name, "eDP", 3) == 0 || strncasecmp(oi->name, "LVDS", 4) == 0);
            if (oi->connection == RR_Connected && oi->crtc != 0 && !isBuiltin) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
                if (ci != nullptr && ci->width > 0 && ci->height > 0) {
                    const long area = (long)ci->width * (long)ci->height;
                    if (area > bestArea) {
                        bestArea = area;
                        x = ci->x; y = ci->y; w = (int)ci->width; h = (int)ci->height;
                        found = true;
                    }
                }
                if (ci != nullptr) XRRFreeCrtcInfo(ci);
            }
            XRRFreeOutputInfo(oi);
        }
    }
    XRRFreeScreenResources(res);
    return found;
}

// Create a normal decorated X11 window for the viewer (opaque, default visual),
// landscape 1920x1080 centered on the 3D panel — the app passes it via
// XR_DXR_xlib_window_binding so the runtime weaves window-relative. Override
// with MODEL_WINDOW="WxH+X+Y" (X,Y absolute virtual-desktop px; WxH alone
// re-centers). Returns false (xDisplay left null) when no X server is
// available, so the caller falls back to hosted-NULL (also the CI-safe path).
static bool CreateAppWindow(AppXrSession& xr) {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        LOG_INFO("XOpenDisplay failed (no X server) — using hosted-NULL windowing");
        return false;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XSetWindowAttributes attrs = {};
    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.event_mask = StructureNotifyMask | KeyPressMask;

    unsigned int w = kDefaultWindowW, h = kDefaultWindowH;
    int px = 0, py = 0;
    int prx = 0, pry = 0, prw = 0, prh = 0;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        // Authoritative: XR_DXR_display_info reports the 3D panel's desktop
        // rect. Prefer it — on a multi-monitor box the RandR PRIMARY is often
        // NOT the 3D panel.
        prx = xr.displayScreenLeft;
        pry = xr.displayScreenTop;
        prw = (int)xr.displayPixelWidth;
        prh = (int)xr.displayPixelHeight;
        px = prx + (prw - (int)w) / 2;
        py = pry + (prh - (int)h) / 2;
        LOG_INFO("3D panel (display_info) %dx%d at (%d,%d) — centering %ux%u window at (%d,%d)",
                 prw, prh, prx, pry, w, h, px, py);
    } else if (GetPanelRect(dpy, root, prx, pry, prw, prh)) {
        px = prx + (prw - (int)w) / 2;
        py = pry + (prh - (int)h) / 2;
        LOG_INFO("Panel rect (Xrandr) %dx%d at (%d,%d) — centering %ux%u window at (%d,%d)",
                 prw, prh, prx, pry, w, h, px, py);
    } else {
        const int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
        px = (sw - (int)w) / 2;
        py = (sh - (int)h) / 2;
        LOG_INFO("Xrandr panel query failed — centering %ux%u on default screen %dx%d at (%d,%d)",
                 w, h, sw, sh, px, py);
    }

    // MODEL_WINDOW="WxH+X+Y" override (X,Y absolute virtual-desktop px); WxH
    // alone re-centers on the same panel/screen origin.
    if (const char* wenv = getenv("MODEL_WINDOW")) {
        unsigned int ow = 0, oh = 0; int ox = 0, oy = 0;
        int n = sscanf(wenv, "%ux%u+%d+%d", &ow, &oh, &ox, &oy);
        if (n >= 2 && ow > 0 && oh > 0) {
            w = ow; h = oh;
            if (n >= 4) {
                px = ox; py = oy;
                LOG_INFO("MODEL_WINDOW override: %ux%u at absolute (%d,%d)", w, h, px, py);
            } else {
                if (prw > 0 && prh > 0) { px = prx + (prw - (int)w) / 2; py = pry + (prh - (int)h) / 2; }
                LOG_INFO("MODEL_WINDOW override: %ux%u (re-centered at %d,%d)", w, h, px, py);
            }
        }
    }

    Window win = XCreateWindow(dpy, root, px, py, w, h, 0, CopyFromParent, InputOutput,
                               CopyFromParent, CWBackPixel | CWEventMask, &attrs);
    if (win == 0) {
        LOG_ERROR("XCreateWindow failed — using hosted-NULL windowing");
        XCloseDisplay(dpy);
        return false;
    }
    XStoreName(dpy, win, "DisplayXR 3D Model Viewer");

    // Close button → clean exit (ClientMessage in the event pump).
    g_wmDeleteAtom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    if (g_wmDeleteAtom != None) XSetWMProtocols(dpy, win, &g_wmDeleteAtom, 1);

    // WM_NORMAL_HINTS with USPosition|PPosition so the WM honors the
    // create-time position instead of auto-placing (GNOME/Mutter).
    {
        XSizeHints hints = {};
        hints.flags = USPosition | PPosition;
        hints.x = px; hints.y = py;
        XSetWMNormalHints(dpy, win, &hints);
    }

    XMapWindow(dpy, win);
    XFlush(dpy);
    // Re-assert the position after mapping — Mutter ignores the create-time
    // x/y of a freshly-mapped toplevel but honors a post-map move.
    XMoveWindow(dpy, win, px, py);
    XFlush(dpy);

    xr.xDisplay = dpy;
    xr.xWindow = win;
    xr.xWinW = w;
    xr.xWinH = h;
    LOG_INFO("Created %ux%u app window at (%d,%d) — XR_DXR_xlib_window_binding", w, h, px, py);
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
    if (g_modelRenderer.getRobustSceneBounds(0.05f, 0.95f, center, extent)) {
        g_camPos[0] = center[0]; g_camPos[1] = center[1]; g_camPos[2] = center[2];
        // Viewport: this leg is hosted-NULL, so g_windowW/H is the runtime's
        // presentation surface (the panel, or the X window when we own one) —
        // set during init, before the first load. Only its aspect matters.
        const float viewportW = (float)g_windowW, viewportH = (float)g_windowH;
        float vh = dxr::AutoFitVHeight(extent[0], extent[1], viewportW, viewportH);
        if (!(vh > 1e-3f)) vh = kDefaultVirtualDisplayHeightM;
        g_viewParams.virtualDisplayHeight = vh;
        const bool haveViewport = (viewportW > 0.0f && viewportH > 0.0f);
        const float aspect = haveViewport ? (viewportW / viewportH) : 0.0f;
        const char* boundBy = !haveViewport ? "height (no viewport)"
                            : (extent[0] / aspect > extent[1]) ? "width" : "height";
        LOG_INFO("Auto-fit: center=(%.3f,%.3f,%.3f) extent W=%.3f H=%.3f "
                 "viewport=%.0fx%.0f (aspect %.3f) bound-by=%s vHeight=%.3f",
                 center[0], center[1], center[2], extent[0], extent[1],
                 viewportW, viewportH, aspect, boundBy, vh);
    } else {
        g_viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
    }
    g_camYaw = 0.0f; g_camPitch = 0.0f;
    g_viewParams.scaleFactor = 1.0f;
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

// Pump the app window's X11 events: O / Ctrl+O = open-file dialog, close
// button = clean exit, ConfigureNotify = track live window size.
static void PumpXEvents(AppXrSession& xr) {
    if (xr.xDisplay == nullptr) return;
    while (XPending(xr.xDisplay) > 0) {
        XEvent ev;
        XNextEvent(xr.xDisplay, &ev);
        switch (ev.type) {
        case KeyPress: {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            // Ctrl+O = open a model (uniform across demos + platforms, incl.
            // the mediaplayer). Strict: Ctrl must be held (bare O does nothing).
            if ((sym == XK_o || sym == XK_O) && (ev.xkey.state & ControlMask)) StartFilePicker();
            // Viewing conditions (issue #70): [ / ] step exposure in quarter
            // stops, G cycles the tone curve. Both are reported in the log so a
            // reference capture records the grading it was taken under.
            else if (sym == XK_bracketleft || sym == XK_bracketright) {
                const float step = (sym == XK_bracketright) ? 0.25f : -0.25f;
                g_modelRenderer.setExposureEV(g_modelRenderer.exposureEV() + step);
                LOG_INFO("Exposure: %+.2f EV", g_modelRenderer.exposureEV());
            } else if (sym == XK_g || sym == XK_G) {
                g_modelRenderer.cycleToneCurve();
                LOG_INFO("Tone curve: %s", g_modelRenderer.toneCurveName());
            }
            break;
        }
        case ConfigureNotify:
            if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
                xr.xWinW = (unsigned int)ev.xconfigure.width;
                xr.xWinH = (unsigned int)ev.xconfigure.height;
                g_windowW = xr.xWinW;
                g_windowH = xr.xWinH;
            }
            break;
        case ClientMessage:
            if (g_wmDeleteAtom != None && (Atom)ev.xclient.data.l[0] == g_wmDeleteAtom) {
                LOG_INFO("Window closed — exiting");
                g_running = false;
            }
            break;
        default: break;
        }
    }
}

static void SignalHandler(int) { g_running = false; }

// ============================================================================
// main
// ============================================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== DisplayXR 3D Model Viewer (Vulkan, Linux / hosted-NULL) ===");

    // Dynamic-recenter pins: default hard-pin X+Y+Z (modelviewer parity).
    // DXR_RECENTER_PIN=XYZ|XY|Z|- overrides (the Linux control path).
    g_recenter.init(/*x=*/true, /*y=*/true, /*z=*/true);
    { char lbl[24]; g_recenter.hudLabel(lbl, sizeof(lbl)); LOG_INFO("Recenter: %s", lbl); }

    { const char* mode = getenv("SIM_DISPLAY_OUTPUT");
      if (mode) {
          if (strcmp(mode, "sbs") == 0) g_windowW = 2560; // hint only; runtime owns the window
      } }

    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // Handle app: own an X11 window on the 3D panel (display_info queried in
    // InitializeOpenXR gives the panel rect). Falls back to hosted-NULL when
    // no X server is available (also the CI-safe path — CI never runs this).
    CreateAppWindow(xr);

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }
    std::vector<const char*> devExts; std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }
    VkDevice vkDevice = VK_NULL_HANDLE; VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }
    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

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
    if (xr.xWinW > 0 && xr.xWinH > 0) {
        g_windowW = xr.xWinW;
        g_windowH = xr.xWinH;
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
    TryAutoLoadBundledScene();

    LOG_INFO("=== Entering main loop ===");
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Assert the app's default 3D rendering mode once the session is running.
        if (xr.sessionRunning && xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE)
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, xr.currentRenderingMode);

        g_modelRenderer.updateAnimation(dt);
        PollEvents(xr);
        PumpXEvents(xr);   // O / Ctrl+O = open dialog; close button = exit
        PollFilePicker();  // async zenity result → loadModel + auto-fit

        if (!xr.sessionRunning) { struct timespec ts{0, 50 * 1000 * 1000}; nanosleep(&ts, nullptr); continue; }

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
            quat_from_yaw_pitch(g_camYaw, g_camPitch, &cameraPose.orientation);
            float rigPos[3]; ComputeRigPosition(rigPos);
            cameraPose.position = {rigPos[0], rigPos[1], rigPos[2]};
            const float rigVH = g_viewParams.virtualDisplayHeight / g_viewParams.scaleFactor;

            const bool useRig = xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;
            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
            if (useRig) {
                displayRig.pose = cameraPose;
                displayRig.virtualDisplayHeight = rigVH;
                displayRig.ipdFactor = g_viewParams.ipdFactor;
                displayRig.parallaxFactor = g_viewParams.parallaxFactor;
                displayRig.perspectiveFactor = g_viewParams.perspectiveFactor;
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
                if (modeViewCount > runtimeViewCount) modeViewCount = runtimeViewCount;
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

                uint32_t imageIndex;
                if (AcquireSwapchainImage(xr, imageIndex)) {
                    rendered = true;
                    projectionViews.assign((size_t)eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
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

                    if (g_modelRenderer.hasModel()) {
                        VkImage targetImage = swapchainImages[imageIndex].image;
                        VkFormat swapFormat = (VkFormat)xr.swapchain.format;
                        for (int eye = 0; eye < eyeCount; eye++)
                            g_modelRenderer.renderEye(targetImage, swapFormat,
                                xr.swapchain.width, xr.swapchain.height,
                                tileOffsets[eye].first, tileOffsets[eye].second,
                                renderW, renderH, viewMat[eye].data(), projMat[eye].data());
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
    CleanupOpenXR(xr);
    if (vkDevice) vkDestroyDevice(vkDevice, nullptr);
    if (vkInstance) vkDestroyInstance(vkInstance, nullptr);
    LOG_INFO("Clean exit");
    return 0;
}

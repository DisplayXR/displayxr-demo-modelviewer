// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_DXR_win32_window_binding
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr_session_common.h"
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_mcp_tools.h>
#include <string>

// XR_DXR_view_rig (W7 of #396): the runtime owns the off-axis Kooima and
// returns render-ready XrView{pose, fov}; the app deletes its own. App-owned
// flag (displayxr::common's XrSessionManager carries no app-named fields,
// #396 W4); set by InitializeOpenXR.
extern bool g_hasViewRigExt;

// INV-1.3 (XR_DXR_display_info v16, runtime#715): 3D panel top-left in
// Windows virtual-screen pixels (XrDisplayDesktopPositionDXR). (0,0) =
// primary monitor / unknown — safe default, incl. on pre-v16 runtimes.
// Set by InitializeOpenXR; the app creates its window at this position.
extern int32_t g_displayDesktopLeft;
extern int32_t g_displayDesktopTop;

// ── XR_DXR_mcp_tools (#47): app-defined agent tools ─────────────────────────
// Ported from the macOS build (macos/main.mm). The model viewer registers its
// own appId ("modelviewer") + tools and answers agent tool calls that arrive as
// XrEventDataMCPToolCallDXR. This state is app-owned (like g_hasViewRigExt)
// rather than living on displayxr::common's XrSessionManager, because the
// shared PollEvents() hardcodes a cube-app set_spin/get_status handler with no
// custom-tool hook — so the model viewer owns its event poll
// (PollEventsModelViewer) and MCP dispatch. Every PFN is resolved defensively:
// if the extension is absent or any entry point is NULL the whole feature is
// inert (older runtime / MCP capability gate off) and registration is skipped —
// never a crash. Set by InitializeOpenXR.
extern bool                         g_hasMcpToolsExt;
extern PFN_xrSetMCPAppInfoDXR       g_pfnSetMcpAppInfo;
extern PFN_xrRegisterMCPToolDXR     g_pfnRegisterMcpTool;
extern PFN_xrUnregisterMCPToolDXR   g_pfnUnregisterMcpTool;
extern PFN_xrGetMCPToolCallArgsDXR  g_pfnGetMcpToolCallArgs;
extern PFN_xrSubmitMCPToolResultDXR g_pfnSubmitMcpToolResult;

// True only when the extension is advertised AND every entry point resolved.
// Gate all registration / dispatch on this so the path stays a no-op on
// runtimes that predate the extension or leave the MCP capability off.
inline bool McpToolsResolved() {
    return g_hasMcpToolsExt && g_pfnSetMcpAppInfo && g_pfnRegisterMcpTool &&
           g_pfnUnregisterMcpTool && g_pfnGetMcpToolCallArgs &&
           g_pfnSubmitMcpToolResult;
}

// App-installed dispatcher: given a tool name + JSON args (argsJson is "" for
// no-arg tools), acts on the real Windows app state (model loader / camera /
// animation controller), fills success, and returns the result JSON. Runs on
// the render thread, from PollEventsModelViewer — where app state is naturally
// consistent (no locking beyond the app's own mutexes).
using McpToolCallHandler = std::string (*)(const char* toolName,
                                           const char* argsJson, bool& success);
void SetMcpToolCallHandler(McpToolCallHandler handler);

// Model-viewer-owned event poll. Replicates displayxr::common's PollEvents()
// (session-state machine + rendering-mode / eye-tracking / file-picker events)
// AND routes XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_DXR to the installed handler.
// Replaces the shared PollEvents(*xr) call in the render loop — see the note
// on the g_*Mcp* globals above for why the shared one can't serve the viewer's
// custom tools (its baked-in set_spin/get_status handler would consume + drop
// them, timing the agent out after ~5 s).
bool PollEventsModelViewer(XrSessionManager& xr);

// Initialize OpenXR instance with Vulkan + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get Vulkan graphics requirements and set up Vulkan instance/device per OpenXR spec
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance with required extensions from the runtime
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device selected by the runtime
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// Get required device extensions from the runtime
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage);

// Find a graphics queue family
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device with required extensions
bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue);

// Create OpenXR session with Vulkan binding + win32_window_binding
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd);

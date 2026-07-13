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
// own appId ("modelviewer") + tools; agent tool calls arrive as
// XrEventDataMCPToolCallDXR and are dispatched by the shared PollEvents (common
// v2.1.0) through XrSessionManager::mcpToolHandler — the app installs a handler
// (main.cpp) and no longer forks PollEvents. Only the REGISTRATION entry points
// stay app-owned (SetAppInfo / Register / Unregister — the last has no field on
// displayxr::common's XrSessionManager, and the viewer late-(un)registers its
// animation tools). The DISPATCH entry points (GetToolCallArgs / SubmitResult)
// are owned by PollEvents and live on the XrSessionManager (xr.pfn*EXT), set in
// InitializeOpenXR. Every PFN is resolved defensively: if the extension is
// absent or any registration entry point is NULL, McpToolsResolved() is false
// and registration is skipped (older runtime / MCP capability gate off) — never
// a crash. Set by InitializeOpenXR.
extern bool                         g_hasMcpToolsExt;
extern PFN_xrSetMCPAppInfoDXR       g_pfnSetMcpAppInfo;
extern PFN_xrRegisterMCPToolDXR     g_pfnRegisterMcpTool;
extern PFN_xrUnregisterMCPToolDXR   g_pfnUnregisterMcpTool;

// True only when the extension is advertised AND every registration entry point
// resolved. Gate all tool registration on this so the path stays a no-op on
// runtimes that predate the extension or leave the MCP capability off. (The
// dispatch entry points are the shared PollEvents' concern, resolved onto the
// XrSessionManager.)
inline bool McpToolsResolved() {
    return g_hasMcpToolsExt && g_pfnSetMcpAppInfo && g_pfnRegisterMcpTool &&
           g_pfnUnregisterMcpTool;
}

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

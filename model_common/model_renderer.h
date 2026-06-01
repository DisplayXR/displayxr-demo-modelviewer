// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  glTF 2.0 PBR model renderer for the DisplayXR model-viewer demo.
 *
 * Vendor-neutral analog of 3dgs_common/gs_renderer.h. Loads a .glb/.gltf
 * model via tinygltf and rasterises it with a metallic-roughness PBR pass to
 * a viewport region of an OpenXR-provided Vulkan swapchain image, once per
 * eye/view, using the asymmetric per-eye Kooima projection the platform code
 * supplies.
 *
 * The intended implementation is a port of the MIT-licensed
 * SaschaWillems/Vulkan-glTF-PBR `vkglTF::Model` loader + PBR/IBL shaders.
 * This header is the SKELETON: the method surface mirrors what
 * windows/main.cpp + macos/main.mm already call on GsRenderer, so the
 * platform glue retargets with a near-mechanical rename. See ../PORTING.md.
 *
 * Method bodies in model_renderer.cpp are stubs (return false / clear to the
 * background colour) until the renderer is ported.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>

struct ModelRenderer {
    // Initialize with the OpenXR runtime's Vulkan resources.
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    // Load a glTF 2.0 model (.glb or .gltf). Parses the scene graph, uploads
    // vertex/index buffers + material textures, builds the PBR pipeline.
    bool loadModel(const char* gltfPath);

    // Load a built-in debug model (e.g. a unit cube) so the demo renders
    // something before any file is opened.
    bool loadDebugModel();

    // Returns true if a model is currently loaded.
    bool hasModel() const;

    // Returns the loaded model file path.
    const std::string& modelPath() const;

    // Total primitive (draw-call) count across the loaded scene graph.
    uint32_t primitiveCount() const;

    // World-space axis-aligned bounding box of the loaded model.
    // Returns false if no model is loaded; otherwise writes min/max xyz.
    bool getSceneBBox(float outMin[3], float outMax[3]) const;

    // Robust center + per-axis extent for auto-framing. For a mesh model the
    // raw AABB is already robust, so this can forward to getSceneBBox; kept as
    // a distinct entry point because the platform framing code calls it.
    // Returns false if no model is loaded.
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;

    // Raycast pick against the loaded mesh: nearest surface hit along a ray.
    // Returns true if a hit was found, with the hit point written to hitPos.
    bool pickSurface(const float rayOrigin[3], const float rayDir[3],
                     float hitPos[3], float maxDistance = 100.0f) const;

    // Find the display yaw (radians, around world +Y) for initial framing.
    // For a model this can simply return 0 (front-facing) unless a smarter
    // heuristic is wanted. Signature kept to match the platform call site.
    float findBestYaw(const float displayCenter[3],
                      const float viewerOffsetLocal[3],
                      uint32_t numCandidates = 8) const;

    // Render one view to a region of a Vulkan swapchain image.
    // Manages its own command buffers internally (allocate, record, submit, wait).
    // viewMatrix and projMatrix are column-major float[16].
    // transparentBg=true clears uncovered pixels to RGBA(0,0,0,0) so the
    // runtime chroma-key pass yields desktop see-through.
    // clipFarViewSpace>0 culls geometry beyond that view-space depth. 0=off.
    void renderEye(VkImage swapchainImage,
                   VkFormat swapchainFormat,
                   uint32_t imageWidth,
                   uint32_t imageHeight,
                   uint32_t viewportX,
                   uint32_t viewportY,
                   uint32_t viewportWidth,
                   uint32_t viewportHeight,
                   const float viewMatrix[16],
                   const float projMatrix[16],
                   bool transparentBg = false,
                   float clipFarViewSpace = 0.0f);

    // Clean up all resources.
    void cleanup();

    ~ModelRenderer();

private:
    // ── Core Vulkan handles (not owned, from OpenXR runtime) ─────────────
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool modelLoaded_ = false;
    std::string loadedModelPath_;
    uint32_t numPrimitives_ = 0;

    // TODO(port): vkglTF::Model instance, PBR pipeline + layout, descriptor
    // pool/sets, per-material texture handles, IBL cubemaps (irradiance,
    // prefiltered env) + BRDF LUT, depth attachment, the offscreen colour
    // target, and the uniform buffers for camera + light + material params.
    // Lift these from SaschaWillems/Vulkan-glTF-PBR (see ../PORTING.md).
};

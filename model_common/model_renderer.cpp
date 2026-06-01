// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SKELETON implementation of the glTF PBR model renderer.
 *
 * Every method here is a stub. The real implementation is a port of
 * SaschaWillems/Vulkan-glTF-PBR (MIT) — see ../PORTING.md for the file map.
 * The stubs let the library compile and expose the surface the platform
 * code (windows/main.cpp, macos/main.mm) calls, so the renderer can be filled
 * in incrementally without touching the OpenXR/window/HUD/input glue.
 */

#include "model_renderer.h"
#include "model_loader.h"

#include <cstdio>

bool ModelRenderer::init(VkInstance instance,
                         VkPhysicalDevice physicalDevice,
                         VkDevice device,
                         VkQueue queue,
                         uint32_t queueFamilyIndex,
                         uint32_t renderWidth,
                         uint32_t renderHeight) {
    instance_ = instance;
    physDevice_ = physicalDevice;
    device_ = device;
    queue_ = queue;
    queueFamily_ = queueFamilyIndex;
    width_ = renderWidth;
    height_ = renderHeight;
    initialized_ = true;
    // TODO(port): create command pool, PBR pipeline + layout, descriptor
    // pool, depth attachment, offscreen colour target, generate the BRDF LUT
    // + IBL cubemaps once (empty environment is fine to start).
    std::fprintf(stderr, "[model_renderer] init stub: %ux%u (renderer not yet ported)\n",
                 renderWidth, renderHeight);
    return true;
}

bool ModelRenderer::loadModel(const char* gltfPath) {
    if (!initialized_ || !gltfPath) return false;
    // TODO(port): vkglTF::Model::loadFromFile(gltfPath, ...); upload buffers
    // + textures; compute numPrimitives_ and the AABB.
    ModelData md;
    if (!model_loader_load(gltfPath, md)) {
        std::fprintf(stderr, "[model_renderer] failed to load '%s'\n", gltfPath);
        return false;
    }
    loadedModelPath_ = gltfPath;
    numPrimitives_ = md.primitiveCount;
    modelLoaded_ = true;
    return true;
}

bool ModelRenderer::loadDebugModel() {
    if (!initialized_) return false;
    // TODO(port): build a unit cube in-memory and upload it.
    loadedModelPath_ = "<debug-cube>";
    numPrimitives_ = 1;
    modelLoaded_ = true;
    return true;
}

bool ModelRenderer::hasModel() const { return modelLoaded_; }

const std::string& ModelRenderer::modelPath() const { return loadedModelPath_; }

uint32_t ModelRenderer::primitiveCount() const { return numPrimitives_; }

bool ModelRenderer::getSceneBBox(float outMin[3], float outMax[3]) const {
    if (!modelLoaded_) return false;
    // TODO(port): return the model's real AABB. Stub: unit box centred at origin.
    outMin[0] = outMin[1] = outMin[2] = -0.5f;
    outMax[0] = outMax[1] = outMax[2] = 0.5f;
    return true;
}

bool ModelRenderer::getRobustSceneBounds(float /*loPct*/, float /*hiPct*/,
                                         float outCenter[3], float outExtent[3]) const {
    float mn[3], mx[3];
    if (!getSceneBBox(mn, mx)) return false;
    for (int i = 0; i < 3; ++i) {
        outCenter[i] = 0.5f * (mn[i] + mx[i]);
        outExtent[i] = mx[i] - mn[i];
    }
    return true;
}

bool ModelRenderer::pickSurface(const float[3], const float[3],
                                float[3], float) const {
    // TODO(port): ray/triangle intersection against the loaded mesh.
    return false;
}

float ModelRenderer::findBestYaw(const float[3], const float[3], uint32_t) const {
    // Models are authored front-facing; default to no rotation.
    return 0.0f;
}

void ModelRenderer::renderEye(VkImage /*swapchainImage*/,
                              VkFormat /*swapchainFormat*/,
                              uint32_t /*imageWidth*/,
                              uint32_t /*imageHeight*/,
                              uint32_t /*viewportX*/,
                              uint32_t /*viewportY*/,
                              uint32_t /*viewportWidth*/,
                              uint32_t /*viewportHeight*/,
                              const float /*viewMatrix*/[16],
                              const float /*projMatrix*/[16],
                              bool /*transparentBg*/,
                              float /*clipFarViewSpace*/) {
    // TODO(port): transition the viewport region, begin the PBR render pass
    // into the offscreen target, draw the node hierarchy with per-primitive
    // material descriptor sets, then blit/copy into the swapchain viewport.
    // No-op stub: leaves the swapchain region untouched.
}

void ModelRenderer::cleanup() {
    if (!initialized_) return;
    // TODO(port): destroy pipelines, descriptor pool, images, buffers,
    // command pool. Stub clears flags only.
    modelLoaded_ = false;
    initialized_ = false;
}

ModelRenderer::~ModelRenderer() { cleanup(); }

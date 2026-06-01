// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  glTF 2.0 PBR model renderer for the DisplayXR model-viewer demo.
 *
 * Vendor-neutral analog of 3dgs_common/gs_renderer.h. Loads a .glb/.gltf model
 * via model_loader (tinygltf) and rasterises it with a metallic-roughness PBR
 * pass into an internal colour image, then blits that into the per-eye
 * swapchain viewport region — reusing the exact viewport-copy + transparency
 * scaffolding the GS renderer uses, so it drops into the platform code with a
 * mechanical rename.
 *
 * v1 scope: static geometry, material FACTORS (base color, metallic,
 * roughness, emissive), one directional light + flat ambient. Textures, IBL,
 * skinning and animation are follow-ups; the shader/CMake hooks are in place.
 * See ../PORTING.md.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>
#include <vector>
#include "model_vulkan_utils.h"
#include "model_loader.h"

struct ModelRenderer {
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    bool loadModel(const char* gltfPath);
    bool loadDebugModel();
    bool hasModel() const;
    const std::string& modelPath() const;
    uint32_t primitiveCount() const;

    bool getSceneBBox(float outMin[3], float outMax[3]) const;
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;
    bool pickSurface(const float rayOrigin[3], const float rayDir[3],
                     float hitPos[3], float maxDistance = 100.0f) const;
    float findBestYaw(const float displayCenter[3],
                      const float viewerOffsetLocal[3],
                      uint32_t numCandidates = 8) const;

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

    void cleanup();
    ~ModelRenderer();

private:
    // Push-constant block (must match shaders/pbr.{vert,frag}). 112 bytes.
    struct PushBlock {
        float model[16];
        float baseColorFactor[4];
        float mrParams[4];   // x=metallic, y=roughness
        float emissive[4];   // rgb
    };
    // Set-0 uniform buffer (must match shaders/pbr.{vert,frag}).
    struct UniformBlock {
        float viewProj[16];
        float view[16];        // Z-forward-adjusted view, for the foreground clip
        float cameraPos[4];
        float lightDir[4];     // .xyz = light direction, .w = clipFar (view-space; 0=off)
        float invViewProj[16]; // inverse(viewProj), for the skybox ray reconstruction
    };

    bool createRenderTargets();
    bool ensureTargets(uint32_t w, uint32_t h);   // (re)create color+depth+framebuffer at this size
    bool createPipeline();
    bool createSamplerAndDefaults();
    bool createIbl();   // generate BRDF LUT + irradiance + prefiltered cubes from the analytic sky
    ModelImage uploadTexture(const struct ModelTexture& tex);
    VkDescriptorSet makeMaterialSet(VkImageView baseColor, VkImageView mr,
                                    VkImageView normal, VkImageView occ,
                                    VkImageView emissive);
    bool finalizeModel(struct ModelData& md);   // upload geometry+textures, build material sets
    void updateUniforms(const float viewMatrix[16], const float projMatrix[16], float clipFar);
    void cleanupModel();

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

    // ── Render targets (internal; blitted to the swapchain viewport) ─────
    VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    ModelImage colorImage_;
    ModelImage depthImage_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    // ── Pipeline ──────────────────────────────────────────────────────────
    VkDescriptorSetLayout dsLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline skyboxPipeline_ = VK_NULL_HANDLE;   // analytic-sky background (opaque mode)
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    ModelBuffer uniformBuffer_;   // host-visible UniformBlock

    // ── Material textures (set = 1: 5 combined image samplers) ───────────
    VkSampler sampler_ = VK_NULL_HANDLE;
    ModelImage whiteTex_;        // 1x1 white  — default base-color/MR/AO/emissive
    ModelImage flatNormalTex_;   // 1x1 (128,128,255) — default tangent-space normal
    VkDescriptorSetLayout matSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool matPool_ = VK_NULL_HANDLE;          // recreated per model
    std::vector<VkDescriptorSet> materialSets_;          // one per material
    VkDescriptorSet defaultMatSet_ = VK_NULL_HANDLE;     // for material == -1

    // ── IBL (set = 2: irradiance cube, prefiltered cube, BRDF LUT) ───────
    struct CubeMap {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // cube view (all mips/layers)
        uint32_t size = 0;
        uint32_t mips = 1;
    };
    // Render the analytic sky into each cube face/mip with the given fragment
    // SPIR-V; perMipRoughness pushes {face, roughness} (prefilter) vs {face} (irradiance).
    bool genCubeMap(CubeMap& cube, uint32_t size, uint32_t mips,
                    const uint32_t* fragSpv, size_t fragSpvBytes, bool perMipRoughness);
    ModelImage brdfLut_;                 // 2D R16G16_SFLOAT
    CubeMap irradianceCube_;
    CubeMap prefilterCube_;
    VkSampler iblCubeSampler_ = VK_NULL_HANDLE;
    VkSampler iblLutSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout iblSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool iblPool_ = VK_NULL_HANDLE;
    VkDescriptorSet iblSet_ = VK_NULL_HANDLE;

    // ── Loaded model GPU data ────────────────────────────────────────────
    ModelBuffer vertexBuffer_;
    ModelBuffer indexBuffer_;
    std::vector<ModelImage>     modelTextures_;
    std::vector<ModelMaterial>  materials_;
    std::vector<ModelPrimitive> primitives_;
    float bboxMin_[3] = {0, 0, 0};
    float bboxMax_[3] = {0, 0, 0};
    bool  hasBBox_ = false;
};

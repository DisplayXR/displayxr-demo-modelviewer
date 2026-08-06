// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
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

    // ── Viewing conditions (issue #70 phase 0) ───────────────────────────────
    // A material comparison against an authoring tool is only meaningful when
    // the environment, the exposure and the tone curve are all pinned and
    // written down. These are the knobs that pin them; the HUD reports the
    // active values so a reference capture is self-documenting.

    // Load an equirectangular HDRI (.hdr / .exr-as-float via stb) as the IBL
    // source and rebake the irradiance + prefiltered cubes from it. Passing
    // nullptr or an empty path reverts to the procedural analytic sky.
    // Rebaking is a handful of blocking fullscreen passes (~100 ms), so call it
    // off the frame loop — at startup or on an explicit user action.
    // Returns false and KEEPS the current environment if the file won't decode.
    bool setEnvironment(const char* hdriPath);
    bool hasHdriEnvironment() const { return envIsHdri_; }
    // Basename of the loaded HDRI, or "analytic sky" when none is set.
    const std::string& environmentName() const { return envName_; }

    // Exposure in stops; the shader multiplies linear radiance by 2^EV.
    void  setExposureEV(float ev);
    float exposureEV() const { return exposureEV_; }

    enum class ToneCurve { Clamp = 0, PbrNeutral = 1, Aces = 2 };
    // Default is PbrNeutral — it preserves authored hue/saturation up to the
    // knee, which is what a "does this material look like it did in the
    // authoring tool" comparison needs. See shaders/tonemap.glsl.
    void      setToneCurve(ToneCurve c);
    ToneCurve toneCurve() const { return toneCurve_; }
    void      cycleToneCurve();
    const char* toneCurveName() const;

    // Extensions the loaded asset declares that this renderer doesn't implement
    // (issue #70). Non-empty means the model on screen differs from what its
    // author saw — the affected materials fall back to their base
    // metallic-roughness layer. Surfaced in the HUD so the difference is never
    // silently attributed to the renderer or the display.
    const std::vector<std::string>& unsupportedExtensions() const {
        return unsupportedExtensions_;
    }
    // "clearcoat, sheen, +3 more" — compact enough for a HUD line. Empty string
    // when the asset uses nothing we lack. Strips the "KHR_materials_" prefix.
    std::string unsupportedExtensionsSummary(size_t maxNamed = 2) const;

    // Advance the active animation clip by dtSeconds and refresh per-primitive
    // model matrices. No-op (static fast-path) when the model has no animation.
    // Call once per frame, before renderEye. Frozen while paused (the pose is
    // still recomputed, so a clip switch / pause shows the correct frame).
    void updateAnimation(float dtSeconds);

    // ── Playback control (Phase 4). All no-op without animations. ────────────
    void setActiveAnimation(int index);   // clamps/wraps; resets time + bind pose
    void cycleAnimation();                 // → next clip (wraps); no-op if <2 clips
    void togglePaused();
    bool isPaused() const { return paused_; }
    bool hasAnimations() const { return !animations_.empty(); }
    // Fills the active clip's status for the HUD; false when the model has no
    // clips. name = clip name, or "Clip <i>" when the glTF clip is unnamed.
    bool getPlaybackInfo(std::string& name, int& index, int& count,
                         float& time, float& duration, bool& playing) const;
    // ── Agent-facing read/seek accessors (XR_DXR_mcp_tools adoption). ───────
    int  animationCount() const { return (int)animations_.size(); }
    int  activeAnimation() const { return activeAnim_; }
    void setPaused(bool p) { paused_ = p; }
    // Clip name + duration by index; the name falls back to "Clip <i>" exactly
    // like getPlaybackInfo so list_animations and the HUD agree.
    bool getAnimationInfo(int index, std::string& name, float& duration) const {
        if (index < 0 || index >= (int)animations_.size()) return false;
        name = animations_[index].name.empty()
            ? ("Clip " + std::to_string(index)) : animations_[index].name;
        duration = animations_[index].duration;
        return true;
    }

    bool getSceneBBox(float outMin[3], float outMax[3]) const;
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;
    // Smoothed world-space centroid of the active skeleton (mean joint
    // position), updated by updateAnimation. Lets the platform bind the
    // display rig to a moving/skinned subject so it stays centered + at the
    // ZDP. Returns false for static / non-skinned models (no binding).
    bool getAnimatedAnchor(float out[3]) const;
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
        float mrParams[4];   // x=metallic, y=roughness, z=isSkinned(0/1), w=jointBase
        float emissive[4];   // rgb, w = index into the material-extension SSBO
    };

    // Per-material KHR_materials_* factors (set 0, binding 1; std430).
    //
    // These live in an SSBO rather than push constants because they don't fit:
    // the push block is already 112 of the 128 bytes Vulkan guarantees, and the
    // tier-1 extensions alone need ~18 floats. Indexing by material (passed in
    // PushBlock::emissive[3]) keeps it to one buffer and one binding no matter
    // how many extensions land later.
    //
    // Packing is dictated by std430 vec4 alignment — five vec4s, 80 bytes.
    struct MaterialExtGpu {
        float p0[4];   // ior, specularFactor, clearcoatFactor, clearcoatRoughness
        float p1[4];   // specularColorFactor.rgb, sheenRoughness
        float p2[4];   // sheenColorFactor.rgb, emissiveStrength
        float p3[4];   // reserved — anisotropy (strength, rotation), iridescence
        float p4[4];   // reserved — iridescence (factor, ior, thickness min/max)
    };
    // Set-0 uniform buffer (must match shaders/pbr.{vert,frag} + skybox.frag).
    struct UniformBlock {
        float viewProj[16];
        float view[16];        // Z-forward-adjusted view, for the foreground clip
        float cameraPos[4];
        float lightDir[4];     // .xyz = light direction, .w = clipFar (view-space; 0=off)
        float invViewProj[16]; // inverse(viewProj), for the skybox ray reconstruction
        float tone[4];         // x=exposure (2^EV), y=curve id, z=directional-light scale
    };

    bool createRenderTargets();
    bool ensureTargets(uint32_t w, uint32_t h);   // (re)create color+depth+framebuffer at this size
    bool createPipeline();
    bool createSamplerAndDefaults();
    bool createIbl();   // BRDF LUT + the env descriptor + the first cube bake
    bool bakeIblCubes();  // (re)generate irradiance + prefiltered cubes from the active environment
    bool createEnvDescriptor();          // set-0 sampler the generation passes read the HDRI from
    void bindEnvEquirect(VkImageView v); // point that descriptor at an image (HDRI or the 1x1 dummy)
    ModelImage uploadTexture(const struct ModelTexture& tex);
    VkDescriptorSet makeMaterialSet(VkImageView baseColor, VkImageView mr,
                                    VkImageView normal, VkImageView occ,
                                    VkImageView emissive);
    bool finalizeModel(struct ModelData& md);   // upload geometry+textures, build material sets
    // Override the load-time (bind-pose) AABB with one measured from the active
    // animation: sample the clip, skin the verts on the CPU, union the box. The
    // bind pose lives in mesh space, which a re-orienting skeleton (e.g. a Z-up
    // mesh stood up Y-up) renders very differently — so the bind box gives the
    // wrong height/center for the fit. No-op when there's no active clip.
    void recomputeAnimatedBounds(const std::vector<ModelVertex>& verts,
                                 const std::vector<uint32_t>& indices);
    // Re-blend morphed primitives (base + Σ weightᵢ·deltaᵢ) into the host-visible
    // vertex buffer using each owning node's current weights. No-op without morph.
    // trackAnchor → also accumulate the morphed verts' world centroid (rig bind).
    void blendMorphs(bool trackAnchor = false);
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

    // True when the swapchain is an sRGB *format* (set per-frame from the
    // swapchainFormat passed to renderEye). The shader gamma-encodes its output
    // ONLY when this is false: a UNORM swapchain (Windows) needs the shader to
    // linear→sRGB encode, while an sRGB swapchain (macOS) gets the encode for
    // free from the blit's hardware write — encoding in the shader too would
    // double-encode. See pbr.frag / skybox.frag (ubo.cameraPos.w flag).
    bool swapchainIsSrgb_ = false;

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
    // Host-visible MaterialExtGpu[]; rebuilt per model load. Always holds at
    // least one (all-default) entry so the binding is valid even for a model
    // with no materials at all.
    ModelBuffer materialExtBuffer_;
    uint32_t    materialExtCount_ = 0;
    bool uploadMaterialExtensions(const std::vector<ModelMaterial>& mats);

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
    // Render the active environment into each cube face/mip with the given
    // fragment SPIR-V. The push block is {face, roughness, envIsHdri}; the
    // generation passes bind envSet_ (the equirect HDRI) at set 0.
    bool genCubeMap(CubeMap& cube, uint32_t size, uint32_t mips,
                    const uint32_t* fragSpv, size_t fragSpvBytes, bool perMipRoughness);
    void destroyCubeMap(CubeMap& cube);
    ModelImage brdfLut_;                 // 2D R16G16_SFLOAT
    CubeMap irradianceCube_;
    CubeMap prefilterCube_;
    VkSampler iblCubeSampler_ = VK_NULL_HANDLE;
    VkSampler iblLutSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout iblSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool iblPool_ = VK_NULL_HANDLE;
    VkDescriptorSet iblSet_ = VK_NULL_HANDLE;

    // ── Environment source for the IBL bake (set = 0 of the generation passes) ─
    // envEquirect_ holds the loaded HDRI; when none is loaded it stays empty and
    // the descriptor points at envDummyTex_ (descriptors must be valid even
    // though the analytic-sky branch never samples it).
    ModelImage envEquirect_;
    ModelImage envDummyTex_;             // 1x1, bound when no HDRI is active
    VkSampler  envSampler_ = VK_NULL_HANDLE;   // REPEAT in u (equirect wraps), CLAMP in v
    VkDescriptorSetLayout envSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      envPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       envSet_ = VK_NULL_HANDLE;
    bool        envIsHdri_ = false;
    std::string envName_ = "analytic sky";

    // ── Grading (issue #70 phase 0) ──────────────────────────────────────────
    // +1 EV is not arbitrary: this viewer's fixed key light and analytic sky
    // produce a dim linear image, while PBR Neutral (like any filmic curve)
    // expects a scene exposed so mid-grey lands near 0.18 and highlights run
    // past 1.0. At EV 0 the scene never reaches the curve's shoulder, so the
    // curve only ever subtracts its 0.04 linear black point — all cost, no
    // highlight rolloff, measurably darker than no tone mapping at all
    // (helmet mean luma 55.6 clamped vs 46.6). Exposure and curve have to be
    // chosen together; this is that choice. Revisit against the phase 1
    // material grid rather than tuning by eye on one asset.
    float     exposureEV_ = 1.0f;
    ToneCurve toneCurve_  = ToneCurve::PbrNeutral;

    // Carried over from the loaded ModelData; cleared on every model load so it
    // always describes the asset currently on screen.
    std::vector<std::string> unsupportedExtensions_;

    // ── Skinning (set = 3: joint-matrix SSBO, vertex stage) ──────────────
    VkDescriptorSetLayout jointSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool jointPool_ = VK_NULL_HANDLE;       // recreated per model
    VkDescriptorSet jointSet_ = VK_NULL_HANDLE;
    ModelBuffer jointBuffer_;                            // host-visible mat4[] SSBO
    std::vector<ModelSkin> skins_;
    uint32_t jointCount_ = 0;                            // matrices in jointBuffer_

    // ── Morph targets (Phase 3: CPU blend into a host-visible vertex buffer) ─
    bool hasMorph_ = false;                  // → vertexBuffer_ is host-visible
    std::vector<ModelMorph>  morphs_;
    std::vector<ModelVertex> morphBase_;     // CPU base verts, re-blended per frame
    float morphCentroid_[3] = {0, 0, 0};     // raw world centroid of morphed verts
    bool  morphCentroidValid_ = false;       // (rig-bind fallback when no skeleton)

    // ── Loaded model GPU data ────────────────────────────────────────────
    ModelBuffer vertexBuffer_;
    ModelBuffer indexBuffer_;
    std::vector<ModelImage>     modelTextures_;
    std::vector<ModelMaterial>  materials_;
    std::vector<ModelPrimitive> primitives_;

    // ── Animation (Phase 1: node TRS). Empty graph → static fast-path ────────
    std::vector<ModelNode>  nodes_;
    std::vector<Animation>  animations_;
    std::vector<int>        rootNodes_;
    std::vector<float>      nodeWorld_;   // scratch: 16 floats/node, per-frame walk
    int   activeAnim_ = -1;              // -1 = none/static (fast-path guard)
    float animTime_   = 0.0f;            // playhead within the active clip (seconds)
    bool  paused_     = false;           // freeze the playhead (Phase 4 play/pause)
    std::vector<ModelNode> bindNodes_;   // bind-pose TRS snapshot; restored on clip switch

    // Display-rig bind: smoothed mean joint position (world space). Valid only
    // while a skinned model is animating; snaps on the first frame then eases.
    float animAnchor_[3] = {0, 0, 0};
    bool  animAnchorValid_ = false;

    // Correction added to the raw skeleton-centroid anchor so it lands on the
    // model's visual centre instead of the joint mean. = (animated AABB centre −
    // mean joint centroid), computed once over the clip in recomputeAnimatedBounds.
    // ~0 when the skeleton already spans the geometry (most glTF rigs); non-zero
    // when joint-free geometry sits off-centre (e.g. an FBX hat with no bones),
    // which would otherwise let the subject ride high/low in frame.
    float anchorOffset_[3] = {0, 0, 0};
    bool  anchorOffsetValid_ = false;

    float bboxMin_[3] = {0, 0, 0};
    float bboxMax_[3] = {0, 0, 0};
    bool  hasBBox_ = false;
};

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
// MV_TEX_SLOTS / MV_MAT_VEC4S. A GLSL file, included here on purpose: it holds
// nothing but #defines, and shaders/pbr.frag includes the same bytes. The
// material SSBO's stride is a function of both numbers, so they cannot be
// allowed to be written down twice. See the file's header and issue #81.
#include "shaders/material_slots.glsl"

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

    // ── Lighting mode (undocked-page parity) ─────────────────────────────────
    // Sky    the analytic procedural sky as an IBL environment plus one key
    //        light. The viewer's own look; unchanged.
    // Studio the three-point directional rig + hemisphere ambient the DisplayXR
    //        storefront's inline-3D tile lights the SAME asset with (three.js
    //        addStudioLights), with the sky IBL knocked down to a residual and
    //        no tone curve — so a model undocked from a page (displayxr-view:)
    //        keeps the materials it had in the tile. Metal is the case that
    //        makes this visible: crisp specular highlights off three lights
    //        read as metal, a smooth sky reflection reads as matte.
    // Room   the storefront page's CURRENT default: three.js RoomEnvironment
    //        baked to a PMREM and used as the whole light source - no punctual
    //        lights at all, NoToneMapping. Emulated analytically into this
    //        viewer's own IBL bake (shaders/room.glsl), so the same panel
    //        reflections travel across a metal body as the page shows.
    // None   no lights at all; the IBL ambient only.
    //
    // The GRADING is part of the mode, not an independent knob — see
    // setLightingMode() — so switching modes re-pins exposure + tone curve.
    // NoLights, not `None`: <X11/X.h> defines `None` as a macro (0L), and this
    // header is compiled on desktop Linux through the xlib window binding. The
    // CLI/protocol token stays "none" - only the enumerator is spelled around it.
    // Room is appended rather than inserted: the values are not persisted, but
    // a mode is quoted by NAME everywhere it is user-visible, and renumbering
    // Studio/NoLights would silently rewrite any hand-set integer in a capture
    // script. Cycle order is sky -> studio -> room -> none (see
    // cycleLightingMode), which is not the enumerator order.
    enum class LightingMode { Sky = 0, Studio = 1, NoLights = 2, Room = 3 };
    void         setLightingMode(LightingMode m);
    LightingMode lightingMode() const { return lightingMode_; }
    const char*  lightingModeName() const;
    void         cycleLightingMode();
    // "studio" / "sky" / "room" / "none" (the LaunchArgs::env vocabulary) -> a mode.
    // Returns false for anything else, leaving `out` untouched.
    static bool  parseLightingMode(const std::string& env, LightingMode& out);

    // Rasterisation samples actually in use (1 when the device or the
    // DXR_MODELVIEWER_MSAA override asked for no multisampling). Surfaced so
    // the platform layer can put it in the log - an AA claim that cannot be
    // read back is not a measurement.
    uint32_t sampleCount() const { return (uint32_t)samples_; }

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
                   float clipFarViewSpace = 0.0f,
                   //! This eye's projection built with an UNRESTRICTED far plane
                   //! (same fov, same near, far = ez + 1000·vH — what
                   //! dxr::ResolveClipPlanes returns for farOffsetVH = 1000).
                   //! The coverage pass projects with THIS, never with
                   //! projMatrix — see the coverage block below for why the
                   //! rasterizer's own far clip is the second clip #128 missed.
                   //! nullptr = "same as projMatrix", for the legs whose real
                   //! far plane is already unrestricted (macOS/Linux have no
                   //! transparent mode) and which never arm the mask.
                   const float projUnclipped[16] = nullptr);

    // ── Unclipped-silhouette coverage (#127 / displayxr-runtime#1470) ────────
    //
    // The `XR_DXR_depth_budget` content mask must describe the silhouette the
    // content WOULD have at an unrestricted rear budget. The rendered alpha
    // cannot: pbr.frag discards everything past `clipFarViewSpace`, so a mask
    // derived from it is a function of the budget the runtime published, and
    // the two feed each other into a ~0.6-1.1 s open/close oscillation
    // whenever the model straddles a text/blank border (runtime#1470).
    //
    // So the mask gets its own source. `beginContentMaskFrame(true)` arms a
    // coverage-only pass — same vertex shader, same skinning, same push
    // constants, same VIEW, into a small R8 target with
    // shaders/coverage.frag, which has no far-clip discard because it has no
    // code at all. Each following renderEye() unions its view's silhouette
    // into the accumulator, so after the frame's views are rendered
    // contentMaskCoverage() is the union-over-views UNCLIPPED silhouette,
    // ready for dxr::ContentMaskFromCoverage(). The click-through window
    // region keeps using the rendered (clipped) alpha — that one IS a visual
    // clip and must stay one.
    //
    // Costs one extra draw of the model's index buffer per view at
    // kContentMaskCovW x kContentMaskCovH with an empty fragment shader, and
    // one ~36 KB readback per view on the submit renderEye already waits on.
    // Disabled by default; only the transparent/borderless leg arms it.
    //
    // THERE WERE TWO CLIPS (#127 follow-up). Removing pbr.frag's discard fixed
    // only the fragment one. The GPU's fixed-function clip (NDC z > 1) removes
    // the rear half of the model before a fragment shader runs at all, and the
    // coverage pass inherited it by reusing the eye's REAL projection — whose
    // far plane IS the published rear budget (ez + farOffsetVH·vH, straight out
    // of dxr::ResolveClipPlanes). So the mask stayed a function of the budget
    // and kept oscillating against it. The pass therefore gets its OWN set-0
    // uniform buffer holding `projUnclipped * view`, written per view beside
    // the real one; nothing else about it differs (same view matrix, same
    // near plane, same viewport/scissor, no CPU-side frustum cull anywhere in
    // this renderer). depthClampEnable was rejected as the fix: it is an
    // optional device feature, and clamping to the far plane is not the same
    // artefact as "the silhouette it would have at an unrestricted budget".

    //! Coverage raster dimensions. Fixed, not aspect-matched to the viewport:
    //! the grid is mapped onto the window's client rect by normalised position
    //! (dxr::ContentMaskFromCoverage), so a different aspect is a pure linear
    //! stretch of the sampling lattice, not a displacement of the silhouette —
    //! and the runtime dilates the mask by its own disparity band before it
    //! measures anything, which is far coarser than the difference.
    static constexpr uint32_t kContentMaskCovW = 256;
    static constexpr uint32_t kContentMaskCovH = 144;

    //! Arm (or disarm) the coverage pass and clear the accumulator. Call once
    //! per frame BEFORE the frame's renderEye() calls.
    void beginContentMaskFrame(bool enable);

    //! The union-over-views unclipped coverage for the last armed frame: one
    //! byte per texel, nonzero = covered, row-major, top-left origin, tightly
    //! packed at kContentMaskCovW x kContentMaskCovH. nullptr when the pass is
    //! disarmed, unavailable (creation failed), or no view has been rendered
    //! into it yet. Valid until the next beginContentMaskFrame().
    const uint8_t* contentMaskCoverage() const;

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
    // Packing is dictated by std430 vec4 alignment: MV_MAT_VEC4S named lanes
    // followed by two MV_TEX_SLOTS-long vec4 arrays. Both counts come from
    // shaders/material_slots.glsl, which pbr.frag includes too, and the
    // static_assert below pins sizeof() to them — the layout is the ABI between
    // this struct and the shader, and #81 is what happens when it drifts.
    struct MaterialExtGpu {
        float p0[4];   // ior, specularFactor, clearcoatFactor, clearcoatRoughness
        float p1[4];   // specularColorFactor.rgb, sheenRoughness
        float p2[4];   // sheenColorFactor.rgb, emissiveStrength
        float p3[4];   // anisotropyStrength, anisotropyRotation, iridescenceFactor, iridescenceIor
        float p4[4];   // iridescenceThicknessMin/Max, transmissionFactor, volumeThickness
        float p5[4];   // attenuationColor.rgb, attenuationDistance (0 = none)
        float p6[4];   // scatterStrength, scatterAnisotropy (KHR_materials_scatter),
                       // diffuseRoughness (KHR_materials_diffuse_roughness),
                       // fuzzFactor (KHR_materials_fuzz)
        float p7[4];   // multiscatterColor.rgb, hasFuzz (0/1)
        float p8[4];   // coatIor, coatDarkening, coatAnisoStrength, coatAnisoRotation
        float p9[4];   // coatColor.rgb, hasCoat (0/1)   (KHR_materials_coat)
        // KHR_texture_transform, one entry per MaterialTexSlot:
        //   uvXf[i] = (offset.x, offset.y, scale.x, scale.y),  uvRot[i].x = radians
        // Identity (0,0,1,1 / 0) unless the asset supplied a transform.
        // MV_TEX_SLOTS, not MTS_COUNT/MTEX_COUNT: the shader spells the length
        // the same way, out of the same file. The enums are asserted equal to it
        // below, so all three move together or nothing compiles.
        float uvXf[MV_TEX_SLOTS][4];
        float uvRot[MV_TEX_SLOTS][4];
    };
    static_assert(sizeof(MaterialExtGpu) == 16 * (MV_MAT_VEC4S + 2 * MV_TEX_SLOTS),
                  "MaterialExtGpu no longer matches the std430 layout MatExt "
                  "declares in shaders/pbr.frag. uploadMaterialExtensions() "
                  "memcpys an array of these, so sizeof() IS the stride the "
                  "shader indexes by: if they disagree, material 0 still reads "
                  "correctly and every later material reads from the wrong "
                  "offset. That is issue #81's signature. Either a lane was "
                  "added without bumping MV_MAT_VEC4S, or the compiler inserted "
                  "padding this struct is not allowed to have.");
    // Set-0 uniform buffer (must match shaders/pbr.{vert,frag} + skybox.frag).
    struct UniformBlock {
        float viewProj[16];
        float view[16];        // Z-forward-adjusted view, for the foreground clip
        float cameraPos[4];
        float lightDir[4];     // .xyz = light direction, .w = clipFar (view-space; 0=off)
        float invViewProj[16]; // inverse(viewProj), for the skybox ray reconstruction
        // Every lane below has an owner. There are no spare lanes in this
        // block: #98 lost a device round to a probe that rode viewport.z on
        // the belief that it was padding — updateUniforms() overwrote it a few
        // lines later and the "probe" readback decoded the ordinary shaded
        // image. Before borrowing a lane, grep updateUniforms() AND the
        // shaders for it. Owners of every .w / partial lane today:
        //   cameraPos.w  = shader sRGB-encode flag (1 = UNORM swapchain)
        //   lightDir.w   = foreground clip, view-space far (0 = off)
        //   tone.w       = probe SELECT (0 none, 1 transmission #75, 2 facing #98)
        //   viewport.zw  = DXR_MODELVIEWER_KULLA_CONTY / _COAT_SPEC_HEMI
        //   studio.xyzw, studioFill.w, studioRim.w = the Studio rig
        //   hemiSky.w, hemiGround.w = unused (ub{} zeroes them; the only free lanes)
        float tone[4];         // x=exposure (2^EV), y=curve id, z=directional-light scale,
                               // w=probe select (0 = normal shading; see above)
        // The internal colour target is the size of the whole SWAPCHAIN IMAGE,
        // but each eye renders into only the top-left viewport of it. Scene-
        // colour sampling (transmission) therefore has to scale clip-space UVs
        // by the viewport's fraction of the image, or it reads past the region
        // that was actually rendered. x,y = viewport/image ratio;
        // z = DXR_MODELVIEWER_KULLA_CONTY, w = DXR_MODELVIEWER_COAT_SPEC_HEMI.
        float viewport[4];
        // ── Studio rig; mirrors the tail of pbr.frag's UBO. Zero (and dead in
        // the shader) in every mode but Studio.
        float studio[4];      // x=on, y=key intensity, z=hemi intensity, w=IBL scale
        float studioFill[4];  // xyz = world dir TO the light, w = intensity
        float studioRim[4];
        float hemiSky[4];     // rgb linear
        float hemiGround[4];  // rgb linear
    };

    bool createRenderTargets();
    bool ensureTargets(uint32_t w, uint32_t h);   // (re)create color+depth+framebuffer at this size
    // Pick samples_ once, at init: 4x if the device supports it for all three
    // attachment formats, else 1. DXR_MODELVIEWER_MSAA=<1|2|4|8> overrides.
    void chooseSampleCount();
    // One attachment image + view. Exists because the MSAA attachments need a
    // sample count and modelCreateImage2D has no parameter for one.
    bool createAttachment(ModelImage& img, uint32_t w, uint32_t h, VkFormat fmt,
                          VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                          VkImageAspectFlags aspect);
    bool createPipeline();
    bool createSamplerAndDefaults();
    bool createIbl();   // BRDF LUT + the env descriptor + the first cube bake
    // Which analytic environment the generation passes should integrate. An
    // HDRI, when one is loaded, outranks both - it is an explicit choice by the
    // person running the viewer, and the lighting mode is not.
    bool envUsesRoom() const { return !envIsHdri_ && lightingMode_ == LightingMode::Room; }
    bool bakeIblCubes();  // (re)generate irradiance + prefiltered cubes from the active environment
    bool createEnvDescriptor();          // set-0 sampler the generation passes read the HDRI from
    void bindEnvEquirect(VkImageView v); // point that descriptor at an image (HDRI or the 1x1 dummy)
    ModelImage uploadTexture(const struct ModelTexture& tex);
// Set 1 is one combined-image-sampler per material texture slot: the five core
    // glTF maps plus the texture-driven variants of the KHR_materials_* factors.
    // Passed as an array rather than 13 positional parameters — the order is the
    // binding order, and MaterialTexSlot names it.
    enum MaterialTexSlot {
        MTEX_BASE_COLOR = 0, MTEX_MR, MTEX_NORMAL, MTEX_OCCLUSION, MTEX_EMISSIVE,
        MTEX_CLEARCOAT, MTEX_CLEARCOAT_ROUGH, MTEX_SHEEN_COLOR, MTEX_SHEEN_ROUGH,
        MTEX_SPECULAR, MTEX_SPECULAR_COLOR, MTEX_TRANSMISSION, MTEX_THICKNESS,
        MTEX_SCATTER_STRENGTH, MTEX_MULTISCATTER_COLOR,
        MTEX_COAT_COLOR, MTEX_COAT_ANISOTROPY,
        MTEX_DIFFUSE_ROUGHNESS, MTEX_FUZZ, MTEX_COAT_NORMAL,
        MTEX_COUNT
    };
    static_assert((int)MTEX_COUNT == (int)MTS_COUNT,
                  "MaterialTexSlot must mirror ModelTexSlot: the loader fills "
                  "ModelMaterial::uvXf[] by ModelTexSlot index and the shader reads "
                  "it by binding index, so a divergence silently transforms the "
                  "wrong texture.");
    static_assert((int)MTEX_COUNT == MV_TEX_SLOTS,
                  "MTEX_COUNT and MV_TEX_SLOTS disagree. MV_TEX_SLOTS "
                  "(shaders/material_slots.glsl) is what pbr.frag sizes its UV "
                  "arrays with, so it, not the enum, sets the SSBO stride the "
                  "shader reads. Adding an enumerator without bumping it is "
                  "exactly the drift that produced #81.");

    VkDescriptorSet makeMaterialSet(const VkImageView views[MTEX_COUNT]);
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
    //! Build the set-0 UniformBlock for this view/projection and upload it.
    //! `dst` = nullptr writes the main camera UBO (uniformBuffer_); pass a
    //! different host-visible buffer to write a second, independently-projected
    //! copy — that is how the coverage pass gets its unrestricted far plane
    //! without disturbing the real pass (both are read by the SAME command
    //! buffer, so they cannot share one buffer).
    void updateUniforms(const float viewMatrix[16], const float projMatrix[16], float clipFar,
                        ModelBuffer *dst = nullptr);
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
    uint32_t vpWidth_ = 0;    // current eye's viewport within the target
    uint32_t vpHeight_ = 0;
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
    // Scene-linear radiance — a SECOND colour attachment written alongside
    // colorImage_ by every pass, holding what the shader had *before*
    // applyToneMapping and the conditional sRGB encode. Transmission samples
    // this, not colorImage_ (issue #75): colorImage_ is display-referred, so
    // compositing it into a pre-tone-map `color` sent the transmitted light
    // through the curve twice and — on a UNORM swapchain — through the sRGB
    // encode twice, which is what washed the glass out.
    //
    // 16F, not a packed small float: the acceptance test for #75 requires a
    // transmissive surface to reproduce the pixels behind it to within 1/255,
    // and B10G11R11_UFLOAT's ~6-bit mantissa (relative error ~1.5%) cannot
    // meet that. R16G16B16A16_SFLOAT is ~0.05% and is a Vulkan mandatory
    // format for every feature this needs (colour attachment, blit src/dst,
    // linear-filtered sampling), so no capability fallback is required.
    VkFormat sceneLinearFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    ModelImage colorImage_;
    ModelImage sceneLinearImage_;
    ModelImage depthImage_;

    // ── MSAA (issue #113) ────────────────────────────────────────────────────
    // The page this viewer undocks from renders with `antialias: true`; the
    // viewer rasterised at 1 sample, so its edges were visibly stepped beside
    // the page's. When samples_ > 1 the two colour attachments and the depth
    // buffer above are joined by multisampled twins that the subpass RESOLVES
    // into them, so colorImage_ / sceneLinearImage_ keep their single-sample
    // identity and every consumer downstream - the per-eye blit, the
    // transmission capture, the mip chain - is untouched.
    //
    // Note what this cannot fix: the display processor hard-masks the
    // SILHOUETTE alpha to 0/1, so a transparent window's outline against the
    // desktop stays hard however many samples are taken. Internal edges and
    // texture detail are what improve.
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    ModelImage colorMsaa_;         // only when samples_ > 1
    ModelImage sceneLinearMsaa_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    // Second pass over the same attachments with LOAD instead of CLEAR, for the
    // transmissive draws that have to come after the scene-colour capture.
    VkRenderPass renderPassLoad_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    // ── Unclipped-silhouette coverage pass (#127) ───────────────────────────
    // Created lazily on the first armed frame — an opaque-mode session never
    // pays for it. No depth attachment and no depth test: the artefact wanted
    // is the UNION of where geometry lands, for which occlusion is irrelevant,
    // and leaving depth out removes the only reason this pass would have to
    // track the main pass's sample count or target size.
    VkRenderPass  maskRenderPass_ = VK_NULL_HANDLE;
    VkPipeline    maskPipeline_ = VK_NULL_HANDLE;
    ModelImage    maskImage_;
    VkFramebuffer maskFramebuffer_ = VK_NULL_HANDLE;
    ModelBuffer   maskReadback_;          // host-visible, kCovW*kCovH bytes
    std::vector<uint8_t> maskCoverage_;   // CPU accumulator, OR-ed per view
    // The pass's OWN set-0: same layout as descriptorSet_, but binding 0 points
    // at maskUniform_ instead of uniformBuffer_ — the copy carrying
    // `projUnclipped * view`. Binding 1 (the material-extension SSBO) is
    // mirrored from the main set so the bound set is complete even though
    // coverage.frag never reads it.
    VkDescriptorPool maskDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet  maskSet_ = VK_NULL_HANDLE;
    ModelBuffer      maskUniform_;
    bool maskArmed_ = false;              // this frame wants the pass
    bool maskFailed_ = false;             // creation failed once; never retry
    bool maskHasView_ = false;            // at least one view accumulated
    // DXR_MODELVIEWER_MASKPASS_TEST=1 — arm the coverage pass unconditionally
    // and log the covered-texel count every ~60 frames. The pass is otherwise
    // reachable only from the Windows transparent/borderless leg, which is the
    // one leg that cannot be exercised on a macOS or Linux box; this makes the
    // Vulkan objects, the draw and the readback testable everywhere the
    // renderer runs at all.
    //
    // It is also a MUTATION test, not just a smoke test. Every armed view is
    // rasterised TWICE: once through the unrestricted projection the pass is
    // supposed to use (count U), and once through an artificially restricted
    // one whose far plane bisects the model's view-space depth range (count R).
    // U must be identical across frames and R must be strictly smaller — if R
    // ever equals U the far plane is not reaching the rasterizer and the test
    // is proving nothing, which is exactly the hole v0.28.5's self-test fell
    // into (clipFar was 0 in every frame it exercised, so both clips were off).
    // Diagnostics only — it changes no output.
    bool maskTestForce_ = false;
    uint32_t maskTestFrames_ = 0;
    VkDescriptorSet maskTestSet_ = VK_NULL_HANDLE;   // set 0 -> maskTestUniform_
    ModelBuffer     maskTestUniform_;
    // U is tracked PER TILE (the eyes see different silhouettes, so a single
    // min/max over all views would read as "varies" for a perfectly invariant
    // pass). maskTestTile_ counts views within the frame; beginContentMaskFrame
    // resets it.
    static constexpr uint32_t kMaskTestTiles = 8;
    uint32_t maskTestTile_ = 0;
    size_t maskTestUMin_[kMaskTestTiles];
    size_t maskTestUMax_[kMaskTestTiles];
    size_t maskTestRMin_ = (size_t)-1, maskTestRMax_ = 0;
    uint32_t maskTestViews_ = 0, maskTestRLtU_ = 0;
    bool ensureMaskPass();
    //! Point the coverage set(s)' binding 1 at the current material-extension
    //! SSBO. Called at creation and again from uploadMaterialExtensions, which
    //! reallocates that buffer on every model load.
    void refreshMaskMaterialBinding();
    void recordMaskPass(VkCommandBuffer cmd, VkDescriptorSet set0);
    void consumeMaskReadback();
    //! Nonzero texels currently in maskReadback_ (the LAST recorded coverage
    //! render), without touching the union accumulator. Test-side only.
    size_t countMaskReadback() const;
    void destroyMaskPass();

    // ── KHR_materials_transmission / _volume (issue #70 phase 2 tier 2) ──────
    // A mipped copy of the opaque pass's colour, which transmissive surfaces
    // refract against. This is the one extension whose cost is architectural
    // rather than shader-local: the copy happens once per renderEye, i.e. once
    // per view tile in the atlas, so it scales with view count — the reason
    // transmission was scoped as its own milestone.
    ModelImage transmissionImage_;
    uint32_t   transmissionMips_ = 1;
    VkSampler  transmissionSampler_ = VK_NULL_HANDLE;
    bool       hasTransmissive_ = false;   // any loaded material transmits
    // DXR_MODELVIEWER_TRANSMISSION_PROBE=1 — the issue #75 acceptance test.
    // Forces thickness 0 (sample straight behind the fragment) and mip 0, then
    // outputs the sampled scene radiance through the shader's own display
    // transform instead of shading. Every transmissive surface must then
    // reproduce the backdrop behind it and visually vanish; anything else means
    // the sample is in the wrong colour space or the UV mapping is wrong.
    // Reusing the shader's own tone/encode tail is deliberate — a probe with a
    // hand-copied display transform can drift away from the one it is testing.
    bool       transmissionProbe_ = false;
    // Front-face winding for the model pipeline (#98). COUNTER_CLOCKWISE (glTF)
    // on every platform; DXR_MODELVIEWER_FRONT_FACE=cw|ccw, or on Android the
    // property `debug.dxr.mv.frontface`, overrides it at init so a wrong call
    // is undone on a device without a rebuild. See createPipeline().
    VkFrontFace frontFace_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // DXR_MODELVIEWER_FACING_PROBE=1 (desktop env) / `debug.dxr.mv.facingprobe`
    // = 1 (Android property) — the #98 facing measurement. Read ONCE at init;
    // off, the renderer behaves byte-for-byte as without it.
    //
    // When frontFace_ disagrees with the asset's winding, gl_FrontFacing is
    // false on every visible fragment, pbr.frag's two-sided flip inverts N,
    // and the whole scene shades at grazing incidence (materials render as
    // environment mirrors). Two different faults produce that look — a wrong
    // winding constant, or an asset wound against its own normals — and only
    // the probe separates them, because it reports the raw pre-flip normal
    // alongside the shading normal. See pbr.frag for the encoding.
    //
    // On: ubo.tone.w = 2, shading is replaced by that encoding, the sky is
    // skipped, the colour clear is alpha 0 ("no geometry here"), and the FIRST
    // tile is read back whole and its mean logged, 1 frame in
    // kFacingProbeEvery. Mutually exclusive with transmissionProbe_ (same lane).
    bool       facingProbe_ = false;
    ModelBuffer facingProbeBuf_;           // host-visible readback staging
    uint32_t   facingProbeFrame_ = 0;      // tile-0 renders since the probe started
    uint32_t   facingProbePixels_ = 0;     // pixels in the pending readback
    static constexpr uint32_t kFacingProbeEvery = 120;
    void readFacingProbe();                // after renderEye's queue idle
    bool createTransmissionTarget(uint32_t w, uint32_t h);
    void captureSceneColor(VkCommandBuffer cmd, uint32_t w, uint32_t h);
    void writeIblSet();                    // (re)write set 2, incl. the transmission image

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
    ModelImage coatAnisoDefaultTex_;  // 1x1 (255,128,255) — coat aniso: dir (+1,0), strength 1
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
    // Sheen directional albedo E(N·V, sheenRoughness) — the table that lets
    // sheen redistribute energy instead of adding it (KHR_materials_sheen).
    ModelImage sheenLut_;
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
    LightingMode lightingMode_ = LightingMode::Sky;

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

// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Translucent hand mesh: spheres at the 26 XR_EXT_hand_tracking
 *         joints, capsules (cylinders between the joint spheres) along each
 *         bone, sized by the reported joint radii. Drawn through
 *         ModelRenderer's overlay hook — inside the renderer's own render
 *         pass, after the opaque model, depth test on / depth write off,
 *         alpha-blended — so it lands in every view's tile the same way the
 *         model does and respects the atlas loop.
 *
 * Vulkan only (the renderer is Vulkan on every desktop leg). The mesh is
 * rebuilt on the CPU once per frame into a 3-deep ring of host-visible
 * buffers, matching ModelRenderer::kFramesInFlight, so a frame still on the
 * GPU never sees the next frame's vertices.
 */
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "model_vulkan_utils.h"

namespace modelviewer {

struct HandRenderJoint {
    bool valid = false;
    float pos[3] = {0.f, 0.f, 0.f};   //!< world (the model's space)
    float radius = 0.01f;             //!< world units
};

struct HandRenderHand {
    bool active = false;              //!< tracked this frame (drives the fade)
    float color[3] = {0.5f, 0.8f, 1.f};
    HandRenderJoint joints[26];       //!< XR_EXT_hand_tracking joint order
};

class HandRenderer {
public:
    //! Pipeline against the renderer's render pass (its sample count and
    //! colour attachment count must match; ModelRenderer exposes both).
    bool init(VkDevice device, VkPhysicalDevice physDevice, VkRenderPass renderPass,
              VkSampleCountFlagBits samples, uint32_t colorAttachmentCount);
    void destroy();
    bool initialized() const { return pipeline_ != VK_NULL_HANDLE; }

    //! Once per app frame, BEFORE the frame's renderEye() calls: advance the
    //! fades by `dtSeconds` and rebuild the mesh into the next ring slot.
    void update(const HandRenderHand hands[2], float dtSeconds);

    //! Record this frame's mesh for one view. Called from the renderer's
    //! overlay hook with the view's matrices (column-major) and whether the
    //! attachment holds sRGB-encoded bytes (a UNORM swapchain) or linear.
    void draw(VkCommandBuffer cmd, const float view[16], const float proj[16], bool encodeSrgb);

    //! Anything visible this frame (a fading hand counts)?
    bool visible() const { return visible_; }
    //! Current fade of hand i (0 = gone, 1 = fully shown).
    float fade(int i) const { return fade_[i & 1]; }

    //! Outline hull extrusion as a fraction of the hand's smallest joint radius
    //! (hairline: ~0.6 mm on an 8 mm fingertip).
    static constexpr float kOutlineFraction = 0.08f;

private:
    struct Vertex { float p[3]; float n[3]; };
    struct Slot {
        ModelBuffer vb, ib;
        void* vbMapped = nullptr;
        void* ibMapped = nullptr;
        uint32_t indexCount[2] = {0, 0};
        uint32_t firstIndex[2] = {0, 0};
        float fade[2] = {0.f, 0.f};
        float outline[2] = {0.f, 0.f};   // world units
        float color[2][3] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    };
    static constexpr uint32_t kSlots = 3;
    // Capacity per slot, two hands: 25 bones x 2 rings of (kSeg+1) verts +
    // 26 joint spheres x (kRings+1) x (kSeg+1) verts, kSeg = 11, kRings = 6
    // (see the generator in the .cpp). Fits uint16 indices with room to spare.
    static constexpr uint32_t kMaxVerts = 2 * (25 * 24 + 26 * 84);
    static constexpr uint32_t kMaxIndices = 2 * (25 * 66 + 26 * 396);

    bool createPipelines(VkRenderPass renderPass, VkSampleCountFlagBits samples, uint32_t colorAttachmentCount);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    // Ghost-hand passes (Ultraleap GenericHandShader order): depth prime
    // (front faces, depth write, no colour) so the translucent body is drawn
    // once per pixel instead of once per overlapping capsule; body (front
    // faces, depth test only, blend); outline (BACK faces extruded along the
    // normals, blend) — the inverted hull that peeks past the primed silhouette.
    VkPipeline pipeline_ = VK_NULL_HANDLE;        // body
    VkPipeline pipelinePrime_ = VK_NULL_HANDLE;   // depth prime
    VkPipeline pipelineOutline_ = VK_NULL_HANDLE; // outline hull
    Slot slots_[kSlots];
    uint32_t slot_ = 0;
    float fade_[2] = {0.f, 0.f};
    bool visible_ = false;
    // One-shot placement diagnostic: the first frame with a valid wrist logs
    // it in world + view-0 clip space (draw()), so a field run proves whether
    // the hand is inside the frustum and in front of the model's near plane.
    float diagWrist_[3] = {0.f, 0.f, 0.f};
    int diagHand_ = -1;
    bool diagLogged_ = false;
};

} // namespace modelviewer

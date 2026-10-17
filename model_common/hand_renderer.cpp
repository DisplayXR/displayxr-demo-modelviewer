// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Translucent hand mesh renderer (see hand_renderer.h).
 */

#include "hand_renderer.h"

#include <cmath>
#include <cstring>

#include "mv_log.h"
#include "hand.vert.h"
#include "hand.frag.h"

namespace modelviewer {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Mesh resolution. Per hand: 25 bones x (2 rings of kSeg+1 verts) +
// 26 joints x ((kRings+1) x (kSeg+1)) verts -- see kMaxVerts in the header
// (25*24 + 26*84 verts, 25*66 + 26*396 indices with kSeg = 11, kRings = 6).
constexpr uint32_t kSeg = 11;     // around
constexpr uint32_t kRings = 6;    // sphere latitude bands

// XR_EXT_hand_tracking joint parents (-1 = root). PALM hangs off the wrist
// but gets no bone: it sits inside the wrist-to-metacarpal fan anyway.
constexpr int kParent[26] = {
    -1,                    // 0 PALM (sphere only)
    -1,                    // 1 WRIST
    1, 2, 3, 4,            // 2..5  THUMB metacarpal, proximal, distal, tip
    1, 6, 7, 8, 9,         // 6..10 INDEX metacarpal .. tip
    1, 11, 12, 13, 14,     // 11..15 MIDDLE
    1, 16, 17, 18, 19,     // 16..20 RING
    1, 21, 22, 23, 24,     // 21..25 LITTLE
};

struct PushBlock {
    float viewProj[16];
    float color[4];
    float eye[4];
    float depth[4];    // model projection's m[10], m[14] (see hand.frag)
    float params[4];   // x = outline extrusion (world units), y = pass (0 body, 1 outline)
};
static_assert(sizeof(PushBlock) == 128, "push block must stay within the 128-byte minimum");

// The hand's near plane as a fraction of the model's. The model clips at
// ez - vH (one virtual display height in front of the glass), which is where a
// hand between the panel and the viewer lives; the hand rasterises against a
// plane 50x nearer and remaps its depth into the model's range in hand.frag.
constexpr float kHandNearFraction = 0.02f;

VkShaderModule makeShader(VkDevice d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(d, &ci, nullptr, &m);
    return m;
}

void mat4Mul(float* out, const float* a, const float* b) {   // out = a * b, column-major
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                            a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    std::memcpy(out, r, sizeof(r));
}

//! Camera position from a rigid view matrix: -R^T t.
void cameraFromView(const float* v, float out[3]) {
    const float tx = v[12], ty = v[13], tz = v[14];
    out[0] = -(v[0] * tx + v[1] * ty + v[2] * tz);
    out[1] = -(v[4] * tx + v[5] * ty + v[6] * tz);
    out[2] = -(v[8] * tx + v[9] * ty + v[10] * tz);
}

struct MeshWriter {
    HandRenderer* owner;
    float* v;          // vertex floats (6 per vertex)
    uint16_t* i;
    uint32_t vcount = 0, icount = 0, vcap, icap;

    bool room(uint32_t nv, uint32_t ni) const { return vcount + nv <= vcap && icount + ni <= icap; }

    void vertex(const float p[3], const float n[3]) {
        float* d = v + (size_t)vcount * 6;
        d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = n[0]; d[4] = n[1]; d[5] = n[2];
        ++vcount;
    }
    void tri(uint32_t a, uint32_t b, uint32_t c) {
        i[icount++] = (uint16_t)a; i[icount++] = (uint16_t)b; i[icount++] = (uint16_t)c;
    }

    //! UV sphere, outward normals, CCW seen from outside.
    void sphere(const float c[3], float r) {
        if (!room((kRings + 1) * (kSeg + 1), kRings * kSeg * 6)) return;
        const uint32_t base = vcount;
        for (uint32_t y = 0; y <= kRings; ++y) {
            const float phi = kPi * (float)y / (float)kRings;            // 0 = top
            const float sy = std::sin(phi), cy = std::cos(phi);
            for (uint32_t x = 0; x <= kSeg; ++x) {
                const float th = 2.f * kPi * (float)x / (float)kSeg;
                const float n[3] = {sy * std::cos(th), cy, sy * std::sin(th)};
                const float p[3] = {c[0] + r * n[0], c[1] + r * n[1], c[2] + r * n[2]};
                vertex(p, n);
            }
        }
        for (uint32_t y = 0; y < kRings; ++y) {
            for (uint32_t x = 0; x < kSeg; ++x) {
                const uint32_t a = base + y * (kSeg + 1) + x;
                const uint32_t b = a + kSeg + 1;
                // With +Y up and theta increasing toward +Z from +X, the ring
                // runs clockwise seen from +Y; (a, a+1, b) is CCW from outside.
                tri(a, a + 1, b);
                tri(a + 1, b + 1, b);
            }
        }
    }

    //! Open cylinder from a to b (the ends sit inside the joint spheres).
    void bone(const float a[3], const float b[3], float r) {
        if (!room(2 * (kSeg + 1), kSeg * 6)) return;
        float axis[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (len < 1.0e-5f) return;
        for (float& f : axis) f /= len;
        // Any perpendicular pair.
        float u[3];
        if (std::fabs(axis[1]) < 0.9f) { u[0] = 0.f; u[1] = 1.f; u[2] = 0.f; }
        else                           { u[0] = 1.f; u[1] = 0.f; u[2] = 0.f; }
        float w[3] = {axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2], axis[0] * u[1] - axis[1] * u[0]};
        float wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        for (float& f : w) f /= wl;
        u[0] = w[1] * axis[2] - w[2] * axis[1];
        u[1] = w[2] * axis[0] - w[0] * axis[2];
        u[2] = w[0] * axis[1] - w[1] * axis[0];

        const uint32_t base = vcount;
        for (uint32_t end = 0; end < 2; ++end) {
            const float* c = end == 0 ? a : b;
            for (uint32_t x = 0; x <= kSeg; ++x) {
                const float th = 2.f * kPi * (float)x / (float)kSeg;
                const float ct = std::cos(th), st = std::sin(th);
                const float n[3] = {u[0] * ct + w[0] * st, u[1] * ct + w[1] * st, u[2] * ct + w[2] * st};
                const float p[3] = {c[0] + r * n[0], c[1] + r * n[1], c[2] + r * n[2]};
                vertex(p, n);
            }
        }
        for (uint32_t x = 0; x < kSeg; ++x) {
            const uint32_t a0 = base + x, a1 = a0 + 1;
            const uint32_t b0 = base + kSeg + 1 + x, b1 = b0 + 1;
            // (u, w, axis) is right-handed, so theta runs CCW seen from +axis
            // (from the far end). Outward CCW winding is then (a0, b0, a1).
            tri(a0, b0, a1);
            tri(a1, b0, b1);
        }
    }
};

} // namespace

bool HandRenderer::init(VkDevice device, VkPhysicalDevice physDevice, VkRenderPass renderPass,
                        VkSampleCountFlagBits samples, uint32_t colorAttachmentCount) {
    destroy();
    device_ = device;
    physDevice_ = physDevice;

    for (Slot& s : slots_) {
        s.vb = modelCreateBuffer(device_, physDevice_, (VkDeviceSize)kMaxVerts * sizeof(Vertex),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        s.ib = modelCreateBuffer(device_, physDevice_, (VkDeviceSize)kMaxIndices * sizeof(uint16_t),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (s.vb.buffer == VK_NULL_HANDLE || s.ib.buffer == VK_NULL_HANDLE ||
            vkMapMemory(device_, s.vb.memory, 0, VK_WHOLE_SIZE, 0, &s.vbMapped) != VK_SUCCESS ||
            vkMapMemory(device_, s.ib.memory, 0, VK_WHOLE_SIZE, 0, &s.ibMapped) != VK_SUCCESS) {
            MV_ERR("HandRenderer: buffer creation failed - hands not drawn\n");
            destroy();
            return false;
        }
    }
    if (!createPipelines(renderPass, samples, colorAttachmentCount)) {
        destroy();
        return false;
    }
    MV_LOG("HandRenderer: ready (%u-sample, %u colour attachment(s))\n", (unsigned)samples, colorAttachmentCount);
    return true;
}

bool HandRenderer::createPipelines(VkRenderPass renderPass, VkSampleCountFlagBits samples,
                                   uint32_t colorAttachmentCount) {
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushBlock)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) != VK_SUCCESS) return false;

    VkShaderModule vs = makeShader(device_, hand_vert_data, sizeof(hand_vert_data));
    VkShaderModule fs = makeShader(device_, hand_frag_data, sizeof(hand_frag_data));
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3},
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;   // dynamic, inherited from renderEye

    // Closed meshes with outward CCW winding (the renderer's Y flip keeps
    // GL-style CCW front faces CCW, #98). Body + prime cull the back; the
    // outline hull culls the FRONT (inverted hull).
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;

    // Sorted after the opaque model by construction (the hook runs last in
    // the pass): depth test against it. Only the prime pass writes depth —
    // the hand's own, so body/outline then pass only on the nearest hand
    // surface (LESS_OR_EQUAL against the primed value).
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Attachment 0: straight-alpha blend; alpha accumulates (ONE / 1-SRC_A)
    // so a transparent-background window keeps the hand's coverage.
    // Attachment 1 (the scene-linear copy transmission refracts): untouched.
    VkPipelineColorBlendAttachmentState cba[4] = {};
    if (colorAttachmentCount > 4) colorAttachmentCount = 4;
    cba[0].blendEnable = VK_TRUE;
    cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].colorBlendOp = VK_BLEND_OP_ADD;
    cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    for (uint32_t i = 1; i < colorAttachmentCount; ++i) cba[i].colorWriteMask = 0;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = colorAttachmentCount;
    cb.pAttachments = cba;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dyn;
    gp.layout = layout_; gp.renderPass = renderPass; gp.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);

    // Depth prime: same state, depth write on, every colour channel masked.
    if (r == VK_SUCCESS) {
        VkPipelineDepthStencilStateCreateInfo dsPrime = ds;
        dsPrime.depthWriteEnable = VK_TRUE;
        VkPipelineColorBlendAttachmentState cbaPrime[4] = {};
        VkPipelineColorBlendStateCreateInfo cbPrime = cb;
        cbPrime.pAttachments = cbaPrime;   // writeMask 0, blend off, everywhere
        VkGraphicsPipelineCreateInfo gpPrime = gp;
        gpPrime.pDepthStencilState = &dsPrime;
        gpPrime.pColorBlendState = &cbPrime;
        r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpPrime, nullptr, &pipelinePrime_);
    }
    // Outline hull: back faces only (the extrusion happens in the vertex shader).
    if (r == VK_SUCCESS) {
        VkPipelineRasterizationStateCreateInfo rsOut = rs;
        rsOut.cullMode = VK_CULL_MODE_FRONT_BIT;
        VkGraphicsPipelineCreateInfo gpOut = gp;
        gpOut.pRasterizationState = &rsOut;
        r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpOut, nullptr, &pipelineOutline_);
    }

    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) {
        MV_ERR("HandRenderer: pipeline creation failed (%d)\n", (int)r);
        return false;
    }
    return true;
}

void HandRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelinePrime_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelinePrime_, nullptr); pipelinePrime_ = VK_NULL_HANDLE; }
    if (pipelineOutline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelineOutline_, nullptr); pipelineOutline_ = VK_NULL_HANDLE; }
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    for (Slot& s : slots_) {
        if (s.vbMapped) { vkUnmapMemory(device_, s.vb.memory); s.vbMapped = nullptr; }
        if (s.ibMapped) { vkUnmapMemory(device_, s.ib.memory); s.ibMapped = nullptr; }
        if (s.vb.buffer != VK_NULL_HANDLE) modelDestroyBuffer(device_, s.vb);
        if (s.ib.buffer != VK_NULL_HANDLE) modelDestroyBuffer(device_, s.ib);
        s.indexCount[0] = s.indexCount[1] = 0;
    }
    device_ = VK_NULL_HANDLE;
    visible_ = false;
}

void HandRenderer::update(const HandRenderHand hands[2], float dtSeconds) {
    if (!initialized()) return;
    if (dtSeconds < 0.f) dtSeconds = 0.f;
    if (dtSeconds > 0.25f) dtSeconds = 0.25f;

    // Fade in fast (~0.15 s), out slower (~0.4 s) so a tracking blip does
    // not strobe the hand.
    for (int h = 0; h < 2; ++h) {
        const float target = hands[h].active ? 1.f : 0.f;
        const float rate = hands[h].active ? (1.f / 0.15f) : (1.f / 0.4f);
        float f = fade_[h];
        if (f < target) { f += rate * dtSeconds; if (f > target) f = target; }
        else if (f > target) { f -= rate * dtSeconds; if (f < target) f = target; }
        fade_[h] = f;
    }

    slot_ = (slot_ + 1) % kSlots;
    Slot& s = slots_[slot_];
    MeshWriter w{this, (float*)s.vbMapped, (uint16_t*)s.ibMapped, 0, 0, kMaxVerts, kMaxIndices};

    visible_ = false;
    for (int h = 0; h < 2; ++h) {
        s.firstIndex[h] = w.icount;
        s.indexCount[h] = 0;
        s.fade[h] = fade_[h];
        s.outline[h] = 0.f;
        std::memcpy(s.color[h], hands[h].color, sizeof(s.color[h]));
        if (fade_[h] <= 0.f) continue;
        const HandRenderHand& hand = hands[h];
        float minRadius = 0.f;
        for (int j = 0; j < 26; ++j) {
            const HandRenderJoint& jt = hand.joints[j];
            if (!jt.valid) continue;
            if (minRadius <= 0.f || jt.radius < minRadius) minRadius = jt.radius;
            w.sphere(jt.pos, jt.radius);
            const int p = kParent[j];
            if (p >= 0 && hand.joints[p].valid) {
                const float r = 0.85f * (jt.radius < hand.joints[p].radius ? jt.radius : hand.joints[p].radius);
                w.bone(hand.joints[p].pos, jt.pos, r);
            }
        }
        s.indexCount[h] = w.icount - s.firstIndex[h];
        s.outline[h] = minRadius * kOutlineFraction;
        if (s.indexCount[h] > 0) visible_ = true;
    }
}

void HandRenderer::draw(VkCommandBuffer cmd, const float view[16], const float proj[16], bool encodeSrgb) {
    if (!initialized() || !visible_) return;
    const Slot& s = slots_[slot_];

    // Own projection: same off-axis frustum (the x/y entries of a Vulkan
    // [0,1]-depth perspective matrix do not depend on near), near plane pulled
    // in. For that matrix form n = m14 / m10 and f = m14 / (1 + m10).
    float projHand[16];
    std::memcpy(projHand, proj, sizeof(projHand));
    const float m10 = proj[10], m14 = proj[14];
    if (m10 != 0.f && (1.f + m10) != 0.f) {
        const float n = m14 / m10;
        const float f = m14 / (1.f + m10);
        if (n > 0.f && f > n) {
            const float nh = n * kHandNearFraction;
            projHand[10] = f / (nh - f);
            projHand[14] = f * nh / (nh - f);
        }
    }

    PushBlock pb{};
    mat4Mul(pb.viewProj, projHand, view);
    cameraFromView(view, pb.eye);
    pb.eye[3] = encodeSrgb ? 1.f : 0.f;
    pb.depth[0] = m10;
    pb.depth[1] = m14;

    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &s.vb.buffer, &off);
    vkCmdBindIndexBuffer(cmd, s.ib.buffer, 0, VK_INDEX_TYPE_UINT16);

    // Three passes over both hands, in the Ghost Hand order: depth prime,
    // body, outline. Priming BOTH hands first means two overlapping hands
    // also resolve to one nearest surface per pixel.
    const VkPipeline passes[3] = {pipelinePrime_, pipeline_, pipelineOutline_};
    for (int pass = 0; pass < 3; ++pass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, passes[pass]);
        for (int h = 0; h < 2; ++h) {
            if (s.indexCount[h] == 0) continue;
            pb.color[0] = s.color[h][0]; pb.color[1] = s.color[h][1]; pb.color[2] = s.color[h][2];
            pb.color[3] = s.fade[h];
            pb.params[0] = pass == 2 ? s.outline[h] : 0.f;
            pb.params[1] = pass == 2 ? 1.f : 0.f;
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushBlock), &pb);
            vkCmdDrawIndexed(cmd, s.indexCount[h], 1, s.firstIndex[h], 0, 0);
        }
    }
}

} // namespace modelviewer

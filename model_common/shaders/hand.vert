#version 450
// Translucent hand mesh (joint spheres + bone capsules), drawn after the
// opaque model inside the same render pass. Clean +Y-up world coordinates;
// the renderer's negative-height viewport does the Vulkan Y flip, exactly as
// for pbr.vert.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(push_constant) uniform PushBlock {
    mat4 viewProj;   // handProj * view — the MODEL projection with a much nearer near plane
    vec4 color;      // rgb = hand tint (linear), a = base alpha (fade applied)
    vec4 eye;        // xyz = camera position (world), w = 1 -> sRGB-encode output
    vec4 depth;      // xy = the MODEL projection's (m[10], m[14]); hand.frag remaps depth with them
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vToEye;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    vNormal = inNormal;
    vToEye = pc.eye.xyz - inPos;
}

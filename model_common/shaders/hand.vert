#version 450
// Translucent "ghost" hand (joint spheres + bone capsules), drawn after the
// opaque model inside the same render pass in three passes — depth prime,
// body, outline — see hand_renderer.cpp. Clean +Y-up world coordinates; the
// renderer's negative-height viewport does the Vulkan Y flip, as for pbr.vert.
//
// Look modelled on the Ultraleap Ghost Hand material (the Ultraleap Unity
// plugin's Ultraleap/Tracking/Hands/Runtime/Models/GenericHand/
// GenericHandShader.shader + "Ghost Hand.mat", Apache-2.0): the outline pass extrudes
// the mesh along its normals and is drawn back-face-only (inverted hull).

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(push_constant) uniform PushBlock {
    mat4 viewProj;   // handProj * view — the MODEL projection with a much nearer near plane
    vec4 color;      // rgb = hand tint (linear), a = fade [0,1]
    vec4 eye;        // xyz = camera position (world), w = 1 -> sRGB-encode output
    vec4 depth;      // xy = the MODEL projection's (m[10], m[14]); hand.frag remaps depth with them
    vec4 params;     // x = outline extrusion (world units, 0 = none), y = pass (0 body, 1 outline)
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vToEye;

void main() {
    vec3 p = inPos + inNormal * pc.params.x;
    gl_Position = pc.viewProj * vec4(p, 1.0);
    vNormal = inNormal;
    vToEye = pc.eye.xyz - p;
}

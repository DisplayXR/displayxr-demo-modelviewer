// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU skinning of a rigged hand mesh from the 26 XR_EXT_hand_tracking
 *         joints: the bone-name -> joint map, the bone frames, and the
 *         per-vertex blend. Pure C++ (no Vulkan, no OpenXR, no tinygltf), so
 *         tests/hand_skin_tests.cpp checks it on every CI leg.
 *
 * The mesh is the Ultraleap Unity plug-in's GenericHand (the "Ghost Hands"
 * mesh), converted to glTF by scripts/convert_ultraleap_hand.py and embedded
 * in the binary (assets/hands/, THIRD_PARTY.md). It is a LEFT hand; the right
 * one is its mirror image (HandSkinMirror), exactly as the reference plug-in
 * builds it.
 *
 * How the rig is driven (HandSkinComputeFrames):
 *   - Every rig bone maps to ONE XR joint by name (Leap naming: Wrist, Palm,
 *     <finger>_meta / _a / _b / _c / _end; the thumb has no _c). The bone's
 *     head IS that joint, so the bind-space joint positions come from the
 *     bones' inverse bind matrices, and the XR palm joint (0) drives nothing.
 *   - A frame per joint is built the same way twice, once from the bind
 *     positions (rest) and once from the tracked ones: the root (wrist) frame
 *     looks along wrist -> middle knuckle with the back of the hand as "up"
 *     (the tracked wrist's +Y when its orientation is valid, else the palm
 *     plane's normal); each finger bone looks along joint -> next joint and
 *     takes its roll from the knuckle line (index - little proximal joint),
 *     the axis a finger curls about, so a curl is exact and a curled finger
 *     never goes degenerate; the thumb, which can point along that line,
 *     instead swings the wrist frame onto its own direction (minimal rotation).
 *   - Bone matrix = T(tracked joint) * R_tracked * S * R_rest^T * T(-rest joint),
 *     S = the hand's scale across the bone (wrist -> middle knuckle length
 *     ratio; the joints arrive in rig units that scale with the window) and
 *     the bone's own length ratio along it, so every bone head lands exactly
 *     on its tracked joint and the fingertips reach the tracked tips.
 *   - Limitation: finger and thumb TWIST about their own axis is not
 *     tracked (only positions drive the bones below the wrist); for the
 *     GenericHand's round fingers this is invisible.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace modelviewer {

constexpr int kHandSkinJointCount = 26;   //!< XR_HAND_JOINT_COUNT_EXT
constexpr int kHandSkinMaxBones = 64;

//! A skinned hand mesh in its bind pose (glTF skin semantics: positions are
//! in the space the inverse bind matrices map from).
struct HandSkinMesh {
    std::vector<float> pos;        //!< 3 per vertex
    std::vector<float> nrm;        //!< 3 per vertex
    std::vector<float> alpha;      //!< 1 per vertex (the reference's transparency map, baked)
    std::vector<uint8_t> joints;   //!< 4 per vertex, skin joint (bone) index
    std::vector<float> weights;    //!< 4 per vertex, sum 1
    std::vector<uint16_t> indices; //!< triangle list, CCW = outside
    std::vector<int> boneXr;       //!< per bone: XR joint driving it (-1 = none)
    std::vector<std::string> boneNames;
    float restJoint[kHandSkinJointCount][3] = {};
    bool restValid[kHandSkinJointCount] = {};
    bool leftHand = true;

    size_t vertexCount() const { return pos.size() / 3; }
};

//! XR joint for a rig bone name ("L_index_a" -> INDEX_PROXIMAL, "R_Palm" ->
//! WRIST, ...), -1 if the name is not a hand bone.
int HandSkinXrJointForBone(const std::string& name);

//! Fill boneXr from boneNames and restJoint from the bind-space bone heads
//! (`boneHeads`, 3 floats per bone). Fails (with `err`) if a joint the frames
//! need has no bone, or a bone influences vertices but maps to nothing.
bool HandSkinFinalize(HandSkinMesh& mesh, const std::vector<float>& boneHeads, std::string* err);

//! The mirror image across x = 0 (positions, normals, rest joints negated in
//! x, triangle winding flipped so CCW stays outside): left <-> right.
HandSkinMesh HandSkinMirror(const HandSkinMesh& mesh);

//! Tracked joint, in the space the mesh is to be drawn in.
struct HandSkinJointIn {
    bool valid = false;
    float pos[3] = {0.f, 0.f, 0.f};
    bool orientValid = false;
    float orient[4] = {0.f, 0.f, 0.f, 1.f};   //!< x, y, z, w (XR_EXT_hand_tracking axes: +Y = back of the hand)
};

//! One bone transform: affine (3x3 column-major linear part + translation)
//! and the matching normal matrix.
struct HandSkinBone {
    float lin[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float trans[3] = {0.f, 0.f, 0.f};
    float nrm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

//! Per-XR-joint bone transforms for a tracked hand. False (out untouched)
//! when a joint the frames need is missing or degenerate.
bool HandSkinComputeFrames(const HandSkinMesh& mesh, const HandSkinJointIn joints[kHandSkinJointCount],
                           HandSkinBone out[kHandSkinJointCount]);

//! Linear-blend skin every vertex. Writes position (3 floats) and unit normal
//! (3 floats) at `dst + v * strideFloats`.
void HandSkinApply(const HandSkinMesh& mesh, const HandSkinBone frames[kHandSkinJointCount],
                   float* dst, size_t strideFloats);

//! The rest pose's "back of the hand" direction (what a tracked wrist's +Y
//! is matched against). Test aid.
void HandSkinRestDorsal(const HandSkinMesh& mesh, float out[3]);

//! Load the embedded rigged hand (.glb bytes): one skinned mesh, bone names
//! as above. Implemented in hand_skin_gltf.cpp (tinygltf).
bool HandSkinLoadGlb(const uint8_t* data, size_t size, HandSkinMesh& out, std::string* err);

} // namespace modelviewer

// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Self-check for hand_skin.h against the embedded Ultraleap hand:
 *         the bone-name -> XR joint map, that the rest pose skins to the bind
 *         mesh, that a rigid move / uniform scale of the joints moves the mesh
 *         rigidly (wrist orientation path included), that a finger curl keeps
 *         its phalanx rigid, and that the mirrored right hand stays outward-
 *         wound. No OpenXR runtime, no Vulkan: exit 0 = every case passed.
 */

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "hand_skin.h"
#include "hand_mesh_glb.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace modelviewer;

namespace {

int g_failures = 0;
int g_checks = 0;
constexpr float kPi = 3.14159265358979323846f;

void Check(bool cond, const std::string& what) {
    ++g_checks;
    std::printf("  %s  %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

struct Xf {   // p' = s * R p + t, R column-major
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float t[3] = {0, 0, 0};
    float s = 1.f;
    void apply(const float* p, float* o) const {
        for (int r = 0; r < 3; ++r) o[r] = s * (R[r] * p[0] + R[3 + r] * p[1] + R[6 + r] * p[2]) + t[r];
    }
    void rot(const float* p, float* o) const {
        for (int r = 0; r < 3; ++r) o[r] = R[r] * p[0] + R[3 + r] * p[1] + R[6 + r] * p[2];
    }
};

//! Rotation about unit axis (x, y, z) by angle a.
Xf AxisAngle(float x, float y, float z, float a) {
    Xf f;
    const float c = std::cos(a), s = std::sin(a), C = 1.f - c;
    const float m[3][3] = {{c + x * x * C, x * y * C - z * s, x * z * C + y * s},
                           {y * x * C + z * s, c + y * y * C, y * z * C - x * s},
                           {z * x * C - y * s, z * y * C + x * s, c + z * z * C}};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) f.R[col * 3 + r] = m[r][col];
    return f;
}

//! Quaternion (x, y, z, w) of a column-major rotation matrix.
void QuatFromR(const float* R, float q[4]) {
    const float m00 = R[0], m11 = R[4], m22 = R[8];
    const float m21 = R[5], m12 = R[7], m02 = R[6], m20 = R[2], m10 = R[1], m01 = R[3];
    const float tr = m00 + m11 + m22;
    if (tr > 0.f) {
        const float s = std::sqrt(tr + 1.f) * 2.f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
}

void RestJoints(const HandSkinMesh& m, const Xf& xf, HandSkinJointIn in[kHandSkinJointCount]) {
    for (int j = 0; j < kHandSkinJointCount; ++j) {
        in[j] = HandSkinJointIn{};
        in[j].valid = m.restValid[j];
        if (in[j].valid) xf.apply(m.restJoint[j], in[j].pos);
    }
}

std::vector<float> Skin(const HandSkinMesh& m, const HandSkinJointIn in[kHandSkinJointCount], bool* ok) {
    HandSkinBone bones[kHandSkinJointCount];
    *ok = HandSkinComputeFrames(m, in, bones);
    std::vector<float> out(m.vertexCount() * 6, 0.f);
    if (*ok) HandSkinApply(m, bones, out.data(), 6);
    return out;
}

//! Max position / normal error of `skinned` against xf(bind).
void Compare(const HandSkinMesh& m, const std::vector<float>& skinned, const Xf& xf, float* posErr, float* nrmErr) {
    *posErr = 0.f; *nrmErr = 0.f;
    for (size_t v = 0; v < m.vertexCount(); ++v) {
        float p[3], n[3];
        xf.apply(&m.pos[v * 3], p);
        xf.rot(&m.nrm[v * 3], n);
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; ++k) {
            *posErr = std::fmax(*posErr, std::fabs(skinned[v * 6 + k] - p[k]));
            *nrmErr = std::fmax(*nrmErr, std::fabs(skinned[v * 6 + 3 + k] - n[k] / nl));
        }
    }
}

//! Fraction of triangles whose winding normal agrees with their vertex normals.
float OutwardFraction(const HandSkinMesh& m) {
    size_t agree = 0, total = 0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const float* a = &m.pos[m.indices[t] * 3];
        const float* b = &m.pos[m.indices[t + 1] * 3];
        const float* c = &m.pos[m.indices[t + 2] * 3];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float g[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float n[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 3; ++r) n[r] += m.nrm[m.indices[t + k] * 3 + r];
        ++total;
        if (g[0] * n[0] + g[1] * n[1] + g[2] * n[2] > 0.f) ++agree;
    }
    return total ? (float)agree / (float)total : 0.f;
}

void TestBoneNames() {
    std::printf("bone name -> XR joint\n");
    Check(HandSkinXrJointForBone("L_Wrist") == 1, "L_Wrist -> WRIST");
    Check(HandSkinXrJointForBone("R_Palm") == 1, "R_Palm -> WRIST (rigid with it)");
    Check(HandSkinXrJointForBone("L_thumb_meta") == 2, "thumb_meta -> THUMB_METACARPAL");
    Check(HandSkinXrJointForBone("L_thumb_b") == 4, "thumb_b -> THUMB_DISTAL");
    Check(HandSkinXrJointForBone("R_thumb_end") == 5, "thumb_end -> THUMB_TIP");
    Check(HandSkinXrJointForBone("L_index_a") == 7, "index_a -> INDEX_PROXIMAL");
    Check(HandSkinXrJointForBone("L_middle_c") == 14, "middle_c -> MIDDLE_DISTAL");
    Check(HandSkinXrJointForBone("L_ring_meta") == 16, "ring_meta -> RING_METACARPAL");
    Check(HandSkinXrJointForBone("L_pinky_end") == 25, "pinky_end -> LITTLE_TIP");
    Check(HandSkinXrJointForBone("L_thumb_c") == -1, "the thumb has no _c");
    Check(HandSkinXrJointForBone("Armature") == -1, "non-bone -> -1");
}

void TestAsset(const HandSkinMesh& left) {
    std::printf("embedded asset\n");
    Check(left.vertexCount() > 500 && left.vertexCount() < 8000, "a few thousand vertices at most (" + std::to_string(left.vertexCount()) + ")");
    Check(left.indices.size() % 3 == 0 && !left.indices.empty(), "triangle list");
    Check(left.leftHand, "it is a left hand");
    int mapped = 0;
    for (int b : left.boneXr) mapped += b >= 0;
    Check(mapped == (int)left.boneXr.size(), "every bone maps to an XR joint (" + std::to_string(mapped) + ")");
    int rest = 0;
    for (int j = 1; j < kHandSkinJointCount; ++j) rest += left.restValid[j];
    Check(rest == 25, "rest position for all 25 non-palm joints");
    float minA = 1.f, maxA = 0.f;
    for (float a : left.alpha) { minA = std::fmin(minA, a); maxA = std::fmax(maxA, a); }
    Check(minA < 0.1f && maxA > 0.9f, "baked transparency fades the wrist cut, keeps the hand opaque");
    Check(OutwardFraction(left) > 0.95f, "CCW winding faces out");
    float dorsal[3];
    HandSkinRestDorsal(left, dorsal);
    Check(dorsal[2] > 0.9f, "rest back-of-hand = +z (fingers curl toward -z)");
}

void TestRestIsBind(const HandSkinMesh& m, const char* side) {
    std::printf("rest pose skins to the bind mesh (%s)\n", side);
    HandSkinJointIn in[kHandSkinJointCount];
    Xf id;
    RestJoints(m, id, in);
    bool ok;
    const std::vector<float> s = Skin(m, in, &ok);
    Check(ok, "frames built");
    float pe, ne;
    Compare(m, s, id, &pe, &ne);
    Check(pe < 1.0e-5f, "position error " + std::to_string(pe) + " m");
    Check(ne < 1.0e-4f, "normal error " + std::to_string(ne));
}

void TestRigid(const HandSkinMesh& m, bool useOrientation) {
    std::printf("rigid move + scale %s\n", useOrientation ? "(wrist orientation drives the roll)" : "(palm plane drives the roll)");
    Xf xf = AxisAngle(0.267f, 0.535f, 0.802f, 1.9f);
    xf.t[0] = 0.3f; xf.t[1] = -1.2f; xf.t[2] = 2.5f;
    xf.s = 3.5f;   // rig units: the joints scale with the window
    HandSkinJointIn in[kHandSkinJointCount];
    RestJoints(m, xf, in);
    if (useOrientation) {
        // Wrist orientation whose +Y is the moved rest back-of-hand direction.
        float d[3];
        HandSkinRestDorsal(m, d);
        // Rotation taking +Y onto d, then xf's rotation.
        const float c = d[1];
        float axis[3] = {d[2], 0.f, -d[0]};   // (0,1,0) x d
        const float sl = std::sqrt(axis[0] * axis[0] + axis[2] * axis[2]);
        Xf toD = sl > 1e-6f ? AxisAngle(axis[0] / sl, 0.f, axis[2] / sl, std::atan2(sl, c)) : Xf{};
        float R[9];
        for (int col = 0; col < 3; ++col) {
            float e[3] = {toD.R[col * 3], toD.R[col * 3 + 1], toD.R[col * 3 + 2]};
            xf.rot(e, &R[col * 3]);
        }
        in[1].orientValid = true;
        QuatFromR(R, in[1].orient);
    }
    bool ok;
    const std::vector<float> s = Skin(m, in, &ok);
    Check(ok, "frames built");
    float pe, ne;
    Compare(m, s, xf, &pe, &ne);
    Check(pe < 1.0e-4f, "position error " + std::to_string(pe) + " (units of a 3.5x hand)");
    Check(ne < 1.0e-3f, "normal error " + std::to_string(ne));
}

void TestCurl(const HandSkinMesh& m) {
    std::printf("index curl keeps the distal phalanx rigid\n");
    HandSkinJointIn in[kHandSkinJointCount];
    Xf id;
    RestJoints(m, id, in);
    // Bend the index at its proximal joint (7) by 70 degrees about the
    // palm's lateral axis (index - little knuckle), carrying 8, 9, 10.
    const float* p7 = m.restJoint[7];
    float lat[3] = {m.restJoint[7][0] - m.restJoint[22][0], m.restJoint[7][1] - m.restJoint[22][1],
                    m.restJoint[7][2] - m.restJoint[22][2]};
    const float l = std::sqrt(lat[0] * lat[0] + lat[1] * lat[1] + lat[2] * lat[2]);
    const Xf rot = AxisAngle(lat[0] / l, lat[1] / l, lat[2] / l, 70.f * kPi / 180.f);
    for (int j = 8; j <= 10; ++j) {
        const float d[3] = {m.restJoint[j][0] - p7[0], m.restJoint[j][1] - p7[1], m.restJoint[j][2] - p7[2]};
        float r[3];
        rot.rot(d, r);
        for (int k = 0; k < 3; ++k) in[j].pos[k] = p7[k] + r[k];
    }
    bool ok;
    const std::vector<float> s = Skin(m, in, &ok);
    Check(ok, "frames built");
    // Vertices fully on the index distal bone (XR 9) must move exactly like
    // the rotated finger: rot about p7.
    int bone9 = -1;
    for (size_t b = 0; b < m.boneXr.size(); ++b)
        if (m.boneXr[b] == 9) bone9 = (int)b;
    // Rigid = distances to the bone's two ends (new joints 9 and 10) kept,
    // and exactly the ideal rotation (the curl axis is the knuckle line).
    float distErr = 0.f, err = 0.f;
    int n = 0, moved = 0;
    for (size_t v = 0; v < m.vertexCount(); ++v) {
        if (m.joints[v * 4] != bone9 || m.weights[v * 4] < 0.999f) continue;
        const float d[3] = {m.pos[v * 3] - p7[0], m.pos[v * 3 + 1] - p7[1], m.pos[v * 3 + 2] - p7[2]};
        float r[3];
        rot.rot(d, r);
        float dist = 0.f;
        for (int k = 0; k < 3; ++k) {
            err = std::fmax(err, std::fabs(s[v * 6 + k] - (p7[k] + r[k])));
            dist += (s[v * 6 + k] - m.pos[v * 3 + k]) * (s[v * 6 + k] - m.pos[v * 3 + k]);
        }
        for (int j = 9; j <= 10; ++j) {
            float a = 0.f, b = 0.f;
            for (int k = 0; k < 3; ++k) {
                a += (s[v * 6 + k] - in[j].pos[k]) * (s[v * 6 + k] - in[j].pos[k]);
                b += (m.pos[v * 3 + k] - m.restJoint[j][k]) * (m.pos[v * 3 + k] - m.restJoint[j][k]);
            }
            distErr = std::fmax(distErr, std::fabs(std::sqrt(a) - std::sqrt(b)));
        }
        if (std::sqrt(dist) > 0.01f) ++moved;
        ++n;
    }
    Check(n > 10, std::to_string(n) + " vertices ride the distal bone alone");
    Check(moved == n, "all of them moved (> 1 cm)");
    Check(distErr < 1.0e-4f, "rigid: distances to the bone's joints kept (error " + std::to_string(distErr) + " m)");
    Check(err < 1.0e-4f, "they follow the curled finger exactly (error " + std::to_string(err) + " m)");
}

//! Optional eyeball aid: with HAND_SKIN_TESTS_OBJ_DIR set, write the skinned
//! mesh of a few poses as .obj (render them in any viewer). No checks.
void RotateAbout(HandSkinJointIn in[kHandSkinJointCount], int pivot, int first, int last, const float* axis, float a) {
    const Xf r = AxisAngle(axis[0], axis[1], axis[2], a);
    const float* p = in[pivot].pos;
    for (int j = first; j <= last; ++j) {
        const float d[3] = {in[j].pos[0] - p[0], in[j].pos[1] - p[1], in[j].pos[2] - p[2]};
        float o[3];
        r.rot(d, o);
        for (int k = 0; k < 3; ++k) in[j].pos[k] = p[k] + o[k];
    }
}

void WriteObj(const HandSkinMesh& m, const HandSkinJointIn in[kHandSkinJointCount], const std::string& path) {
    bool ok;
    const std::vector<float> s = Skin(m, in, &ok);
    if (!ok) return;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (size_t v = 0; v < m.vertexCount(); ++v)
        std::fprintf(f, "v %f %f %f %f %f %f\n", s[v * 6], s[v * 6 + 1], s[v * 6 + 2], m.alpha[v], m.alpha[v], m.alpha[v]);
    for (size_t v = 0; v < m.vertexCount(); ++v) std::fprintf(f, "vn %f %f %f\n", s[v * 6 + 3], s[v * 6 + 4], s[v * 6 + 5]);
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
        std::fprintf(f, "f %d//%d %d//%d %d//%d\n", m.indices[t] + 1, m.indices[t] + 1, m.indices[t + 1] + 1,
                     m.indices[t + 1] + 1, m.indices[t + 2] + 1, m.indices[t + 2] + 1);
    std::fclose(f);
    std::printf("  wrote %s\n", path.c_str());
}

void DumpPoses(const HandSkinMesh& m, const std::string& dir, const char* side) {
    HandSkinJointIn in[kHandSkinJointCount];
    Xf id;
    RestJoints(m, id, in);
    WriteObj(m, in, dir + "/" + side + "_rest.obj");
    // Fist: every finger curls 50 degrees at each of its three joints about
    // the knuckle line; the thumb folds 35 degrees twice about the palm normal.
    float lat[3] = {in[7].pos[0] - in[22].pos[0], in[7].pos[1] - in[22].pos[1], in[7].pos[2] - in[22].pos[2]};
    float l = std::sqrt(lat[0] * lat[0] + lat[1] * lat[1] + lat[2] * lat[2]);
    for (float& c : lat) c /= l;
    const float sgn = m.leftHand ? -1.f : 1.f;   // positive curl = toward the palm
    const float ax[3] = {lat[0] * sgn, lat[1] * sgn, lat[2] * sgn};
    for (int base : {6, 11, 16, 21})
        for (int j = base + 1; j <= base + 3; ++j) RotateAbout(in, j, j + 1, base + 4, ax, 50.f * kPi / 180.f);
    float dorsal[3];
    HandSkinRestDorsal(m, dorsal);
    const float tax[3] = {dorsal[0] * sgn, dorsal[1] * sgn, dorsal[2] * sgn};
    RotateAbout(in, 3, 4, 5, tax, 35.f * kPi / 180.f);
    RotateAbout(in, 2, 3, 5, tax, 35.f * kPi / 180.f);
    WriteObj(m, in, dir + "/" + side + "_fist.obj");
}

} // namespace

int main() {
    TestBoneNames();
    HandSkinMesh left;
    std::string err;
    const bool loaded = HandSkinLoadGlb(kHandMeshGlb, kHandMeshGlb_size, left, &err);
    Check(loaded, "load the embedded hand" + (loaded ? std::string() : (": " + err)));
    if (loaded) {
        TestAsset(left);
        TestRestIsBind(left, "left");
        TestRigid(left, false);
        TestRigid(left, true);
        TestCurl(left);
        const HandSkinMesh right = HandSkinMirror(left);
        std::printf("mirrored right hand\n");
        Check(!right.leftHand, "it is a right hand");
        Check(OutwardFraction(right) > 0.95f, "winding still faces out");
        float d[3];
        HandSkinRestDorsal(right, d);
        Check(d[2] > 0.9f, "back of the hand still +z");
        TestRestIsBind(right, "right");
        TestRigid(right, false);
        TestRigid(right, true);
        if (const char* dir = std::getenv("HAND_SKIN_TESTS_OBJ_DIR")) {
            std::printf("pose dumps\n");
            DumpPoses(left, dir, "left");
            DumpPoses(right, dir, "right");
        }
    }
    std::printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

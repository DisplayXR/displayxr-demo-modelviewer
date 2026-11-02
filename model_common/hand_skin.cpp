// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Rigged hand CPU skinning (see hand_skin.h).
 */

#include "hand_skin.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace modelviewer {

namespace {

// XR_EXT_hand_tracking joint indices used here.
constexpr int kWrist = 1;
constexpr int kIndexProximal = 7;
constexpr int kMiddleProximal = 12;
constexpr int kLittleProximal = 22;

// Finger chains, base to tip.
constexpr int kChainLen[5] = {4, 5, 5, 5, 5};
constexpr int kChain[5][5] = {
    {2, 3, 4, 5, -1},        // thumb: metacarpal, proximal, distal, tip
    {6, 7, 8, 9, 10},        // index: metacarpal, proximal, intermediate, distal, tip
    {11, 12, 13, 14, 15},    // middle
    {16, 17, 18, 19, 20},    // ring
    {21, 22, 23, 24, 25},    // little
};

struct V3 { float x, y, z; };
inline V3 v3(const float* p) { return {p[0], p[1], p[2]}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline bool normalize(V3& a) {
    const float l = len(a);
    if (!(l > 1.0e-9f)) return false;
    a = a * (1.f / l);
    return true;
}

//! Orthonormal frame, columns X, Y, Z. Z = -(bone direction), +Y = back of
//! the hand: the XR_EXT_hand_tracking joint convention.
struct Frame { V3 c[3]; };

inline V3 apply(const Frame& f, V3 v) { return f.c[0] * v.x + f.c[1] * v.y + f.c[2] * v.z; }
inline V3 applyT(const Frame& f, V3 v) { return {dot(f.c[0], v), dot(f.c[1], v), dot(f.c[2], v)}; }

//! The minimal rotation taking unit `a` onto unit `b`, applied to `v`.
V3 swing(V3 a, V3 b, V3 v) {
    const V3 k = cross(a, b);
    const float c = dot(a, b);
    if (c < -0.9999f) return v * -1.f;   // antiparallel: never happens on a hand; keep it defined
    // Rodrigues with the unnormalised axis: v' = v c + (k x v) + k (k.v) / (1 + c)
    return v * c + cross(k, v) + k * (dot(k, v) / (1.f + c));
}

bool frameFrom(V3 dir, V3 up, Frame& f) {
    V3 z = dir * -1.f;
    if (!normalize(z)) return false;
    V3 y = up - z * dot(up, z);
    if (!normalize(y)) return false;
    f.c[2] = z;
    f.c[1] = y;
    f.c[0] = cross(y, z);
    return true;
}

//! Frames + bone lengths for one hand pose (rest or tracked). `up` = the
//! back-of-hand reference for the root, or null for the palm-plane normal.
bool buildFrames(const float P[kHandSkinJointCount][3], const bool valid[kHandSkinJointCount], const V3* up,
                 bool leftHand, Frame frames[kHandSkinJointCount], float lens[kHandSkinJointCount]) {
    const int need[] = {kWrist, kIndexProximal, kMiddleProximal, kLittleProximal};
    for (int j : need)
        if (!valid[j]) return false;
    for (int c = 0; c < 5; ++c)
        for (int i = 0; i < kChainLen[c]; ++i)
            if (!valid[kChain[c][i]]) return false;

    const V3 wrist = v3(P[kWrist]);
    V3 rootDir = v3(P[kMiddleProximal]) - wrist;
    const float rootLen = len(rootDir);
    if (!normalize(rootDir)) return false;
    V3 rootUp;
    if (up != nullptr) {
        rootUp = *up;
    } else {
        // Palm-plane normal. Index is thumb-side of the little finger, so
        // (index - wrist) x (little - wrist) points out of the BACK of a left
        // hand and out of the palm of a right one.
        rootUp = cross(v3(P[kIndexProximal]) - wrist, v3(P[kLittleProximal]) - wrist);
        if (!leftHand) rootUp = rootUp * -1.f;
    }
    if (!frameFrom(rootDir, rootUp, frames[kWrist])) return false;
    lens[kWrist] = rootLen;
    frames[0] = frames[kWrist];   // the palm joint drives nothing; keep it defined
    lens[0] = rootLen;

    // Knuckle line: the axis the four fingers curl about.
    V3 knuckles = v3(P[kIndexProximal]) - v3(P[kLittleProximal]);
    if (!normalize(knuckles)) return false;

    for (int c = 0; c < 5; ++c) {
        V3 parentDir = rootDir;
        for (int i = 0; i < kChainLen[c]; ++i) {
            const int j = kChain[c][i];
            V3 dir = parentDir;    // tip: continue the last bone
            float l = 0.f;
            if (i + 1 < kChainLen[c]) {
                dir = v3(P[kChain[c][i + 1]]) - v3(P[j]);
                l = len(dir);
                if (!normalize(dir)) dir = parentDir;
            }
            Frame f;
            V3 x = knuckles - dir * dot(knuckles, dir);
            if (c != 0 && normalize(x)) {
                // Finger: X along the knuckle line (made perpendicular to the
                // bone), Z = -bone, Y = Z x X. A curl about the knuckle line
                // moves Z and Y and leaves X: exact for any curl.
                f.c[2] = dir * -1.f;
                f.c[0] = x;
                f.c[1] = cross(f.c[2], f.c[0]);
            } else {
                // Thumb (or a finger pointing along the knuckle line): swing
                // the wrist frame onto the bone.
                for (int a = 0; a < 3; ++a) f.c[a] = swing(rootDir, dir, frames[kWrist].c[a]);
            }
            frames[j] = f;
            lens[j] = l;
            parentDir = dir;
        }
    }
    return true;
}

V3 quatRotate(const float q[4], V3 v) {
    const V3 u = {q[0], q[1], q[2]};
    const float s = q[3];
    return u * (2.f * dot(u, v)) + v * (s * s - dot(u, u)) + cross(u, v) * (2.f * s);
}

} // namespace

int HandSkinXrJointForBone(const std::string& nameIn) {
    std::string n;
    n.reserve(nameIn.size());
    for (char ch : nameIn) n.push_back((char)std::tolower((unsigned char)ch));
    // Strip the side prefix ("l_" / "r_").
    if (n.size() > 2 && (n[0] == 'l' || n[0] == 'r') && n[1] == '_') n = n.substr(2);
    if (n == "wrist" || n == "palm") return kWrist;
    static const struct { const char* finger; int base; bool thumb; } fingers[] = {
        {"thumb", 2, true}, {"index", 6, false}, {"middle", 11, false}, {"ring", 16, false}, {"pinky", 21, false},
        {"little", 21, false},
    };
    for (const auto& f : fingers) {
        const std::string pre = std::string(f.finger) + "_";
        if (n.compare(0, pre.size(), pre) != 0) continue;
        const std::string seg = n.substr(pre.size());
        if (f.thumb) {
            if (seg == "meta") return f.base + 0;
            if (seg == "a") return f.base + 1;
            if (seg == "b") return f.base + 2;
            if (seg == "end") return f.base + 3;
            return -1;
        }
        if (seg == "meta") return f.base + 0;
        if (seg == "a") return f.base + 1;
        if (seg == "b") return f.base + 2;
        if (seg == "c") return f.base + 3;
        if (seg == "end") return f.base + 4;
        return -1;
    }
    return -1;
}

bool HandSkinFinalize(HandSkinMesh& mesh, const std::vector<float>& boneHeads, std::string* err) {
    const size_t bones = mesh.boneNames.size();
    if (bones == 0 || bones > (size_t)kHandSkinMaxBones || boneHeads.size() != bones * 3) {
        if (err) *err = "bad bone count";
        return false;
    }
    mesh.boneXr.assign(bones, -1);
    std::memset(mesh.restValid, 0, sizeof(mesh.restValid));
    bool leftSeen = false, rightSeen = false;
    for (size_t b = 0; b < bones; ++b) {
        const std::string& nm = mesh.boneNames[b];
        if (nm.size() > 1 && nm[1] == '_') {
            if (nm[0] == 'L' || nm[0] == 'l') leftSeen = true;
            if (nm[0] == 'R' || nm[0] == 'r') rightSeen = true;
        }
        const int xr = HandSkinXrJointForBone(nm);
        mesh.boneXr[b] = xr;
        if (xr < 0) continue;
        // The Palm bone shares the wrist's head in the Leap rig; the first
        // bone to claim a joint (the Wrist, the skin's root) wins.
        if (!mesh.restValid[xr]) {
            std::memcpy(mesh.restJoint[xr], &boneHeads[b * 3], sizeof(float) * 3);
            mesh.restValid[xr] = true;
        }
    }
    if (leftSeen != rightSeen) mesh.leftHand = leftSeen;

    // Every influencing bone must map to a joint.
    const size_t nv = mesh.vertexCount();
    for (size_t v = 0; v < nv; ++v) {
        for (int k = 0; k < 4; ++k) {
            const uint8_t b = mesh.joints[v * 4 + k];
            if (mesh.weights[v * 4 + k] <= 0.f) continue;
            if (b >= bones || mesh.boneXr[b] < 0) {
                if (err) *err = "vertex weighted to an unmapped bone '" + (b < bones ? mesh.boneNames[b] : std::string("?")) + "'";
                return false;
            }
        }
    }
    Frame frames[kHandSkinJointCount];
    float lens[kHandSkinJointCount];
    if (!buildFrames(mesh.restJoint, mesh.restValid, nullptr, mesh.leftHand, frames, lens)) {
        if (err) *err = "rest pose is missing a joint (needs the wrist and every finger bone)";
        return false;
    }
    return true;
}

HandSkinMesh HandSkinMirror(const HandSkinMesh& in) {
    HandSkinMesh m = in;
    for (size_t i = 0; i < m.pos.size(); i += 3) m.pos[i] = -m.pos[i];
    for (size_t i = 0; i < m.nrm.size(); i += 3) m.nrm[i] = -m.nrm[i];
    for (int j = 0; j < kHandSkinJointCount; ++j) m.restJoint[j][0] = -m.restJoint[j][0];
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) std::swap(m.indices[t + 1], m.indices[t + 2]);
    for (std::string& n : m.boneNames)
        if (n.size() > 1 && n[1] == '_') {
            if (n[0] == 'L') n[0] = 'R';
            else if (n[0] == 'R') n[0] = 'L';
            else if (n[0] == 'l') n[0] = 'r';
            else if (n[0] == 'r') n[0] = 'l';
        }
    m.leftHand = !in.leftHand;
    return m;
}

void HandSkinRestDorsal(const HandSkinMesh& mesh, float out[3]) {
    Frame frames[kHandSkinJointCount];
    float lens[kHandSkinJointCount];
    out[0] = out[1] = out[2] = 0.f;
    if (!buildFrames(mesh.restJoint, mesh.restValid, nullptr, mesh.leftHand, frames, lens)) return;
    out[0] = frames[kWrist].c[1].x;
    out[1] = frames[kWrist].c[1].y;
    out[2] = frames[kWrist].c[1].z;
}

bool HandSkinComputeFrames(const HandSkinMesh& mesh, const HandSkinJointIn joints[kHandSkinJointCount],
                           HandSkinBone out[kHandSkinJointCount]) {
    Frame rest[kHandSkinJointCount], trk[kHandSkinJointCount];
    float restLen[kHandSkinJointCount], trkLen[kHandSkinJointCount];
    if (!buildFrames(mesh.restJoint, mesh.restValid, nullptr, mesh.leftHand, rest, restLen)) return false;

    float P[kHandSkinJointCount][3];
    bool valid[kHandSkinJointCount];
    for (int j = 0; j < kHandSkinJointCount; ++j) {
        valid[j] = joints[j].valid;
        std::memcpy(P[j], joints[j].pos, sizeof(P[j]));
    }
    V3 up;
    const V3* upPtr = nullptr;
    if (joints[kWrist].orientValid) {
        up = quatRotate(joints[kWrist].orient, {0.f, 1.f, 0.f});
        if (normalize(up)) upPtr = &up;
    }
    if (!buildFrames(P, valid, upPtr, mesh.leftHand, trk, trkLen)) return false;

    // Hand scale across every bone; each bone's own ratio along it.
    if (!(restLen[kWrist] > 0.f) || !(trkLen[kWrist] > 0.f)) return false;
    const float s = trkLen[kWrist] / restLen[kWrist];

    HandSkinBone tmp[kHandSkinJointCount];
    for (int j = 0; j < kHandSkinJointCount; ++j) {
        const int src = (j == 0) ? kWrist : j;
        float along = s;
        if (restLen[src] > 1.0e-6f && trkLen[src] > 0.f) {
            along = trkLen[src] / restLen[src];
            along = std::min(std::max(along, 0.5f * s), 2.f * s);
        }
        const float S[3] = {s, s, along};   // frame axes X, Y (across), Z (along the bone)
        const Frame& R = rest[src];
        const Frame& T = trk[src];
        // lin = T * diag(S) * R^T ; nrm = T * diag(1/S) * R^T  (column-major)
        for (int col = 0; col < 3; ++col) {
            const V3 e = {col == 0 ? 1.f : 0.f, col == 1 ? 1.f : 0.f, col == 2 ? 1.f : 0.f};
            const V3 r = applyT(R, e);                     // R^T e
            const V3 a = apply(T, {r.x * S[0], r.y * S[1], r.z * S[2]});
            const V3 n = apply(T, {r.x / S[0], r.y / S[1], r.z / S[2]});
            tmp[j].lin[col * 3 + 0] = a.x; tmp[j].lin[col * 3 + 1] = a.y; tmp[j].lin[col * 3 + 2] = a.z;
            tmp[j].nrm[col * 3 + 0] = n.x; tmp[j].nrm[col * 3 + 1] = n.y; tmp[j].nrm[col * 3 + 2] = n.z;
        }
        // trans = P_trk - lin * P_rest
        const V3 pr = v3(mesh.restJoint[src]);
        const float* L = tmp[j].lin;
        const V3 lp = {L[0] * pr.x + L[3] * pr.y + L[6] * pr.z, L[1] * pr.x + L[4] * pr.y + L[7] * pr.z,
                       L[2] * pr.x + L[5] * pr.y + L[8] * pr.z};
        tmp[j].trans[0] = P[src][0] - lp.x;
        tmp[j].trans[1] = P[src][1] - lp.y;
        tmp[j].trans[2] = P[src][2] - lp.z;
    }
    std::memcpy(out, tmp, sizeof(tmp));
    return true;
}

void HandSkinApply(const HandSkinMesh& mesh, const HandSkinBone frames[kHandSkinJointCount], float* dst,
                   size_t strideFloats) {
    const size_t nv = mesh.vertexCount();
    for (size_t v = 0; v < nv; ++v) {
        const float* p = &mesh.pos[v * 3];
        const float* n = &mesh.nrm[v * 3];
        float op[3] = {0.f, 0.f, 0.f}, on[3] = {0.f, 0.f, 0.f};
        for (int k = 0; k < 4; ++k) {
            const float w = mesh.weights[v * 4 + k];
            if (w <= 0.f) continue;
            const int xr = mesh.boneXr[mesh.joints[v * 4 + k]];
            if (xr < 0) continue;
            const HandSkinBone& B = frames[xr];
            for (int r = 0; r < 3; ++r) {
                op[r] += w * (B.lin[r] * p[0] + B.lin[3 + r] * p[1] + B.lin[6 + r] * p[2] + B.trans[r]);
                on[r] += w * (B.nrm[r] * n[0] + B.nrm[3 + r] * n[1] + B.nrm[6 + r] * n[2]);
            }
        }
        float nl = std::sqrt(on[0] * on[0] + on[1] * on[1] + on[2] * on[2]);
        if (nl < 1.0e-12f) nl = 1.f;
        float* d = dst + v * strideFloats;
        d[0] = op[0]; d[1] = op[1]; d[2] = op[2];
        d[3] = on[0] / nl; d[4] = on[1] / nl; d[5] = on[2] / nl;
    }
}

} // namespace modelviewer

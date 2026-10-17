// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Turntable orbit: the viewer's orbit state is a MODEL transform, not
 *         a camera move.
 *
 * On a 3D display the camera is the physical viewer. Orbiting the camera
 * around the model spins the whole world — the display plane, the
 * convergence distance, the rear depth budget and anything that lives in the
 * app's reference space (tracked hands) would all swing with it. So every leg
 * keeps its familiar orbit state — the point under the display centre `C`
 * (cameraPos: WASD, double-click focus, the animated anchor), the yaw / pitch
 * / roll `Q` (mouse drag, pinch drag, auto-orbit) and the zoom `sf`
 * (ViewParams::scaleFactor) — but interprets it as the transform that places
 * the model in front of a FIXED rig:
 *
 *     S = Scale(sf) · R(Q)⁻¹ · T(−C)          (model space → locate space)
 *
 * The rig declared through XR_DXR_view_rig is the identity pose at the locate
 * origin with the fit's virtualDisplayHeight — it never moves, never zooms —
 * and the model is drawn through S (ModelRenderer::setSceneTransform), which
 * the renderer applies to the model and its ray casts only; the hand overlay
 * is drawn in the locate space untouched.
 *
 * S reproduces the picture the old camera orbit produced exactly: yaw turns
 * the model about the world up axis through C, pitch about the display's
 * right axis, roll about the view axis, and zoom is a uniform scale about C,
 * so C stays on the convergence plane. Keys, mouse, touch and hands all feed
 * the same state and therefore agree by construction.
 */
#pragma once

#include <openxr/openxr.h>

#include <cmath>

namespace modelviewer {

//! The fixed rig every leg declares: identity at the locate origin.
inline XrPosef OrbitRigPose() { return {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}}; }

//! Column-major S = Scale(sf) · R(orbit.orientation)⁻¹ · T(−orbit.position).
inline void BuildSceneTransform(float out[16], const XrPosef& orbit, float scale) {
    const XrQuaternionf& q = orbit.orientation;
    // Rotation matrix of q (column-major), then transpose it for the inverse.
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    // R(q) rows (r[row][col]); R⁻¹ = Rᵀ so Rinv[row][col] = R[col][row].
    const float r00 = 1.f - 2.f * (yy + zz), r01 = 2.f * (xy - wz),       r02 = 2.f * (xz + wy);
    const float r10 = 2.f * (xy + wz),       r11 = 1.f - 2.f * (xx + zz), r12 = 2.f * (yz - wx);
    const float r20 = 2.f * (xz - wy),       r21 = 2.f * (yz + wx),       r22 = 1.f - 2.f * (xx + yy);
    // M = s · Rᵀ ; t = −M · C.
    const float s = scale > 0.f ? scale : 1.f;
    const float m00 = s * r00, m01 = s * r10, m02 = s * r20;
    const float m10 = s * r01, m11 = s * r11, m12 = s * r21;
    const float m20 = s * r02, m21 = s * r12, m22 = s * r22;
    const float cx = orbit.position.x, cy = orbit.position.y, cz = orbit.position.z;
    // Column-major: out[col*4 + row].
    out[0] = m00; out[1] = m10; out[2] = m20; out[3] = 0.f;
    out[4] = m01; out[5] = m11; out[6] = m21; out[7] = 0.f;
    out[8] = m02; out[9] = m12; out[10] = m22; out[11] = 0.f;
    out[12] = -(m00 * cx + m01 * cy + m02 * cz);
    out[13] = -(m10 * cx + m11 * cy + m12 * cz);
    out[14] = -(m20 * cx + m21 * cy + m22 * cz);
    out[15] = 1.f;
}

} // namespace modelviewer

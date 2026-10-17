// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pinch-gesture math (see hand_gestures.h for the spec).
 */

#include "hand_gestures.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace modelviewer {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

//! Wrap an angle difference into (-pi, pi].
float wrapAngle(float a) {
    while (a > kPi) a -= 2.f * kPi;
    while (a <= -kPi) a += 2.f * kPi;
    return a;
}

} // namespace

void HandGestureConfig::applyEnvironment() {
    if (const char* e = std::getenv("DXR_MODELVIEWER_PINCH_M")) {
        float v = 0.f;
        if (std::sscanf(e, "%f", &v) == 1 && v > 0.f) {
            pinchEnterM = v;
            pinchExitM = v * 1.6f;
            std::printf("hands: DXR_MODELVIEWER_PINCH_M -> pinch enter %.3f m, exit %.3f m\n", pinchEnterM, pinchExitM);
        }
    }
    if (const char* e = std::getenv("DXR_MODELVIEWER_ORBIT_RAD_PER_M")) {
        float v = 0.f;
        if (std::sscanf(e, "%f", &v) == 1 && v > 0.f) {
            orbitRadPerM = v;
            std::printf("hands: DXR_MODELVIEWER_ORBIT_RAD_PER_M -> %.2f rad/m\n", v);
        }
    }
}

bool PinchDetector::update(bool tracked, float tipDistanceM, float enterM, float exitM) {
    if (!tracked) {
        pinched_ = false;
        return false;
    }
    if (pinched_) {
        if (tipDistanceM > exitM) pinched_ = false;
    } else {
        if (tipDistanceM < enterM) pinched_ = true;
    }
    return pinched_;
}

void HandGestureController::reset() {
    for (int h = 0; h < 2; ++h) {
        pinch_[h].reset();
        tipDist_[h] = -1.f;
        pointValid_[h] = false;
        prevOne_[h] = false;
    }
    prevTwo_ = false;
    prevTime_ = -1.0;
}

HandGestureDelta HandGestureController::update(const HandGestureFrame& frame) {
    HandGestureDelta out;

    // Frame interval for the EMA. The first frame, a stalled clock or a
    // long gap all count as "no smoothing history" rather than a huge step.
    float dt = 0.f;
    if (prevTime_ >= 0.0 && frame.timeSec > prevTime_) {
        dt = (float)(frame.timeSec - prevTime_);
        if (dt > 0.25f) dt = 0.25f;
    }
    prevTime_ = frame.timeSec;

    bool pinched[2] = {false, false};
    for (int h = 0; h < 2; ++h) {
        const HandGestureSample& s = frame.hand[h];
        if (!s.tracked) {
            pinch_[h].reset();
            tipDist_[h] = -1.f;
            pointValid_[h] = false;
            continue;
        }
        tipDist_[h] = dist3(s.thumbTip, s.indexTip);
        pinched[h] = pinch_[h].update(true, tipDist_[h], cfg_.pinchEnterM, cfg_.pinchExitM);

        // The pinch point is the midpoint of the two tips; smooth it.
        float raw[3];
        for (int i = 0; i < 3; ++i) raw[i] = 0.5f * (s.thumbTip[i] + s.indexTip[i]);
        if (!pointValid_[h] || cfg_.smoothTauS <= 0.f || dt <= 0.f) {
            for (int i = 0; i < 3; ++i) point_[h][i] = raw[i];
        } else {
            const float alpha = 1.f - std::exp(-dt / cfg_.smoothTauS);
            for (int i = 0; i < 3; ++i) point_[h][i] += alpha * (raw[i] - point_[h][i]);
        }
        pointValid_[h] = true;
    }

    const bool two = pinched[0] && pinched[1];
    const int one = (!two && pinched[0]) ? 0 : (!two && pinched[1]) ? 1 : -1;
    const float k = cfg_.orbitRadPerM;

    if (two) {
        out.twoHand = true;
        float mid[3], v[3];
        for (int i = 0; i < 3; ++i) {
            mid[i] = 0.5f * (point_[0][i] + point_[1][i]);
            v[i] = point_[1][i] - point_[0][i];
        }
        const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        // Roll is measured in the display plane (XY); the view axis is Z.
        const float angle = std::atan2(v[1], v[0]);
        if (prevTwo_) {
            // Midpoint drag = orbit, mouse-drag sign convention.
            out.yaw = -(mid[0] - prevMid_[0]) * k;
            out.pitch = (mid[1] - prevMid_[1]) * k;
            // Separation ratio = zoom, clamped so one bad sample cannot slam it.
            if (prevDist_ > 1.0e-4f && d > 1.0e-4f) {
                float z = d / prevDist_;
                if (z > cfg_.maxZoomStep) z = cfg_.maxZoomStep;
                if (z < 1.f / cfg_.maxZoomStep) z = 1.f / cfg_.maxZoomStep;
                out.zoom = z;
            }
            // The hands turning CCW (as the viewer sees them) must turn the
            // model CCW on screen. The rig is the camera: rolling it by +a
            // turns the world by -a, so the rig gets the negated angle.
            out.roll = -wrapAngle(angle - prevAngle_);
        }
        for (int i = 0; i < 3; ++i) prevMid_[i] = mid[i];
        prevDist_ = d;
        prevAngle_ = angle;
        prevTwo_ = true;
        prevOne_[0] = prevOne_[1] = false;
    } else if (one >= 0) {
        out.oneHandOrbit = true;
        const float* p = point_[one];
        if (prevOne_[one]) {
            out.yaw = -(p[0] - prevPoint_[one][0]) * k;
            out.pitch = (p[1] - prevPoint_[one][1]) * k;
        }
        for (int i = 0; i < 3; ++i) prevPoint_[one][i] = p[i];
        prevOne_[one] = true;
        prevOne_[one ^ 1] = false;
        prevTwo_ = false;
    } else {
        prevTwo_ = false;
        prevOne_[0] = prevOne_[1] = false;
    }

    return out;
}

} // namespace modelviewer

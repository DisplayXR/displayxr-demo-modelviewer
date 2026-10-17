// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pinch-gesture math for the model viewer: hand positions in, rig
 *         deltas out. Pure C++ — no OpenXR, no Vulkan — so it is unit-tested
 *         without a runtime (tests/hand_gestures_tests.cpp).
 *
 * Input is the thumb-tip / index-tip position of each hand in the space the
 * hands were located in (the app's LOCAL space: physical metres, +X right,
 * +Y up, +Z toward the viewer, display-plane relative under the rig rule of
 * runtime ADR-034 Amendment 2). Output is what to ADD to the display rig's
 * yaw / pitch / roll and what to MULTIPLY its zoom (ViewParams::scaleFactor)
 * by, with the same sign convention as the mouse drag every leg already
 * implements (`yaw -= dx`, `pitch -= dy_screen`, `scaleFactor *= factor`).
 *
 * Gesture spec
 * ------------
 * - pinch       = thumb tip to index tip distance below `pinchEnterM`; it
 *                 stays a pinch until the distance exceeds `pinchExitM`
 *                 (hysteresis, so a jittery tip does not chatter).
 * - one hand pinched and moving = ORBIT: the pinch point's lateral motion
 *                 (metres) × `orbitRadPerM` goes to yaw (x) and pitch (y).
 * - both hands pinched:
 *     · inter-hand distance ratio       = ZOOM (dolly) — doubling the
 *                                         separation doubles the zoom
 *     · rotation of the inter-hand vector about the view axis (Z) = ROLL
 *     · midpoint motion                 = ORBIT, as for one hand
 * - a hand that is not tracked contributes nothing; the frame it comes
 *   back re-anchors the gesture, so there is never a jump.
 * - the pinch point is smoothed with a short EMA (`smoothTauS`) BEFORE
 *   differencing, so the deltas are jitter-free without lagging a real
 *   drag noticeably. 0 disables it (the deterministic tests use that).
 */
#pragma once

#include <cstdint>

namespace modelviewer {

struct HandGestureSample {
    bool tracked = false;             //!< both tips located this frame
    float thumbTip[3] = {0.f, 0.f, 0.f};
    float indexTip[3] = {0.f, 0.f, 0.f};
};

struct HandGestureFrame {
    HandGestureSample hand[2];        //!< 0 = left, 1 = right
    double timeSec = 0.0;             //!< monotonic seconds (for the EMA)
};

struct HandGestureDelta {
    float yaw = 0.f;      //!< radians; ADD to the rig yaw
    float pitch = 0.f;    //!< radians; ADD to the rig pitch (caller clamps)
    float roll = 0.f;     //!< radians; ADD to the rig roll about the view axis
    float zoom = 1.f;     //!< MULTIPLY ViewParams::scaleFactor by this
    bool oneHandOrbit = false;  //!< exactly one hand pinched this frame
    bool twoHand = false;       //!< both hands pinched this frame
    bool any() const { return oneHandOrbit || twoHand; }
};

struct HandGestureConfig {
    float pinchEnterM = 0.025f;      //!< tip distance that starts a pinch
    float pinchExitM = 0.040f;       //!< tip distance that ends it (> enter)
    float orbitRadPerM = 10.0f;      //!< 10 cm of pinch drag ~= 57 degrees
    float smoothTauS = 0.05f;        //!< EMA time constant on the pinch point; 0 = off
    float maxZoomStep = 1.25f;       //!< per-frame zoom ratio clamp (both ways)

    //! Apply DXR_MODELVIEWER_PINCH_M=<metres> (pinch-enter distance; exit =
    //! 1.6x) and DXR_MODELVIEWER_ORBIT_RAD_PER_M=<rad/m> when set. Desktop
    //! tuning aids for a given tracker / hand size; defaults otherwise.
    void applyEnvironment();
};

//! Two-threshold pinch state. `update` returns the new state.
class PinchDetector {
public:
    bool update(bool tracked, float tipDistanceM, float enterM, float exitM);
    bool pinched() const { return pinched_; }
    void reset() { pinched_ = false; }

private:
    bool pinched_ = false;
};

class HandGestureController {
public:
    HandGestureController() = default;
    explicit HandGestureController(const HandGestureConfig& cfg) : cfg_(cfg) {}

    const HandGestureConfig& config() const { return cfg_; }
    void setConfig(const HandGestureConfig& cfg) { cfg_ = cfg; }

    //! Feed one frame of hand samples; returns the rig delta for this frame.
    HandGestureDelta update(const HandGestureFrame& frame);

    //! Pinch state of hand `i` (0 = left, 1 = right) after the last update.
    bool pinched(int i) const { return pinch_[i & 1].pinched(); }
    //! Last measured thumb-index tip distance (metres); < 0 when untracked.
    float tipDistance(int i) const { return tipDist_[i & 1]; }

    //! Forget every anchor (e.g. when tracking is lost for a while).
    void reset();

private:
    HandGestureConfig cfg_;
    PinchDetector pinch_[2];
    float tipDist_[2] = {-1.f, -1.f};
    // Smoothed pinch point per hand, and whether it holds a value.
    float point_[2][3] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    bool pointValid_[2] = {false, false};
    // Gesture anchors from the previous frame.
    bool prevOne_[2] = {false, false};
    float prevPoint_[2][3] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    bool prevTwo_ = false;
    float prevMid_[3] = {0.f, 0.f, 0.f};
    float prevDist_ = 0.f;
    float prevAngle_ = 0.f;
    double prevTime_ = -1.0;
};

} // namespace modelviewer

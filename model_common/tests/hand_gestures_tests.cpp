// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Self-check for hand_gestures.h — pinch hysteresis, one-hand orbit,
 *         two-hand zoom / roll / midpoint orbit, tracking-loss re-anchoring,
 *         and the EMA. No OpenXR runtime, no Vulkan: one console exe, exit 0
 *         = every case passed (same shape as gltf_uri_tests).
 */

#include "hand_gestures.h"

#include <cmath>
#include <cstdio>
#include <string>

using modelviewer::HandGestureConfig;
using modelviewer::HandGestureController;
using modelviewer::HandGestureDelta;
using modelviewer::HandGestureFrame;
using modelviewer::HandGestureSample;
using modelviewer::PinchDetector;

namespace {

int g_failures = 0;
int g_checks = 0;
constexpr float kPi = 3.14159265358979323846f;

void Check(bool cond, const std::string& what) {
    ++g_checks;
    std::printf("  %s  %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

//! A hand whose pinch point sits at (x, y, z); `open` metres between the tips.
HandGestureSample Hand(float x, float y, float z, float open) {
    HandGestureSample s;
    s.tracked = true;
    s.thumbTip[0] = x - 0.5f * open; s.thumbTip[1] = y; s.thumbTip[2] = z;
    s.indexTip[0] = x + 0.5f * open; s.indexTip[1] = y; s.indexTip[2] = z;
    return s;
}

HandGestureConfig Deterministic() {
    HandGestureConfig c;
    c.smoothTauS = 0.f;   // no EMA: deltas are exact
    c.orbitRadPerM = 10.f;
    return c;
}

void TestPinchHysteresis() {
    std::printf("pinch hysteresis\n");
    PinchDetector p;
    Check(!p.update(true, 0.05f, 0.025f, 0.040f), "open hand is not a pinch");
    Check(!p.update(true, 0.030f, 0.025f, 0.040f), "between the thresholds from open stays open");
    Check(p.update(true, 0.020f, 0.025f, 0.040f), "below enter starts the pinch");
    Check(p.update(true, 0.030f, 0.025f, 0.040f), "between the thresholds from pinched stays pinched");
    Check(p.update(true, 0.039f, 0.025f, 0.040f), "just under exit still pinched");
    Check(!p.update(true, 0.041f, 0.025f, 0.040f), "above exit releases");
    Check(p.update(true, 0.010f, 0.025f, 0.040f), "re-pinch");
    Check(!p.update(false, 0.010f, 0.025f, 0.040f), "tracking loss releases regardless of distance");
}

void TestOneHandOrbit() {
    std::printf("one-hand orbit\n");
    HandGestureController c(Deterministic());
    HandGestureFrame f;
    double t = 0.0;
    float yaw = 0.f, pitch = 0.f;
    // Right hand pinched, dragging +X by 0.10 m and +Y by 0.05 m over 10 frames.
    for (int i = 0; i <= 10; ++i) {
        f.timeSec = t; t += 1.0 / 60.0;
        f.hand[0] = HandGestureSample();                    // left untracked
        f.hand[1] = Hand(0.01f * i, 0.005f * i, 0.3f, 0.01f);
        HandGestureDelta d = c.update(f);
        Check(d.oneHandOrbit && !d.twoHand, "frame flagged one-hand");
        if (i == 0) Check(d.yaw == 0.f && d.pitch == 0.f, "first pinched frame anchors, no delta");
        yaw += d.yaw; pitch += d.pitch;
    }
    Check(Near(yaw, -1.0f, 1e-4f), "0.10 m right drag -> yaw -1.0 rad (mouse sign)");
    Check(Near(pitch, 0.5f, 1e-4f), "0.05 m upward drag -> pitch +0.5 rad");
    Check(c.pinched(1) && !c.pinched(0), "pinch state readable per hand");

    // Open the hand: no more deltas, even while it keeps moving.
    f.timeSec = t; t += 1.0 / 60.0;
    f.hand[1] = Hand(0.5f, 0.5f, 0.3f, 0.08f);
    HandGestureDelta d = c.update(f);
    Check(!d.any() && d.yaw == 0.f, "released hand produces nothing");
    // Pinch again far away: anchors, no jump.
    f.timeSec = t; t += 1.0 / 60.0;
    f.hand[1] = Hand(0.5f, 0.5f, 0.3f, 0.01f);
    d = c.update(f);
    Check(d.oneHandOrbit && d.yaw == 0.f && d.pitch == 0.f, "re-pinch re-anchors with zero delta");
}

void TestTwoHand() {
    std::printf("two-hand zoom / roll / orbit\n");
    HandGestureController c(Deterministic());
    HandGestureFrame f;
    double t = 0.0;
    auto step = [&](HandGestureSample l, HandGestureSample r) {
        f.timeSec = t; t += 1.0 / 60.0;
        f.hand[0] = l; f.hand[1] = r;
        return c.update(f);
    };
    // Anchor: hands 0.20 m apart on the X axis.
    HandGestureDelta d = step(Hand(-0.10f, 0.f, 0.3f, 0.01f), Hand(0.10f, 0.f, 0.3f, 0.01f));
    Check(d.twoHand && d.zoom == 1.f && d.roll == 0.f, "first two-hand frame anchors");

    // Zoom: separate to 0.40 m over 8 frames (each step under the clamp).
    float zoom = 1.f;
    for (int i = 1; i <= 8; ++i) {
        const float half = 0.10f + 0.0125f * i;
        d = step(Hand(-half, 0.f, 0.3f, 0.01f), Hand(half, 0.f, 0.3f, 0.01f));
        zoom *= d.zoom;
        Check(d.yaw == 0.f && d.pitch == 0.f, "symmetric separation moves no midpoint");
    }
    Check(Near(zoom, 2.0f, 1e-3f), "doubling the separation doubles the zoom");

    // Per-frame clamp: a single frame that jumps 2.25x (0.40 -> 0.90 m, still
    // inside the separation guard) is limited to maxZoomStep.
    d = step(Hand(-0.45f, 0.f, 0.3f, 0.01f), Hand(0.45f, 0.f, 0.3f, 0.01f));
    Check(Near(d.zoom, 1.25f, 1e-5f), "one-frame 2.25x jump clamps to maxZoomStep");

    // Roll: rotate the pair CCW by 90 degrees in 9 steps of 10 degrees.
    HandGestureController c2(Deterministic());
    HandGestureFrame f2;
    double t2 = 0.0;
    float roll = 0.f;
    for (int i = 0; i <= 9; ++i) {
        const float a = (float)i * (kPi / 18.f);
        const float hx = 0.20f * std::cos(a), hy = 0.20f * std::sin(a);
        f2.timeSec = t2; t2 += 1.0 / 60.0;
        f2.hand[0] = Hand(-hx, -hy, 0.3f, 0.01f);
        f2.hand[1] = Hand(hx, hy, 0.3f, 0.01f);
        HandGestureDelta r = c2.update(f2);
        roll += r.roll;
        if (i > 0) Check(Near(r.zoom, 1.f, 1e-4f), "pure rotation keeps the zoom at 1");
    }
    Check(Near(roll, -kPi / 2.f, 1e-3f), "90 deg CCW hand rotation -> rig roll -pi/2 (model follows hands)");

    // Wrap: crossing +/-pi must not produce a 2*pi spike.
    HandGestureController c3(Deterministic());
    HandGestureFrame f3;
    float rollSum = 0.f;
    for (int i = 0; i <= 4; ++i) {
        const float a = kPi - 0.1f + 0.05f * (float)i;     // 170 deg -> 190 deg
        const float hx = 0.20f * std::cos(a), hy = 0.20f * std::sin(a);
        f3.timeSec = (double)i / 60.0;
        f3.hand[0] = Hand(-hx, -hy, 0.3f, 0.01f);
        f3.hand[1] = Hand(hx, hy, 0.3f, 0.01f);
        rollSum += c3.update(f3).roll;
    }
    Check(Near(rollSum, -0.2f, 1e-3f), "angle wrap across pi is continuous");

    // Midpoint drag with both hands = orbit.
    HandGestureController c4(Deterministic());
    HandGestureFrame f4;
    float yaw = 0.f;
    for (int i = 0; i <= 5; ++i) {
        f4.timeSec = (double)i / 60.0;
        f4.hand[0] = Hand(-0.10f + 0.02f * i, 0.f, 0.3f, 0.01f);
        f4.hand[1] = Hand(0.10f + 0.02f * i, 0.f, 0.3f, 0.01f);
        yaw += c4.update(f4).yaw;
    }
    Check(Near(yaw, -1.0f, 1e-4f), "two-hand midpoint drag 0.10 m -> yaw -1.0 rad");
}

void TestTrackingLoss() {
    std::printf("tracking loss\n");
    HandGestureController c(Deterministic());
    HandGestureFrame f;
    f.timeSec = 0.0;
    f.hand[0] = Hand(-0.10f, 0.f, 0.3f, 0.01f);
    f.hand[1] = Hand(0.10f, 0.f, 0.3f, 0.01f);
    c.update(f);
    // Left hand drops out: the right hand alone must not inherit the
    // two-hand anchor (that would be a spurious orbit/zoom), and the frame
    // after re-anchors cleanly.
    f.timeSec = 1.0 / 60.0;
    f.hand[0] = HandGestureSample();
    f.hand[1] = Hand(0.15f, 0.f, 0.3f, 0.01f);
    HandGestureDelta d = c.update(f);
    Check(d.oneHandOrbit && !d.twoHand, "one hand lost -> one-hand mode");
    Check(d.yaw == 0.f && d.zoom == 1.f && d.roll == 0.f, "no delta on the frame the mode changes");
    Check(c.tipDistance(0) < 0.f, "untracked hand reports no tip distance");
    f.timeSec = 2.0 / 60.0;
    f.hand[1] = Hand(0.16f, 0.f, 0.3f, 0.01f);
    d = c.update(f);
    Check(Near(d.yaw, -0.1f, 1e-5f), "next frame orbits from the new anchor only");
    // Left comes back pinched: two-hand mode anchors again without a jump.
    f.timeSec = 3.0 / 60.0;
    f.hand[0] = Hand(-0.30f, 0.f, 0.3f, 0.01f);
    d = c.update(f);
    Check(d.twoHand && d.yaw == 0.f && d.zoom == 1.f, "hand returns -> two-hand anchors with zero delta");
}

void TestSeparationGuard() {
    std::printf("two-hand separation guard\n");
    HandGestureConfig cfg = Deterministic();
    cfg.maxHandSeparationM = 0.5f;
    HandGestureController c(cfg);
    HandGestureFrame f;
    f.timeSec = 0.0;
    f.hand[0] = Hand(-0.10f, 0.f, 0.3f, 0.01f);
    f.hand[1] = Hand(0.10f, 0.f, 0.3f, 0.01f);
    c.update(f);
    // A glitch frame: hands 2 m apart. Nothing happens, baseline dropped.
    f.timeSec = 1.0 / 60.0;
    f.hand[1] = Hand(1.90f, 0.f, 0.3f, 0.01f);
    HandGestureDelta d = c.update(f);
    Check(!d.any() && d.zoom == 1.f, "implausible separation: no gesture at all");
    // Back to plausible, moved: re-anchors with zero delta rather than
    // integrating the glitch, then tracks again.
    f.timeSec = 2.0 / 60.0;
    f.hand[1] = Hand(0.20f, 0.f, 0.3f, 0.01f);
    d = c.update(f);
    Check(d.twoHand && d.zoom == 1.f && d.yaw == 0.f, "first plausible frame after a glitch re-anchors");
    f.timeSec = 3.0 / 60.0;
    f.hand[1] = Hand(0.25f, 0.f, 0.3f, 0.01f);
    d = c.update(f);
    Check(Near(d.zoom, 0.35f / 0.30f, 1e-4f), "then tracks from the new baseline");
}

void TestSmoothing() {
    std::printf("EMA smoothing\n");
    HandGestureConfig cfg = Deterministic();
    cfg.smoothTauS = 0.05f;
    HandGestureController c(cfg);
    HandGestureFrame f;
    // A step: the pinch point jumps 0.10 m and holds. The accumulated yaw
    // must approach the unsmoothed -1.0 rad monotonically, never overshoot,
    // and get within 1% after 0.5 s (10 time constants).
    f.timeSec = 0.0;
    f.hand[1] = Hand(0.f, 0.f, 0.3f, 0.01f);
    c.update(f);
    float yaw = 0.f, prevYaw = 0.f;
    bool monotone = true, overshoot = false;
    for (int i = 1; i <= 30; ++i) {
        f.timeSec = (double)i / 60.0;
        f.hand[1] = Hand(0.10f, 0.f, 0.3f, 0.01f);
        yaw += c.update(f).yaw;
        if (yaw > prevYaw + 1e-6f) monotone = false;
        if (yaw < -1.0f - 1e-4f) overshoot = true;
        prevYaw = yaw;
    }
    Check(monotone, "smoothed response is monotone");
    Check(!overshoot, "smoothed response never overshoots");
    Check(Near(yaw, -1.0f, 0.01f), "smoothed response converges within 1% in 0.5 s");
    Check(std::fabs(yaw) < 1.0f, "smoothing lags the raw step (it is doing something)");
}

} // namespace

int main() {
    TestPinchHysteresis();
    TestOneHandOrbit();
    TestTwoHand();
    TestTrackingLoss();
    TestSeparationGuard();
    TestSmoothing();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

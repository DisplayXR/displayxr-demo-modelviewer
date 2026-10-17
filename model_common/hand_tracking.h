// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  XR_EXT_hand_tracking consumer for the model viewer, plus the map
 *         from the physical display space the joints arrive in to the
 *         virtual world the model lives in.
 *
 * Lifecycle: the platform leg checks `kExtensionName` against
 * xrEnumerateInstanceExtensionProperties and lists it at xrCreateInstance
 * (F-5); after xrCreateSession it calls init(); every frame, after
 * xrWaitFrame, locate(); destroy() before xrDestroySession. Everything is
 * inert when the extension is absent, the system reports
 * `supportsHandTracking == XR_FALSE`, or tracker creation fails — the demo
 * then runs exactly as before.
 *
 * Space: joints are located in the app's LOCAL reference space, where the
 * runtime reports them in physical metres relative to the physical display
 * plane and composed with the rig (ADR-034 Amendment 2: hands follow
 * WASD/mouse-look, not eye-tracked head parallax). Under XR_DXR_view_rig the
 * model lives in a virtual world where the rig's `virtualDisplayHeight`
 * spans the window canvas, so a joint is rebased through HandRigMap: display
 * plane -> canvas centre -> metres-to-virtual scale -> the chained rig pose.
 * Without the rig (no extension) the locate space IS the model's space and
 * the map is the identity.
 */
#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_view_rig.h>

#include <cstdint>

#include "hand_gestures.h"

namespace modelviewer {

struct HandJointSet {
    bool active = false;                                     //!< XrHandJointLocationsEXT::isActive
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];  //!< valid when active
};

class HandTracking {
public:
    static constexpr const char* kExtensionName = XR_EXT_HAND_TRACKING_EXTENSION_NAME;

    //! Create both trackers. `extensionEnabled` = the leg listed
    //! XR_EXT_hand_tracking at xrCreateInstance. Returns true when hand
    //! tracking is live; false leaves the object inert (safe to keep calling).
    bool init(XrInstance instance, XrSystemId systemId, XrSession session, bool extensionEnabled);
    void destroy();

    bool enabled() const { return tracker_[0] != XR_NULL_HANDLE && tracker_[1] != XR_NULL_HANDLE; }
    //! The system property, whatever init() decided (for the log / HUD).
    bool systemSupportsHandTracking() const { return supported_; }

    //! Locate all joints of both hands in `base` at `time`. No-op when inert.
    void locate(XrSpace base, XrTime time);

    //! Debug / mounting aid: a constant offset (metres, locate space) added
    //! to every located joint. init() seeds it from
    //! DXR_MODELVIEWER_HANDS_OFFSET_M="x,y,z"; zero by default. Lets a dev
    //! box bring sim_input's root-space circles (1.45 m under the panel) into
    //! view, or nudge a sensor mounted off the panel's centre line.
    void setOffset(const XrVector3f& o) { offset_ = o; }
    const XrVector3f& offset() const { return offset_; }

    const HandJointSet& hand(int i) const { return hands_[i & 1]; }
    bool anyActive() const { return hands_[0].active || hands_[1].active; }
    uint64_t locateCount() const { return locateCount_; }

    //! Gesture input from a joint set: tracked only when the set is active
    //! AND both tip positions are valid this frame.
    static void toGestureSample(const HandJointSet& h, HandGestureSample& out);

private:
    PFN_xrCreateHandTrackerEXT pfnCreate_ = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroy_ = nullptr;
    PFN_xrLocateHandJointsEXT pfnLocate_ = nullptr;
    XrHandTrackerEXT tracker_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    HandJointSet hands_[2];
    bool supported_ = false;
    XrVector3f offset_ = {0.f, 0.f, 0.f};
    uint64_t locateCount_ = 0;
    uint64_t failLogged_ = 0;
};

//! Physical display space (where the joints are) -> the model's world.
struct HandRigMap {
    XrPosef displayPlane = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};  //!< XrViewDisplayRawDXR::displayPlanePose
    XrVector3f canvasOffsetM = {0.f, 0.f, 0.f};  //!< canvas centre - display centre, display metres
    XrPosef rig = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};  //!< the XrDisplayRigDXR pose the app chained
    float m2v = 1.f;                              //!< rig virtualDisplayHeight / canvas height (m)
    bool identity = true;                         //!< no rig: joints already in the model's space
};

//! Build the map for this frame from the locate's raw channel and the rig
//! the app chained. `raw` may be null (no rig / no raw channel) -> identity.
//! The display pixel/metre extents are XrDisplayInfoDXR's; they place the
//! canvas (XrViewDisplayRawDXR::canvasRectPx) on the panel.
HandRigMap MakeHandRigMap(const XrViewDisplayRawDXR* raw,
                          uint32_t displayPixelW, uint32_t displayPixelH,
                          float displayWidthM, float displayHeightM,
                          const XrPosef& rigPose, float rigVirtualDisplayHeight);

//! A point located in the hand's space -> the model's world.
XrVector3f HandRigMapPoint(const HandRigMap& map, const XrVector3f& p);

//! A joint radius (metres) -> world units.
inline float HandRigMapRadius(const HandRigMap& map, float radiusM) { return radiusM * map.m2v; }

} // namespace modelviewer

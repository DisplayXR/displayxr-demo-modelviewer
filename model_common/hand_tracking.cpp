// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  XR_EXT_hand_tracking consumer + display-space -> world map.
 */

#include "hand_tracking.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mv_log.h"

namespace modelviewer {

namespace {

XrVector3f quatRotate(const XrQuaternionf& q, const XrVector3f& v) {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const float tx = 2.f * (q.y * v.z - q.z * v.y);
    const float ty = 2.f * (q.z * v.x - q.x * v.z);
    const float tz = 2.f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

XrQuaternionf quatConjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

} // namespace

bool HandTracking::init(XrInstance instance, XrSystemId systemId, XrSession session, bool extensionEnabled) {
    destroy();
    if (!extensionEnabled || instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
        MV_LOG("hands: XR_EXT_hand_tracking not enabled - hand input off\n");
        return false;
    }

    // The runtime advertises the extension whenever it is built with it; the
    // SYSTEM says whether a hand-tracking-capable provider is actually present
    // (Ultraleap, sim_input, ...). Ask before creating anything.
    XrSystemHandTrackingPropertiesEXT ht = {XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES, &ht};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, systemId, &sp))) {
        supported_ = ht.supportsHandTracking == XR_TRUE;
    }
    if (!supported_) {
        MV_LOG("hands: system reports supportsHandTracking = false - hand input off "
               "(no provider with hand tracking; sim_input / Ultraleap not active)\n");
        return false;
    }

    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&pfnCreate_)) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)&pfnDestroy_)) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&pfnLocate_)) ||
        pfnCreate_ == nullptr || pfnDestroy_ == nullptr || pfnLocate_ == nullptr) {
        MV_ERR("hands: xrGetInstanceProcAddr for XR_EXT_hand_tracking failed - hand input off\n");
        pfnCreate_ = nullptr; pfnDestroy_ = nullptr; pfnLocate_ = nullptr;
        return false;
    }

    for (int h = 0; h < 2; ++h) {
        XrHandTrackerCreateInfoEXT ci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        const XrResult r = pfnCreate_(session, &ci, &tracker_[h]);
        if (XR_FAILED(r)) {
            MV_ERR("hands: xrCreateHandTrackerEXT(%s) failed: %d - hand input off\n",
                   h == 0 ? "left" : "right", (int)r);
            destroy();
            return false;
        }
    }
    MV_LOG("hands: XR_EXT_hand_tracking live - two trackers, %d joints each\n", XR_HAND_JOINT_COUNT_EXT);

    // Optional constant offset (see setOffset). Desktop legs only; Android
    // has no environment to read.
    if (const char* e = std::getenv("DXR_MODELVIEWER_HANDS_OFFSET_M")) {
        XrVector3f o = {0.f, 0.f, 0.f};
        if (std::sscanf(e, "%f,%f,%f", &o.x, &o.y, &o.z) == 3) {
            offset_ = o;
            MV_LOG("hands: DXR_MODELVIEWER_HANDS_OFFSET_M -> joints shifted by (%.3f, %.3f, %.3f) m\n", o.x, o.y, o.z);
        } else {
            MV_ERR("hands: DXR_MODELVIEWER_HANDS_OFFSET_M='%s' is not x,y,z - ignored\n", e);
        }
    }
    return true;
}

void HandTracking::destroy() {
    for (int h = 0; h < 2; ++h) {
        if (tracker_[h] != XR_NULL_HANDLE && pfnDestroy_ != nullptr) pfnDestroy_(tracker_[h]);
        tracker_[h] = XR_NULL_HANDLE;
        hands_[h].active = false;
    }
}

void HandTracking::locate(XrSpace base, XrTime time) {
    if (!enabled() || base == XR_NULL_HANDLE) return;
    ++locateCount_;
    for (int h = 0; h < 2; ++h) {
        XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = base;
        li.time = time;
        XrHandJointLocationsEXT loc = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        loc.jointCount = XR_HAND_JOINT_COUNT_EXT;
        loc.jointLocations = hands_[h].joints;
        const XrResult r = pfnLocate_(tracker_[h], &li, &loc);
        if (XR_FAILED(r)) {
            hands_[h].active = false;
            if (failLogged_++ < 3) MV_ERR("hands: xrLocateHandJointsEXT(%d) failed: %d\n", h, (int)r);
            continue;
        }
        hands_[h].active = loc.isActive == XR_TRUE;
        if (hands_[h].active && (offset_.x != 0.f || offset_.y != 0.f || offset_.z != 0.f)) {
            for (XrHandJointLocationEXT& j : hands_[h].joints) {
                j.pose.position.x += offset_.x;
                j.pose.position.y += offset_.y;
                j.pose.position.z += offset_.z;
            }
        }
    }
}

void HandTracking::toGestureSample(const HandJointSet& h, HandGestureSample& out) {
    out = HandGestureSample();
    if (!h.active) return;
    const XrHandJointLocationEXT& t = h.joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT& i = h.joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    if (!(t.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(i.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        return;
    }
    out.tracked = true;
    out.thumbTip[0] = t.pose.position.x; out.thumbTip[1] = t.pose.position.y; out.thumbTip[2] = t.pose.position.z;
    out.indexTip[0] = i.pose.position.x; out.indexTip[1] = i.pose.position.y; out.indexTip[2] = i.pose.position.z;
}

HandRigMap MakeHandRigMap(const XrViewDisplayRawDXR* raw,
                          uint32_t displayPixelW, uint32_t displayPixelH,
                          float displayWidthM, float displayHeightM,
                          const XrPosef& rigPose, float rigVirtualDisplayHeight) {
    HandRigMap m;
    if (raw == nullptr || raw->type != XR_TYPE_VIEW_DISPLAY_RAW_DXR || rigVirtualDisplayHeight <= 0.f) {
        return m;   // identity
    }
    m.identity = false;
    m.displayPlane = raw->displayPlanePose;
    m.rig = rigPose;

    // The canvas (window client area) the rig spans, placed on the panel.
    // canvasRectPx is top-down panel pixels; +Y up in display space.
    float canvasHeightM = raw->canvasSizeMeters.height;
    if (displayPixelW > 0 && displayPixelH > 0 && raw->canvasRectPx.extent.width > 0 &&
        raw->canvasRectPx.extent.height > 0) {
        const float cxPx = (float)raw->canvasRectPx.offset.x + 0.5f * (float)raw->canvasRectPx.extent.width;
        const float cyPx = (float)raw->canvasRectPx.offset.y + 0.5f * (float)raw->canvasRectPx.extent.height;
        const float mPerPxX = displayWidthM / (float)displayPixelW;
        const float mPerPxY = displayHeightM / (float)displayPixelH;
        m.canvasOffsetM.x = (cxPx - 0.5f * (float)displayPixelW) * mPerPxX;
        m.canvasOffsetM.y = -(cyPx - 0.5f * (float)displayPixelH) * mPerPxY;
        if (canvasHeightM <= 0.f) canvasHeightM = (float)raw->canvasRectPx.extent.height * mPerPxY;
    }
    if (canvasHeightM <= 0.f) canvasHeightM = displayHeightM;
    m.m2v = canvasHeightM > 0.f ? rigVirtualDisplayHeight / canvasHeightM : 1.f;
    return m;
}

XrVector3f HandRigMapPoint(const HandRigMap& map, const XrVector3f& p) {
    if (map.identity) return p;
    // locate space -> physical display space (display-centre origin).
    XrVector3f d = {p.x - map.displayPlane.position.x, p.y - map.displayPlane.position.y,
                    p.z - map.displayPlane.position.z};
    d = quatRotate(quatConjugate(map.displayPlane.orientation), d);
    // display centre -> canvas centre, then metres -> virtual units.
    d.x = (d.x - map.canvasOffsetM.x) * map.m2v;
    d.y = (d.y - map.canvasOffsetM.y) * map.m2v;
    d.z = (d.z - map.canvasOffsetM.z) * map.m2v;
    // canvas-local -> world through the virtual display plane (rig) pose.
    const XrVector3f r = quatRotate(map.rig.orientation, d);
    return {r.x + map.rig.position.x, r.y + map.rig.position.y, r.z + map.rig.position.z};
}

} // namespace modelviewer

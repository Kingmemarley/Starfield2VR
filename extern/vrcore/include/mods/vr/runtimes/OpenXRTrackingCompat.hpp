// vrcore - definitions for OpenXR body and hand tracking extensions.
// The pinned OpenXR SDK (1.0.22) predates XR_FB_body_tracking, XR_META_body_tracking_full_body,
// XR_META_body_tracking_fidelity and XR_EXT_hand_tracking_data_source. The types below are copied
// from the OpenXR 1.1.63 registry header and are only defined when the SDK headers lack them.
#pragma once

#include <openxr/openxr.h>

#ifndef XR_FB_body_tracking
#define XR_FB_body_tracking 1
XR_DEFINE_HANDLE(XrBodyTrackerFB)
#define XR_FB_BODY_TRACKING_EXTENSION_NAME "XR_FB_body_tracking"

typedef enum XrBodyJointSetFB {
    XR_BODY_JOINT_SET_DEFAULT_FB = 0,
    XR_BODY_JOINT_SET_FULL_BODY_META = 1000274000,
    XR_BODY_JOINT_SET_MAX_ENUM_FB = 0x7FFFFFFF
} XrBodyJointSetFB;

typedef struct XrBodyJointLocationFB {
    XrSpaceLocationFlags locationFlags;
    XrPosef pose;
} XrBodyJointLocationFB;

typedef struct XrSystemBodyTrackingPropertiesFB {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBool32 supportsBodyTracking;
} XrSystemBodyTrackingPropertiesFB;

typedef struct XrBodyTrackerCreateInfoFB {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    XrBodyJointSetFB bodyJointSet;
} XrBodyTrackerCreateInfoFB;

typedef struct XrBodyJointsLocateInfoFB {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    XrSpace baseSpace;
    XrTime time;
} XrBodyJointsLocateInfoFB;

typedef struct XrBodyJointLocationsFB {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBool32 isActive;
    float confidence;
    uint32_t jointCount;
    XrBodyJointLocationFB* jointLocations;
    uint32_t skeletonChangedCount;
    XrTime time;
} XrBodyJointLocationsFB;

typedef XrResult (XRAPI_PTR *PFN_xrCreateBodyTrackerFB)(XrSession session, const XrBodyTrackerCreateInfoFB* createInfo, XrBodyTrackerFB* bodyTracker);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyBodyTrackerFB)(XrBodyTrackerFB bodyTracker);
typedef XrResult (XRAPI_PTR *PFN_xrLocateBodyJointsFB)(XrBodyTrackerFB bodyTracker, const XrBodyJointsLocateInfoFB* locateInfo, XrBodyJointLocationsFB* locations);
#endif

#ifndef XR_META_body_tracking_full_body
#define XR_META_body_tracking_full_body 1
#define XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME "XR_META_body_tracking_full_body"

typedef struct XrSystemPropertiesBodyTrackingFullBodyMETA {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBool32 supportsFullBodyTracking;
} XrSystemPropertiesBodyTrackingFullBodyMETA;
#endif

#ifndef XR_META_body_tracking_fidelity
#define XR_META_body_tracking_fidelity 1
#define XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME "XR_META_body_tracking_fidelity"

typedef enum XrBodyTrackingFidelityMETA {
    XR_BODY_TRACKING_FIDELITY_LOW_META = 1,
    XR_BODY_TRACKING_FIDELITY_HIGH_META = 2,
    XR_BODY_TRACKING_FIDELITY_MAX_ENUM_META = 0x7FFFFFFF
} XrBodyTrackingFidelityMETA;

typedef struct XrSystemPropertiesBodyTrackingFidelityMETA {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBool32 supportsBodyTrackingFidelity;
} XrSystemPropertiesBodyTrackingFidelityMETA;

typedef struct XrBodyTrackingFidelityStatusMETA {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBodyTrackingFidelityMETA fidelity;
} XrBodyTrackingFidelityStatusMETA;

typedef XrResult (XRAPI_PTR *PFN_xrRequestBodyTrackingFidelityMETA)(XrBodyTrackerFB bodyTracker, const XrBodyTrackingFidelityMETA fidelity);
#endif

#ifndef XR_EXT_hand_tracking_data_source
#define XR_EXT_hand_tracking_data_source 1
#define XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME "XR_EXT_hand_tracking_data_source"

typedef enum XrHandTrackingDataSourceEXT {
    XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT = 1,
    XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT = 2,
    XR_HAND_TRACKING_DATA_SOURCE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrHandTrackingDataSourceEXT;

typedef struct XrHandTrackingDataSourceInfoEXT {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    uint32_t requestedDataSourceCount;
    XrHandTrackingDataSourceEXT* requestedDataSources;
} XrHandTrackingDataSourceInfoEXT;

typedef struct XrHandTrackingDataSourceStateEXT {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    XrBool32 isActive;
    XrHandTrackingDataSourceEXT dataSource;
} XrHandTrackingDataSourceStateEXT;
#endif

// Structure type and count values (plain constants, so they work whether or not the SDK's
// XrStructureType enum already lists them).
namespace vrcore_xr {
inline constexpr XrStructureType TYPE_BODY_TRACKER_CREATE_INFO_FB = (XrStructureType)1000076001;
inline constexpr XrStructureType TYPE_BODY_JOINTS_LOCATE_INFO_FB = (XrStructureType)1000076002;
inline constexpr XrStructureType TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB = (XrStructureType)1000076004;
inline constexpr XrStructureType TYPE_BODY_JOINT_LOCATIONS_FB = (XrStructureType)1000076005;
inline constexpr XrStructureType TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META = (XrStructureType)1000274000;
inline constexpr XrStructureType TYPE_BODY_TRACKING_FIDELITY_STATUS_META = (XrStructureType)1000284000;
inline constexpr XrStructureType TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META = (XrStructureType)1000284001;
inline constexpr XrStructureType TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT = (XrStructureType)1000428000;
inline constexpr XrStructureType TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT = (XrStructureType)1000428001;

inline constexpr uint32_t BODY_JOINT_COUNT_FB = 70;        // XR_BODY_JOINT_COUNT_FB
inline constexpr uint32_t FULL_BODY_JOINT_COUNT_META = 84; // XR_FULL_BODY_JOINT_COUNT_META
inline constexpr XrBodyJointSetFB BODY_JOINT_SET_DEFAULT = (XrBodyJointSetFB)0;
inline constexpr XrBodyJointSetFB BODY_JOINT_SET_FULL_BODY = (XrBodyJointSetFB)1000274000;
inline constexpr XrBodyTrackingFidelityMETA BODY_TRACKING_FIDELITY_HIGH = (XrBodyTrackingFidelityMETA)2;
} // namespace vrcore_xr

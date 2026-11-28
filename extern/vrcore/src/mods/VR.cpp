// vrcore - VR mod implementation.
// Runtime initialization, pose and input code adapted from REFramework's VR mod (MIT, praydog).
// The frame-index driven AFR synchronisation and the Starfield-facing API are new.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <utility/Module.hpp>

#include "Framework.hpp"
#include "ModSettings.h"
#include "mods/VR.hpp"
#include "mods/VRConfig.hpp"
#include "mods/vr/LaserSight.hpp"
#include "nvidia/UpscalerAfrNvidiaModule.h"

const std::string VR::actions_json = R"json(
{
  "actions": [
    { "name": "/actions/default/in/Pose", "type": "pose" },
    { "name": "/actions/default/in/Trigger", "type": "boolean" },
    { "name": "/actions/default/in/Grip", "type": "boolean" },
    { "name": "/actions/default/in/Joystick", "type": "vector2" },
    { "name": "/actions/default/in/JoystickClick", "type": "boolean" },
    { "name": "/actions/default/in/AButtonLeft", "type": "boolean" },
    { "name": "/actions/default/in/AButtonRight", "type": "boolean" },
    { "name": "/actions/default/in/BButtonLeft", "type": "boolean" },
    { "name": "/actions/default/in/BButtonRight", "type": "boolean" },
    { "name": "/actions/default/in/ThumbrestTouchLeft", "type": "boolean" },
    { "name": "/actions/default/in/ThumbrestTouchRight", "type": "boolean" },
    { "name": "/actions/default/in/DPad_Up", "type": "boolean" },
    { "name": "/actions/default/in/DPad_Right", "type": "boolean" },
    { "name": "/actions/default/in/DPad_Down", "type": "boolean" },
    { "name": "/actions/default/in/DPad_Left", "type": "boolean" },
    { "name": "/actions/default/in/SystemButton", "type": "boolean" },
    { "name": "/actions/default/in/BackButton", "type": "boolean" },
    { "name": "/actions/default/in/Touchpad", "type": "vector2" },
    { "name": "/actions/default/in/TouchpadClick", "type": "boolean" },
    { "name": "/actions/default/out/Haptic", "type": "vibration" }
  ],
  "action_sets": [
    { "name": "/actions/default", "usage": "single" }
  ],
  "default_bindings": [],
  "localization": [
    {
      "language_tag": "en_US",
      "/actions/default": "Default",
      "/actions/default/in/Pose": "Hand Pose",
      "/actions/default/in/Trigger": "Trigger",
      "/actions/default/in/Grip": "Grip",
      "/actions/default/in/Joystick": "Joystick",
      "/actions/default/in/JoystickClick": "Joystick Click",
      "/actions/default/in/AButtonLeft": "A Button (left hand, X on Touch)",
      "/actions/default/in/AButtonRight": "A Button (right hand)",
      "/actions/default/in/BButtonLeft": "B Button (left hand, Y on Touch)",
      "/actions/default/in/BButtonRight": "B Button (right hand)",
      "/actions/default/in/ThumbrestTouchLeft": "Thumbrest Touch (left)",
      "/actions/default/in/ThumbrestTouchRight": "Thumbrest Touch (right)",
      "/actions/default/in/DPad_Up": "DPad Up",
      "/actions/default/in/DPad_Right": "DPad Right",
      "/actions/default/in/DPad_Down": "DPad Down",
      "/actions/default/in/DPad_Left": "DPad Left",
      "/actions/default/in/SystemButton": "System/Menu Button",
      "/actions/default/in/BackButton": "View Button (Steam Frame)",
      "/actions/default/in/Touchpad": "Touchpad",
      "/actions/default/in/TouchpadClick": "Touchpad Click",
      "/actions/default/out/Haptic": "Haptic"
    }
  ]
}
)json";

std::shared_ptr<VR>& VR::get() {
    static auto inst = std::make_shared<VR>();
    return inst;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////////////////////////

std::optional<std::string> VR::on_initialize_d3d_thread() try {
    const auto requested = m_requested_runtime->value(); // 0 = auto, 1 = OpenXR, 2 = OpenVR

    auto try_openxr = [&]() {
        initialize_openxr();

        if (!m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }

        return m_openxr->loaded;
    };

    auto try_openvr = [&]() {
        initialize_openvr();

        if (!m_openvr->loaded) {
            m_openvr->is_hmd_active = false;
            m_openvr->was_hmd_active = false;
            m_openvr->needs_pose_update = false;
        }

        return m_openvr->loaded;
    };

    if (requested == 2) {
        try_openvr() || try_openxr();
    } else {
        try_openxr() || try_openvr();
    }

    if (!get_runtime()->loaded) {
        // Not fatal: the menu shows why VR is unavailable.
        spdlog::info("[VR] No VR runtime loaded");
        return std::nullopt;
    }

    m_init_finished = true;

    if (get_runtime()->is_openxr()) {
        m_openxr->resolution_scale = m_resolution_scale->value();

        if (auto e = initialize_openxr_swapchains()) {
            spdlog::error("[VR] {}", *e);
        }
    }

    return std::nullopt;
} catch (const std::exception& e) {
    spdlog::error("[VR] Exception during initialization: {}", e.what());
    return std::nullopt;
} catch (...) {
    spdlog::error("[VR] Unknown exception during initialization");
    return std::nullopt;
}

std::optional<std::string> VR::initialize_openvr() {
    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
        spdlog::info("[VR] Could not load openvr_api.dll");

        m_openvr->dll_missing = true;
        m_openvr->error = "Could not load openvr_api.dll";
        return std::nullopt;
    }

    m_d3d12.on_reset(this);

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    auto error = vr::VRInitError_None;
    m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return std::nullopt;
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return std::nullopt;
    }

    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return std::nullopt;
    }

    if (auto input_error = initialize_openvr_input()) {
        m_openvr->error = *input_error;
        return std::nullopt;
    }

    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openvr_input() {
    const auto dir = Framework::get_persistent_dir();
    std::filesystem::create_directories(dir);

    const auto actions_path = dir / "actions.json";

    {
        std::ofstream file{actions_path};
        file << actions_json;
    }

    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None || m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None || it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    m_openxr = std::make_shared<runtimes::OpenXR>();
    m_openxr->resolution_scale = m_resolution_scale->value(); // (also when the runtime is set up again)

    spdlog::info("[VR] Initializing OpenXR");

    if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
        spdlog::info("[VR] Could not load openxr_loader.dll");

        m_openxr->loaded = false;
        m_openxr->dll_missing = true;
        m_openxr->error = "Could not load openxr_loader.dll";

        return std::nullopt;
    }

    m_d3d12.on_reset(this);

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    XrResult result{XR_SUCCESS};

    // Step 1: instance
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{XR_KHR_D3D12_ENABLE_EXTENSION_NAME};

        // Body and hand tracking are enabled when the runtime offers them (Quest via Virtual
        // Desktop or Link); they are only used by the shown body.
        std::vector<XrExtensionProperties> available{};
        uint32_t extension_count = 0;
        if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr) == XR_SUCCESS && extension_count > 0) {
            available.resize(extension_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
            if (xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, available.data()) != XR_SUCCESS) {
                available.clear();
            }
        }
        const auto enable_if_available = [&](const char* name) {
            for (const auto& extension : available) {
                if (std::string_view{extension.extensionName} == name) {
                    extensions.push_back(name);
                    return true;
                }
            }
            return false;
        };
        runtimes::OpenXR::ext_body_tracking = enable_if_available(XR_FB_BODY_TRACKING_EXTENSION_NAME);
        runtimes::OpenXR::ext_body_full_body = runtimes::OpenXR::ext_body_tracking && enable_if_available(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
        runtimes::OpenXR::ext_body_fidelity = runtimes::OpenXR::ext_body_tracking && enable_if_available(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME);
        runtimes::OpenXR::ext_hand_tracking = enable_if_available(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        runtimes::OpenXR::ext_hand_data_source = runtimes::OpenXR::ext_hand_tracking && enable_if_available(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
        // The menu panel's curve (a cylinder layer); without it the panel is flat.
        runtimes::OpenXR::ext_cylinder = enable_if_available(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        // (fix205) The Steam Frame's controllers' own profile (SteamVR): their bindings are suggested only with it.
        runtimes::OpenXR::ext_frame_controller = enable_if_available(runtimes::OpenXR::kFrameControllerExtension);
        {
            std::string available_names;
            for (const auto& extension : available) {
                available_names += " ";
                available_names += extension.extensionName;
            }
            spdlog::info("[VR] OpenXR runtime offers:{}", available_names);
        }
        {
            std::string enabled_names;
            for (const auto* name : extensions) {
                enabled_names += " ";
                enabled_names += name;
            }
            spdlog::info("[VR] OpenXR extensions: {} available, enabling:{}", available.size(), enabled_names);
        }

        // (fix197) The OpenXR API layers the loader puts between the game and the runtime (implicit ones load by themselves: overlays,
        // frame generation, the runtime's own helpers), for the log, once each time an instance is made; VectorXR's noted (its status file
        // then read now and then: start_vectorxr_watch).
        {
            std::vector<XrApiLayerProperties> found{};
            uint32_t count = 0;
            if (xrEnumerateApiLayerProperties(0, &count, nullptr) == XR_SUCCESS && count > 0 && count < 256) {
                found.resize(count, XrApiLayerProperties{XR_TYPE_API_LAYER_PROPERTIES});
                if (xrEnumerateApiLayerProperties(count, &count, found.data()) != XR_SUCCESS) {
                    found.clear();
                } else {
                    found.resize(std::min<size_t>(count, found.size()));
                }
            }
            std::string names{};
            bool vectorxr = false;
            for (const auto& layer : found) {
                const std::string name{layer.layerName};
                names += std::format("{}{} (spec {}.{}.{}, version {})", names.empty() ? "" : "; ", name, XR_VERSION_MAJOR(layer.specVersion),
                    XR_VERSION_MINOR(layer.specVersion), XR_VERSION_PATCH(layer.specVersion), layer.layerVersion);
                std::string upper = name;
                std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });
                vectorxr = vectorxr || upper.find("VECTORXR") != std::string::npos;
            }
            static int layer_logs = 0;
            if (layer_logs++ < 8) {
                spdlog::info("[VR] (fix197) OpenXR API layers: {} (implicit layers load by themselves; explicit ones listed here load only when "
                             "enabled)", found.empty() ? std::string{"none listed (no layers, or the list could not be read)"} : names);
            }
            m_vectorxr_layer = vectorxr;
            // (fix200) Its Frame Generation taken as possibly running from the start (it is decided as the session is made, before its
            // status file says anything): the Virtual Desktop fix's empty layer then never goes before the world's picture. Cleared only
            // when a status file of a session first seen after this instance says Frame Generation is off for this game (the session
            // named now is an earlier instance's: check_vectorxr_status).
            m_vectorxr_instance_session = m_vectorxr_last_session.load();
            runtimes::OpenXR::vectorxr_framegen = vectorxr;
            if (vectorxr) {
                spdlog::info("[VR] (fix197) VectorXR's layer is present: its status file is read every 3 s (Frame Generation's state logged as it "
                             "changes)");
                start_vectorxr_watch();
            }
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        strcpy_s(instance_create_info.applicationInfo.applicationName, g_framework->get_game_name());
        strcpy_s(instance_create_info.applicationInfo.engineName, "vrcore");
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            spdlog::error("[VR] {}", m_openxr->error.value());
            return std::nullopt;
        }

        // (fix200) Its layer not in the loader's list (that list can leave out a layer, or not be readable) but its library loaded into the
        // game by the loader all the same (the library its manifest names): taken as there, as above.
        if (!m_vectorxr_layer.load() && GetModuleHandleW(L"XR_APILAYER_DIENERTECH_VECTORXR.dll") != nullptr) {
            m_vectorxr_layer = true;
            runtimes::OpenXR::vectorxr_framegen = true;
            spdlog::info("[VR] (fix200) VectorXR's layer is loaded (its library is in the game, though not in the loader's list): its status file is "
                         "read every 3 s, and its Frame Generation taken as possibly running");
            start_vectorxr_watch();
        }
    }

    // Step 2: system
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());
            return std::nullopt;
        }
    }

    // Step 3: session
    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};
    m_d3d12.openxr().initialize(session_create_info);

    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());
        return std::nullopt;
    }

    // Step 4: spaces
    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());
            return std::nullopt;
        }
    }

    if (m_openxr->floor_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;
        if (xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->floor_space) != XR_SUCCESS) {
            m_openxr->floor_space = XR_NULL_HANDLE;
            spdlog::warn("[VR] No floor-level stage space; eye height unavailable");
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());
            return std::nullopt;
        }
    }

    // Step 5: system properties (logging, and whether body/hand tracking is supported)
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemBodyTrackingPropertiesFB body_properties{vrcore_xr::TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB};
    XrSystemPropertiesBodyTrackingFullBodyMETA full_body_properties{vrcore_xr::TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META};
    XrSystemPropertiesBodyTrackingFidelityMETA fidelity_properties{vrcore_xr::TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META};
    XrSystemHandTrackingPropertiesEXT hand_properties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    const auto chain = [&](auto& props) {
        props.next = system_properties.next;
        system_properties.next = &props;
    };
    if (runtimes::OpenXR::ext_body_tracking) chain(body_properties);
    if (runtimes::OpenXR::ext_body_full_body) chain(full_body_properties);
    if (runtimes::OpenXR::ext_body_fidelity) chain(fidelity_properties);
    if (runtimes::OpenXR::ext_hand_tracking) chain(hand_properties);

    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());
        return std::nullopt;
    }

    spdlog::info("[VR] OpenXR system: {} (vendor {})", system_properties.systemName, system_properties.vendorId);
    // (fix205) The runtime's own name and version (SteamVR says "SteamVR/OpenXR", Virtual Desktop's "VirtualDesktopXR"), kept with the
    // system's name for the headset report (OpenXR::report_headset).
    {
        XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (const auto ip = xrGetInstanceProperties(m_openxr->instance, &instance_properties); ip == XR_SUCCESS) {
            m_openxr->runtime_name = std::format("{} {}.{}.{}", instance_properties.runtimeName, XR_VERSION_MAJOR(instance_properties.runtimeVersion),
                XR_VERSION_MINOR(instance_properties.runtimeVersion), XR_VERSION_PATCH(instance_properties.runtimeVersion));
        } else {
            m_openxr->runtime_name = "? (" + m_openxr->get_result_string(ip) + ")";
        }
        m_openxr->system_name = system_properties.systemName;
        spdlog::info("[Headset] (fix205) OpenXR runtime: {}; system: {}", m_openxr->runtime_name, m_openxr->system_name);
        // (fix205) A Pimax headset (either name, any case): the notice (OpenXR.hpp's kPimaxNotice).
        {
            auto lower = m_openxr->runtime_name + " " + m_openxr->system_name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (lower.find("pimax") != std::string::npos) {
                m_openxr->pimax_since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                m_openxr->pimax = true;
                spdlog::warn("[Headset] (fix205) {}", runtimes::OpenXR::kPimaxNotice);
            }
        }
    }
    spdlog::info("[VR] OpenXR max swapchain {}x{}, {} layers", system_properties.graphicsProperties.maxSwapchainImageWidth,
        system_properties.graphicsProperties.maxSwapchainImageHeight, system_properties.graphicsProperties.maxLayerCount);
    // (fix177) Kept: build mode's console adds its quads only while the frame stays within it.
    m_openxr->max_layers = std::max<uint32_t>(system_properties.graphicsProperties.maxLayerCount, 2u);

    m_openxr->body_tracking_supported = runtimes::OpenXR::ext_body_tracking && body_properties.supportsBodyTracking == XR_TRUE;
    m_openxr->body_full_body_supported = m_openxr->body_tracking_supported && runtimes::OpenXR::ext_body_full_body &&
                                         full_body_properties.supportsFullBodyTracking == XR_TRUE;
    m_openxr->body_fidelity_supported = m_openxr->body_tracking_supported && runtimes::OpenXR::ext_body_fidelity &&
                                        fidelity_properties.supportsBodyTrackingFidelity == XR_TRUE;
    m_openxr->hand_tracking_supported = runtimes::OpenXR::ext_hand_tracking && hand_properties.supportsHandTracking == XR_TRUE;
    spdlog::info("[VR] Body tracking: supported {} full body {} fidelity {} | hand tracking: supported {}", m_openxr->body_tracking_supported,
        m_openxr->body_full_body_supported, m_openxr->body_fidelity_supported, m_openxr->hand_tracking_supported);
    m_openxr->initialize_body_hand_tracking();

    // Step 6: views
    m_openxr->update_render_target_size();

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());
        return std::nullopt;
    }

    m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());
        return std::nullopt;
    }

    detect_controllers();

    // When reinitializing, the config was already loaded, so swapchains can be made right away.
    if (m_init_finished) {
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        return err;
    }

    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (ActionHandle)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Mapped action {} to {}", it.first, openxr_action_name);
        } else {
            spdlog::warn("[VR] Action {} has no OpenXR equivalent", it.first);
        }
    }

    m_left_joystick = (SourceHandle)VRRuntime::Hand::LEFT;
    m_right_joystick = (SourceHandle)VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchains");

    if (auto err = m_d3d12.openxr().create_swapchains()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());
        return m_openxr->error;
    }

    return std::nullopt;
}

std::optional<std::string> VR::reinitialize_openvr() {
    spdlog::info("[VR] Reinitializing OpenVR");
    std::scoped_lock _{m_openvr_mtx};

    m_runtime.reset();
    m_runtime = std::make_shared<VRRuntime>();
    m_openvr.reset();

    m_controllers.clear();
    m_controllers_set.clear();

    auto e = initialize_openvr();

    if (e) {
        spdlog::error("[VR] Failed to reinitialize OpenVR: {}", *e);
    }

    return e;
}

std::optional<std::string> VR::reinitialize_openxr() {
    spdlog::info("[VR] Reinitializing OpenXR");
    std::scoped_lock _{m_openvr_mtx};

    m_d3d12.openxr().destroy_reticle();
    m_d3d12.openxr().destroy_panel();
    m_d3d12.openxr().destroy_ambient(); // (fix164)
    m_d3d12.openxr().destroy_conversation();
    m_d3d12.openxr().destroy_wheel(); // (fix171)
    m_d3d12.openxr().destroy_console(); // (fix177)
    m_d3d12.openxr().destroy_fw_menu(); // (fix196)
    m_d3d12.openxr().destroy_swapchains();

    m_openxr.reset();
    m_runtime.reset();
    m_runtime = std::make_shared<VRRuntime>();

    m_controllers.clear();
    m_controllers_set.clear();

    auto e = initialize_openxr();

    if (e) {
        spdlog::error("[VR] Failed to reinitialize OpenXR: {}", *e);
    }

    return e;
}

bool VR::detect_controllers() {
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        if (vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick) != vr::VRInputError_None) {
            return false;
        }

        if (vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick) != vr::VRInputError_None) {
            return false;
        }

        vr::InputOriginInfo_t left_info{};
        vr::InputOriginInfo_t right_info{};

        const auto left_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_info, sizeof(left_info));
        const auto right_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_info, sizeof(right_info));

        if (left_error != vr::VRInputError_None || right_error != vr::VRInputError_None) {
            return false;
        }

        m_controllers.push_back(left_info.trackedDeviceIndex);
        m_controllers.push_back(right_info.trackedDeviceIndex);
        m_controllers_set.insert(left_info.trackedDeviceIndex);
        m_controllers_set.insert(right_info.trackedDeviceIndex);

        spdlog::info("[VR] Left hand: {}, right hand: {}", left_info.trackedDeviceIndex, right_info.trackedDeviceIndex);
    } else if (get_runtime()->is_openxr()) {
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Frame synchronisation (AFR: even engine frames render the left eye, odd frames the right eye)
//////////////////////////////////////////////////////////////////////////////////////////////////

void VR::on_wait_rendering(uint32_t frame_index) {
    // (fix179) When this game frame began, for the menus' slow frames (D3D12Component::log_slow_frame reads the last 32).
    {
        const uint32_t n = m_game_frames.load(std::memory_order_relaxed);
        m_game_frame_us[n % m_game_frame_us.size()].store(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        m_game_frames.store(n + 1, std::memory_order_release);
    }
    if (!get_runtime()->loaded) {
        return;
    }

    // Toggle requests (from the controller or a key, on other threads) are applied here, on the game thread.
    // Each one moves to the next of stereo, mono and AER (fix136; mono and AER need OpenXR).
    if (const int toggles = m_mono_toggles.exchange(0); toggles > 0) {
        // (fix196) AER only with "Include AER in the eye-mode button" on (off by default): then stereo, mono, AER in turn as before; else
        // stereo and mono. AER chosen in the menu (or saved from before) goes to stereo at the next press either way, as it did. One step a
        // press (a few at most in one frame).
        const bool with_aer = m_aer_in_button->value();
        for (int i = 0; i < std::min(toggles, 6); ++i) {
            if (m_mono->value()) {
                m_mono->value() = false;
                m_aer->value()  = with_aer;
            } else if (m_aer->value()) {
                m_aer->value() = false;
            } else {
                m_mono->value() = true;
            }
        }
        // (fix165) Not to two pictures (stereo, AER) while video memory has been over its budget 3 s: a second picture and DLSS instance
        // would push more of the game's data out to system memory (a pilot's seat at about 10 fps in testing; a helper's research).
        if (!m_mono->value() && m_d3d12.memory_over_for(3000) && get_runtime()->is_openxr()) {
            const char* asked = m_aer->value() ? "AER" : "stereo";
            m_mono->value()   = true;
            m_aer->value()    = false;
            // (fix173) The click's buzz said the switch was made: two short strong pulses after it instead (refused_buzz), so it is felt.
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t since  = m_d3d12.m_mem_over_since_ms.load();
            const int64_t over_s = since != 0 ? (now_ms - since) / 1000 : 0;
            m_refused_buzz_step  = 1;
            m_refused_buzz_at    = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
            spdlog::info("[VR] (fix173) the switch to {} refused, kept in mono: video memory over its budget for {} s ({} of {} MB); two short "
                         "buzzes. Switch again once it has been under its budget 2 s (the log's \"back under its budget\")",
                asked, over_s, m_d3d12.m_mem_used_mb.load(), m_d3d12.m_mem_budget_mb.load());
        }
        spdlog::info("[VR] the eyes now: {} (from the next frame pair){}", m_mono->value() ? "mono" : (m_aer->value() ? "AER" : "stereo"),
            get_runtime()->is_openxr() ? "" : " - mono and AER need OpenXR; this runtime stays in stereo");
    }
    // (fix173) A refused switch's two short strong pulses in both hands: what the click's buzz still plays stopped first, then a pulse, and
    // another 0.2 s on (the click's own buzz, ModConfig's, is one long or short pulse: this is told apart by its two).
    if (m_refused_buzz_asked.exchange(false)) { // (fix198, the review: a menu's refusal, from its own thread)
        m_refused_buzz_step = 1;
        m_refused_buzz_at   = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    }
    if (m_refused_buzz_step > 0 && std::chrono::steady_clock::now() >= m_refused_buzz_at) {
        if (const auto xr = m_openxr; xr != nullptr && get_runtime()->is_openxr() && is_using_controllers()) { // (a copy: it may be made anew on another thread)
            for (const auto hand : {VRRuntime::Hand::LEFT, VRRuntime::Hand::RIGHT}) {
                xr->stop_haptic_vibration(hand);
                xr->trigger_haptic_vibration(0.06f, 0.0f, 1.0f, hand);
            }
        }
        m_refused_buzz_at   = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        m_refused_buzz_step = m_refused_buzz_step == 1 ? 2 : 0;
    }

    // A game menu shown (on the menu panel): mono while it is, if set (MonoInMenus), and back as it was in play once it closes. Held
    // a moment after the last frame with a menu, so a menu's tabs (each a menu of its own) do not switch it back and forth.
    {
        const auto now = std::chrono::steady_clock::now();
        if (ModSettings::g_internalSettings.showQuadDisplay) {
            m_menu_seen_at = now;
        }
        // (fix163) With full stereo to come back to: held after the menu until its pictures are let go (at most 3 s), so that memory is
        // free before the second eye and the game's two new DLSS instances start (stereo glitches after unpausing, cured by
        // going through AER and mono: video memory over its budget, full stereo at half speed; a helper's research and the review).
        const auto since     = now - m_menu_seen_at;
        const bool held      = full_stereo_requested() && since < std::chrono::seconds(3) && m_d3d12.menu_release_pending();
        const bool menu_mono = m_mono_in_menus->value() && m_menu_seen_at != std::chrono::steady_clock::time_point{} &&
                               (since < std::chrono::milliseconds(400) || held);
        if (menu_mono != m_menu_mono) {
            m_menu_mono = menu_mono;
            if (!m_mono->value()) {
                spdlog::info("[VR] {}", menu_mono ? "a menu is shown: mono while it is (MonoInMenus)" : "the menu closed: back as in play");
            }
        }
    }

    // Mono or stereo: changed only at the start of a left/right pair (the frame before it has been presented, see below), and
    // remembered for this frame so its present is done the same way. Full stereo likewise (both eyes in every frame, the game
    // side's second view): only in stereo, with the setting on and the game side ready for it.
    if (eye_for_frame(frame_index) == VRRuntime::Eye::LEFT) {
        // (AER with a menu shown: mono, MonoInMenus or not: the panel shows the eyes' pictures from the head's middle, fix136's review.)
        const bool menu_shown = m_menu_seen_at != std::chrono::steady_clock::time_point{} &&
                                std::chrono::steady_clock::now() - m_menu_seen_at < std::chrono::milliseconds(400);
        const bool mono       = (m_mono->value() || m_menu_mono || (m_aer->value() && menu_shown)) && get_runtime()->is_openxr();
        // (fix165) In mono with video memory over its budget 3 s: the right eye's DLSS instance let go (made again at its first use).
        // (fix173) Also as soon as a menu is shown in mono (MonoInMenus, or mono anyway): its instance is not used until the menu closes (the
        // game makes new DLSS instances around menus anyway), and the menu's own pictures and views need the memory as it opens (the Data
        // menu went 300 MB over its budget for 3 s as it opened, 40-141 ms frames). Still only 2 s after the right eye's last use (the GPU
        // done with it: UpscalerAfrNvidiaModule).
        const bool menu_mono_now = mono && menu_shown;
        UpscalerAfrNvidiaModule::set_right_unneeded(mono && (menu_mono_now || m_d3d12.memory_over_for(3000)), menu_mono_now);
        if (mono != m_mono_active.load()) {
            m_mono_active = mono;
            spdlog::info("[VR] {} from frame {}", mono ? "mono: one picture for both eyes, a new one every frame (no stereo depth)"
                                                       : "stereo: the eyes take turns, each a new picture every other frame", frame_index);
        }
        const bool aer = !mono && m_aer->value() && get_runtime()->is_openxr();
        if (aer != m_aer_active.load()) {
            m_aer_active = aer;
            spdlog::info("[VR] {} from frame {}", aer ? "AER: one eye drawn each frame, each a headset frame of its own (the other eye's last picture with it)"
                                                      : "AER off", frame_index);
        }
        const bool native = !mono && !aer && m_full_stereo->value() && get_runtime()->is_openxr() && m_native_possible.load();
        if (native != m_native_active.load()) {
            m_native_active = native;
            static int told = 0;
            if (told++ < 20) {
                spdlog::info("[VR] {} from frame {}", native ? "full stereo: both eyes drawn in every frame (two views of the world)"
                                                             : (mono ? "full stereo off (mono)" : "full stereo off: the eyes take turns again"), frame_index);
            }
        }
    }
    m_frame_modes[frame_index % m_frame_modes.size()] = ((uint64_t)frame_index << 3) | (m_aer_active.load() ? 4u : 0u) |
                                                        (m_native_active.load() ? 2u : 0u) | (m_mono_active.load() ? 1u : 0u);

    if (!is_hmd_active()) {
        return;
    }

    // Before starting a new left/right pair, wait until the previous pair was handed to the
    // runtime (xrEndFrame), otherwise xrBeginFrame would discard it.
    // Presents stopped reaching us (device reset, framework re-initializing): don't wait,
    // otherwise every left-eye frame would stall for the full timeout.
    const auto since_present = std::chrono::steady_clock::now().time_since_epoch().count() - m_last_present_time_ns.load();

    // (250 ms: a hitch of the render thread alone, a shader compiling, is waited for: starting the pair early would drop its picture.)
    if (since_present > std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(250)).count()) {
        SetEvent(m_present_finished_event);
        return;
    }

    // Pipelined pacing (fix136): no wait for the frame before (the present thread opens each headset frame after ending the one before,
    // so none is discarded); only kept from running more than two frames ahead of the last one handed over (each frame's records,
    // its mode, poses and the laser's line, are kept for the last eight).
    if (pipelined()) {
        // (Full stereo: kept in step, each frame begun once the one before was handed over, as before fix136: the left eye's picture is
        // copied as the frame is drawn, labelled with the frame the game reports drawing, which with the game frames ahead was often the
        // next one: half the frames shown flat in fix136's test. fix137.)
        const int32_t limit = is_native() ? 0 : 2;
        const int     cap   = is_native() ? 250 : 100;
        const auto ahead = [&] { return (int32_t)(frame_index - 1 - m_last_finished_present.load()); };
        if (m_present_index_ok.load() && ahead() > limit && ahead() < 64) {
            const auto start = std::chrono::steady_clock::now();
            ++m_stats.ahead_waits;
            while (ahead() > limit && m_present_index_ok.load()) {
                if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(cap)) {
                    break;
                }
                const auto quiet = std::chrono::steady_clock::now().time_since_epoch().count() - m_last_present_time_ns.load();
                if (quiet > std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(250)).count()) {
                    break;
                }
                WaitForSingleObject(m_present_finished_event, 2);
                ResetEvent(m_present_finished_event);
            }
            const auto ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            m_stats.longest_pair_wait_ms = std::max(m_stats.longest_pair_wait_ms, ms);
        }
        return;
    }

    // (Mono, full stereo and AER: every frame is a headset frame of its own, so every frame waits for the one before to be handed over.)
    if (!is_single() && eye_for_frame(frame_index) != VRRuntime::Eye::LEFT) {
        return;
    }

    // Wait by frame index: until the present of frame N-1 (the right eye of the previous pair) has finished. The event alone
    // is only a yes/no signal: one extra "set" (a timeout, a hitch, the loading screen where presents pause) let every later
    // pair start one present too early, for good. xrBeginFrame of the new pair then came before the previous right eye was
    // handed over, so that right eye ended the new frame and the left eye's picture was dropped: a black screen with
    // flashes, until pausing or mono happened to put it back in step. The index can't drift like that.
    const uint32_t want   = frame_index - 1;
    const auto     behind = [&] { return (int32_t)(want - m_last_finished_present.load()); };

    // Index usable: presents carry the engine's index, and it is close (a large gap, or presents ahead of this frame, means the
    // counts are not comparable, e.g. right after a reset; then use the event as before). Should waiting by index ever time out again and again (3 times
    // in 10 s: something this did not foresee), it is left off for 30 s and the event used as before.
    static int64_t index_off_until_ns = 0;
    static int64_t recent_timeouts_ns[3]{};
    static int     next_timeout = 0;
    const int64_t  now_ns       = std::chrono::steady_clock::now().time_since_epoch().count();
    if (const int32_t gap = behind(); m_present_index_ok.load() && gap >= 0 && gap <= 4 && now_ns >= index_off_until_ns) {
        if (behind() > 0) {
            const auto start = std::chrono::steady_clock::now();
            ++m_stats.pair_waits;

            // The event was already set although frame N-1 was not presented: the old wait would have gone ahead here.
            if (WaitForSingleObject(m_present_finished_event, 0) == WAIT_OBJECT_0) {
                ++m_stats.early_signals;
                static int logged = 0;
                if (logged < 10) {
                    ++logged;
                    spdlog::info("[VR] frame {}: the 'presented' signal was set before frame {} was presented (last presented {}); "
                                 "waiting for it by index so both eyes stay in step", frame_index, want, m_last_finished_present.load());
                }
            }

            while (behind() > 0 && m_present_index_ok.load()) { // (a present without the index: the event decides next time)
                const auto waited = std::chrono::steady_clock::now() - start;
                if (waited >= std::chrono::milliseconds(333)) {
                    ++m_stats.pair_wait_timeouts;
                    spdlog::warn("[VR] Timed out waiting for frame {} to be presented (last presented {})", want, m_last_finished_present.load());
                    const int64_t t = std::chrono::steady_clock::now().time_since_epoch().count();
                    const int64_t oldest = recent_timeouts_ns[next_timeout];
                    recent_timeouts_ns[next_timeout] = t;
                    next_timeout = (next_timeout + 1) % 3;
                    if (oldest != 0 && t - oldest < std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(10)).count()) {
                        index_off_until_ns = t + std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(30)).count();
                        spdlog::warn("[VR] waiting for the frame before by index timed out 3 times in 10 s: using the old 'presented' signal "
                                     "for the next 30 s");
                    }
                    break;
                }

                // Presents stopped (loading, device reset): don't hold the game.
                const auto quiet = std::chrono::steady_clock::now().time_since_epoch().count() - m_last_present_time_ns.load();
                if (quiet > std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(250)).count()) {
                    break;
                }

                // Woken by the right eye's present (it sets the event after recording its index); short slices so a missed
                // wake-up costs at most a few ms.
                WaitForSingleObject(m_present_finished_event, 4);
                ResetEvent(m_present_finished_event);
            }

            const auto ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            m_stats.longest_pair_wait_ms = std::max(m_stats.longest_pair_wait_ms, ms);
        }

        ResetEvent(m_present_finished_event);
        return;
    }

    if (WaitForSingleObject(m_present_finished_event, 333) == WAIT_TIMEOUT) {
        spdlog::warn("[VR] Timed out waiting for the previous frame to be submitted");
    }

    ResetEvent(m_present_finished_event);
}

void VR::on_begin_rendering(uint32_t frame_index) {
    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    // (fix189) With OpenXR this is asked for by the runtime's XR_SESSION_STATE_LOSS_PENDING (and the bindings editor's saves): the remake
    // frees the runtime while the present thread may still be using it (on_present's get_runtime(), its needs_pose_update after on_frame),
    // so it can crash. Left as it was for a lost session (nothing would work on without a remake); the F11 button is not offered with OpenXR.
    if (runtime->wants_reinitialize) {
        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }

        runtime = get_runtime();

        if (!runtime->loaded) {
            return;
        }
    }

    detect_controllers();

    if (tracing()) {
        trace("begin(camera)", frame_index, (int)eye_for_frame(frame_index));
    }

    // (Pipelined: the present thread polls the events, as it opens and ends the headset frames: the session's state is its.)
    if (!(pipelined() && runtime->is_openxr()) && (is_single() || eye_for_frame(frame_index) == VRRuntime::Eye::LEFT)) {
        runtime->consume_events(nullptr);
    }

    // (fix198, the review) A menu's sharpness: applied once it has not changed for 750 ms (each step of the VR tab's stepper would remake
    // the headset's images otherwise) and no game menu is shown (in the game's Settings it is applied as the menu closes; at the main menu,
    // once a save is loaded and play shows).
    if (const float asked = m_res_asked_value.load(); asked > 0.0f && runtime->is_openxr()) {
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - m_res_asked_ms.load() >= 750 && !ModSettings::g_internalSettings.showQuadDisplay) {
            float expected = asked;
            if (m_res_asked_value.compare_exchange_strong(expected, -1.0f)) { // (taken: a newer value waits its own 750 ms)
                if (const auto xr = m_openxr; xr != nullptr && xr->resolution_scale != m_resolution_scale->value()) {
                    m_wants_swapchain_rebuild = true;
                }
            }
        }
    }

    if (m_wants_swapchain_rebuild && runtime->is_openxr()) {
        // A new resolution scale. Only the size the game is asked to render at changes here: the game window is resized to
        // it (CreationEngineRendererModule::SetWindowSize), and the headset images follow the game's backbuffer on the
        // present thread (D3D12Component::OpenXR::copy), between frames. Destroying and remaking them here, on the game
        // thread, raced the present thread still using them (and remade the copiers mid-use): a crash or a hang.
        m_wants_swapchain_rebuild = false;
        const float scale = m_resolution_scale->value();
        if (m_openxr->resolution_scale != scale) {
            m_openxr->resolution_scale = scale;
            spdlog::info("[VR] resolution scale {:.2f}: each eye renders at {}x{}{}", scale, get_hmd_width(), get_hmd_height(),
                ModSettings::g_debug.disableWindowResize ? " (window resizing is off in Troubleshooting, so the game keeps its size)"
                                                         : " (the game window is resized to it)");
        }
    }
}

void VR::update_hmd_state(uint32_t frame_index) {
    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    // Pipelined pacing (fix136): no xrWaitFrame or xrBeginFrame here, the present thread opens the headset frames. This frame's poses
    // located for the display time it will be shown at: the last one the present thread was given (the frame open now, which the next
    // frame handed over goes into), plus a headset frame for each frame, or pair, before this one not handed over yet.
    if (pipelined() && runtime->is_openxr()) {
        m_openxr->pipelined = true;
        if (is_single() || eye_for_frame(frame_index) == VRRuntime::Eye::LEFT || !runtime->got_first_poses) {
            const XrTime     base   = m_openxr->published_display_time.load();
            const XrDuration period = m_openxr->published_period.load();
            if (base > 0 && period > 0) {
                const int32_t before = std::clamp((int32_t)(frame_index - 1 - m_last_finished_present.load()), 0, 8);
                const int32_t k      = std::clamp(is_single() ? before : (before + 1) / 2, 0, 3);
                m_openxr->pose_display_time = base + (XrTime)k * period;
                ++m_stats.hmd_syncs;
                runtime->update_poses();
            }
        }
        // (Kept for this frame, the pose its pictures are submitted with; a pair's right eye keeps its left eye's.)
        m_openxr->store_poses(frame_index);
    } else if (is_single() || eye_for_frame(frame_index) == VRRuntime::Eye::LEFT || !runtime->got_first_poses) {
        if (runtime->is_openxr()) {
            m_openxr->pipelined = false;
        }
        // Both eyes of a pair use the same predicted pose; only sync on the left eye (mono, full stereo, AER: every frame).
        ++m_stats.hmd_syncs;

        if (runtime->is_openxr()) {
            if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                ++m_stats.hmd_sync_failures;
                return;
            }

            m_openxr->begin_frame();
        } else if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
            ++m_stats.hmd_sync_failures;
            return;
        }

        runtime->update_poses();
    }
    // (The old pacing too: each frame's poses kept, for AER's eyes, each submitted with its own.)
    if (runtime->is_openxr() && !pipelined()) {
        m_openxr->store_poses(frame_index);
    }

    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{runtime->pose_mtx};
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(0);
        runtime->wants_reset_origin = false;
        // (The headset's own recenter: an open menu's panel is put in front of you again; fix196: the mod's own menu's too.)
        m_openxr->panel_recenter = true;
        m_openxr->fw_menu_recenter = true;
    }

    runtime->update_matrices(m_nearz, m_farz);
    update_frustums();

    runtime->got_first_poses = true;
}

void VR::update_frustums() {
    auto runtime = get_runtime();

    std::unique_lock _{runtime->eyes_mtx};

    for (auto i = 0; i < 2; ++i) {
        Vector4f f{};

        if (runtime->is_openxr()) {
            if (m_openxr->views.size() <= (size_t)i) {
                continue;
            }

            const auto& fov = m_openxr->views[i].fov;
            f = Vector4f{tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleUp), tanf(fov.angleDown)};
        } else if (runtime->is_openvr()) {
            // OpenVR's raw projection is y-down: top is negative, bottom positive.
            const auto& raw = runtime->raw_projections[i];
            f = Vector4f{raw[0], raw[1], -raw[2], -raw[3]};
        } else {
            continue;
        }

        runtime->lens_frustums[i] = f; // (the lens itself, fix139)

        // Symmetric projection (on by default; off in fix138 only): each eye drawn with a frustum centred on straight ahead that covers the
        // lens's whole (off-centre) field of view, and the runtime told that is what was drawn. OpenXR only, since there the submitted
        // field of view can be anything.
        if (runtime->is_openxr() && m_symmetric_projection->value()) {
            const float half_w = std::max(std::abs(f[0]), std::abs(f[1]));
            const float half_h = std::max(std::abs(f[2]), std::abs(f[3]));
            f = Vector4f{-half_w, half_w, half_h, -half_h};
        }

        runtime->eye_frustums[i] = f;
    }

    // Mono: one picture from the middle of the head for both eyes, so its frustum covers both eyes' (tangents: left and down
    // are negative).
    {
        const auto& a = runtime->eye_frustums[0];
        const auto& b = runtime->eye_frustums[1];
        Vector4f    u{std::min(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2]), std::min(a[3], b[3])};
        if (runtime->is_openxr() && m_symmetric_projection->value()) {
            const float half_w = std::max(std::abs(u[0]), std::abs(u[1]));
            const float half_h = std::max(std::abs(u[2]), std::abs(u[3]));
            u = Vector4f{-half_w, half_w, half_h, -half_h};
        }
        runtime->mono_frustum = u;
    }

    // The shared lens (fix138, the symmetric projection off; a switch from fix139, where its test showed the game's tiled lighting
    // taking each view as centred: lights left out of 16-pixel blocks): every view drawn with both eyes' frustum together, mono's. The
    // headset's lenses look outward and down (Quest 3, the left eye: tan 1.376 out, 0.839 in, 0.966 up, 1.428 down; the right eye
    // mirrored), so both together reach 1.376 to each side, but only 0.966 up: the centred frustum's top third of a tan (a sixth of
    // its height) is never seen by either eye, and the picture's rows are spent on what is (sharper up and down). Each eye's own
    // lens would also trim each side's unseen part, but then the views would differ, and the game draws some with one camera's
    // lens for all (the stars' camera takes the world camera's, both eyes' views of full stereo; the world behind a menu keeps the
    // lens of the frame the menu opened in): one lens for every view keeps them right.
    if (runtime->is_openxr() && !m_symmetric_projection->value()) {
        runtime->eye_frustums[0] = runtime->eye_frustums[1] = runtime->mono_frustum;
    }
    if (m_mono_active.load()) {
        runtime->frustums[0] = runtime->frustums[1] = runtime->mono_frustum;
    } else {
        runtime->frustums = runtime->eye_frustums;
    }

    const auto& l = runtime->frustums[0];
    const auto half_w = std::max(std::abs(l[0]), std::abs(l[1]));
    const auto half_h = std::max(std::abs(l[2]), std::abs(l[3]));

    if (half_w > 0.0f && half_h > 0.0f) {
        runtime->diagonal_fov = glm::degrees(2.0f * atanf(sqrtf(half_w * half_w + half_h * half_h)));
    }
}

void VR::on_present() {
    m_frame_count++;
    m_presents_done.fetch_add(1, std::memory_order_relaxed);
    m_last_present_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();

    auto runtime = get_runtime();

    // Every way out of here records this present's frame index as finished (before the event, which wakes the game thread
    // waiting for it, see on_wait_rendering).
    const uint32_t present_idx = m_presenter_frame_count.load();
    const auto     finished    = [&](bool signal) {
        m_last_finished_present = present_idx;
        if (signal) {
            SetEvent(m_present_finished_event);
        }
    };

    if (!runtime->loaded) {
        finished(true);
        return;
    }

    // Pick the eye for this present from the engine's frame index. If the index did not change
    // since the previous present (the engine hook is not reporting it), alternate eyes ourselves.
    {
        const auto idx = present_idx;

        if (idx != m_last_presenter_index) {
            m_current_present_eye = eye_for_frame(idx);
            m_last_presenter_index = idx;
            m_present_index_ok = true;
        } else {
            m_current_present_eye = m_current_present_eye == VRRuntime::Eye::LEFT ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
            ++m_stats.stuck_present_index;
            m_present_index_ok = false; // (the game thread waits on the event alone until presents carry the index again)
        }
    }

    const auto eye = get_present_eye();
    const bool mono   = is_mono_frame(m_presenter_frame_count.load());
    const bool native = is_native_frame(m_presenter_frame_count.load());
    const bool aer    = is_aer_frame(m_presenter_frame_count.load());
    m_present_aer     = aer;
    if (runtime->is_openxr()) {
        m_openxr->present_frame = m_presenter_frame_count.load();
    }
    m_present_native  = native;
    m_present_mono  = mono;
    if (runtime->is_openxr()) {
        m_openxr->mono_views = mono;
    }

    // Record one eye timing trace automatically about a minute in (in game by then).
    if (m_stats.presents == 4000) {
        spdlog::info("[VR-TRACE] start (automatic, swap eyes {}, separation {:.2f})", m_swap_eyes->value(), m_eye_separation_scale->value());
        m_trace_remaining = 40;
    }

    if (m_trace_remaining.load() > 0) {
        trace("present", m_presenter_frame_count.load(), (int)eye);

        if (m_trace_remaining.fetch_sub(1) == 1) {
            spdlog::info("[VR-TRACE] end");
        }
    }

    ++m_stats.presents;
    ++(eye == VRRuntime::Eye::LEFT ? m_stats.left_presents : m_stats.right_presents);

    if (m_stats.presents % 300 == 0) {
        const auto& xr = m_openxr;
        spdlog::info("[VR] status: {}presents {} (L {} / R {}, index stuck {}), frame idx engine {} render {} present {}, "
                     "hmd syncs {} (failed {}), copies L {} R {}, copy skipped: shouldRender {} notBegun {}, "
                     "xr: ready {} state {} began {} synced {} shouldRender {} endFrames {} withLayers {} lastEnd {}, "
                     "pair waits {} (early signals {}, timeouts {}, longest {} ms){}",
            mono ? "mono, " : (native ? "full stereo, " : ""), m_stats.presents, m_stats.left_presents, m_stats.right_presents, m_stats.stuck_present_index,
            m_engine_frame_count.load(), m_render_frame_count.load(), m_presenter_frame_count.load(),
            m_stats.hmd_syncs, m_stats.hmd_sync_failures, m_stats.copies_ok[0], m_stats.copies_ok[1],
            m_stats.copy_skipped_should_render, m_stats.copy_skipped_not_begun,
            xr ? xr->ready() : false, xr ? (int)xr->session_state.load() : -1, xr ? xr->frame_began : false,
            xr ? xr->frame_synced : false, xr ? (int)xr->frame_state.shouldRender : -1,
            xr ? xr->stat_end_frames : 0, xr ? xr->stat_end_frames_with_layers : 0, xr ? (int)xr->stat_last_end_result : 0,
            m_stats.pair_waits, m_stats.early_signals, m_stats.pair_wait_timeouts, m_stats.longest_pair_wait_ms,
            !runtime->is_openxr() ? std::string{}
                                  : std::format("; pacing {} (held back {} times, frames not handed over {}), the last xrWaitFrame {} us, the headset's period {:.2f} ms{}",
                                        pipelined() ? "pipelined" : (m_pipelined_failed.load() ? "the old way (pipelined gave up)" : "the old way"),
                                        m_stats.ahead_waits, m_stats.pipelined_misses, xr ? xr->stat_open_waits_us.load() : 0u,
                                        xr ? (double)xr->published_period.load() / 1.0e6 : 0.0, aer ? ", AER" : ""));
        m_stats.longest_pair_wait_ms = 0; // (per status line)
    }

    // The renderer module saw frames out of order. This used to drop one present to "resynchronise", but eyes come from the
    // frame index, so dropping a picture never put them back in step (it only showed a black frame). The start of each pair now
    // waits for the frame before it by index (on_wait_rendering), which is what keeps them in step; here it is only noted.
    if (m_skip_next_present.exchange(false) && !mono && !native) {
        static uint32_t noted = 0;
        if (noted++ < 20) {
            spdlog::info("[VR] frames out of order around present {} (no present dropped; pair waits so far {}, early signals {})",
                present_idx, m_stats.pair_waits, m_stats.early_signals);
        }
    }

    if (runtime->is_openvr()) {
        auto openvr = get_runtime<runtimes::OpenVR>();

        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            if (std::chrono::system_clock::now() - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;

            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                finished(true);
                return;
            }
        } else {
            openvr->is_hmd_active = true; // force out an initial WaitGetPoses
            openvr->was_hmd_active = true;
        }
    }

    // Pipelined pacing (fix136): the present thread owns the headset frames: its events polled, and a frame opened if none is (the
    // first, or after one could not be), before this present's pictures are copied into it.
    const bool pipelined_now = pipelined() && runtime->is_openxr();
    const auto ends_before   = runtime->is_openxr() ? m_openxr->stat_end_frames_with_layers : 0u;
    if (pipelined_now) {
        m_openxr->pipelined = true;
        runtime->consume_events(nullptr);
        if (m_openxr->ready() && !m_openxr->frame_began) {
            m_openxr->open_frame();
        }
    }

    if (runtime->is_openxr()) {
        m_openxr->flat_screen_mode = ModSettings::showFlatScreenDisplay();
        // A game menu (not the flat-screen switch): on the panel that stays in the room, unless that is turned off.
        m_openxr->panel_mode = ModSettings::g_internalSettings.showQuadDisplay && !ModSettings::g_internalSettings.forceFlatScreen &&
                               m_openxr->panel_enabled.load() && !m_openxr->panel_refused;
        m_openxr->panel_world_allowed = ModSettings::g_internalSettings.menuWorldBehind;
        m_openxr->panel_glow_when_empty = ModSettings::g_internalSettings.menuGlowWhenEmpty; // (fix183)
        m_openxr->panel_box_rows = ModSettings::g_internalSettings.menuBoxRows;
    }

    {
        std::scoped_lock _{m_openvr_mtx};
        m_submitted = false;

        const auto e = m_d3d12.on_frame(this);

        if (e == vr::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
            m_openvr->got_first_poses = false;
            m_openvr->needs_pose_update = true;
        }

        if (m_submitted || runtime->needs_pose_update) {
            if (m_submitted && runtime->is_openvr()) {
                vr::VRCompositor()->PostPresentHandoff();
            }

            runtime->needs_pose_update = true;
            m_submitted = false;
        }
    }

    // (Pipelined: presents that should have ended a headset frame with pictures and did not, for 3 s on end while presents go on and
    // the headset wants pictures: something this did not foresee; the old pacing from then on, for the session.)
    static int64_t last_ok_ns = 0; // (started again whenever pipelined pacing starts again: the setting turned back on)
    if (!pipelined_now) {
        last_ok_ns = 0;
    } else {
        const int64_t  now_ns     = std::chrono::steady_clock::now().time_since_epoch().count();
        const bool     due        = mono || native || aer || eye == VRRuntime::Eye::RIGHT;
        const bool     ended      = m_openxr->stat_end_frames_with_layers != ends_before;
        if (last_ok_ns == 0 || !due || ended || !m_openxr->ready() || m_openxr->frame_state.shouldRender != XR_TRUE) {
            last_ok_ns = now_ns;
        } else {
            ++m_stats.pipelined_misses;
            if (now_ns - last_ok_ns > std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(3)).count()) {
                m_pipelined_failed = true;
                spdlog::warn("[VR] pipelined pacing: no headset frame handed over for 3 s ({} presents due without one): the old pacing from "
                             "now on (the game thread waits before each pair)", m_stats.pipelined_misses);
            }
        }
    }

    finished(mono || native || aer || eye == VRRuntime::Eye::RIGHT);
}

void VR::toggle_mono() {
    m_mono_toggles.fetch_add(1); // (applied by the game thread at the next frame)
}

void VR::on_post_present() {
    if (!get_runtime()->loaded) {
        return;
    }

    m_d3d12.on_post_present(this);

    // Pipelined pacing (fix136): the next headset frame opened right after this one was ended and the game's picture presented (as
    // UEVR does): xrWaitFrame paces this thread to the headset, and its display time is the one the game thread's next poses are for.
    if (pipelined() && get_runtime()->is_openxr() && m_openxr->ready() && !m_openxr->frame_began) {
        m_openxr->pipelined = true;
        m_openxr->open_frame();
    }
}

void VR::on_device_reset() {
    spdlog::info("[VR] on_device_reset");

    std::scoped_lock _{m_openxr->sync_mtx};
    m_d3d12.on_reset(this);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Input
//////////////////////////////////////////////////////////////////////////////////////////////////

void VR::update_action_states() {
    auto runtime = get_runtime();

    if (!runtime->loaded || runtime->wants_reinitialize) {
        return;
    }

    if (runtime->is_openvr()) {
        const auto start = std::chrono::high_resolution_clock::now();
        const auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("[VR] VRInput failed to update action state: {}", (uint32_t)error);
        }

        if (std::chrono::high_resolution_clock::now() - start >= std::chrono::milliseconds(30)) {
            spdlog::warn("[VR] VRInput update action state took too long, reinitializing");
            runtime->wants_reinitialize = true;
        }
    } else {
        runtime->update_input();
    }

    if (is_any_action_down()) {
        m_last_controller_update = std::chrono::steady_clock::now();
    }

    if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_mono_key->is_key_down_once()) {
        toggle_mono();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }
}

bool VR::is_any_action_down() {
    if (!m_runtime->ready() || m_controllers.empty()) {
        return false;
    }

    const auto deadzone = m_joystick_deadzone->value();

    if (glm::length(get_left_stick_axis()) >= deadzone || glm::length(get_right_stick_axis()) >= deadzone) {
        return true;
    }

    for (auto& it : m_action_handles) {
        const auto action = it.second.get();

        if (action == m_action_pose || action == m_action_haptic || action == m_action_joystick || action == m_action_touchpad) {
            continue;
        }

        if (is_action_active(action, m_left_joystick) || is_action_active(action, m_right_joystick)) {
            return true;
        }
    }

    return false;
}

bool VR::is_action_active(ActionHandle action, SourceHandle source) const {
    if (!get_runtime()->loaded || action == 0) {
        return false;
    }

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        return data.bActive && data.bState;
    }

    if (get_runtime()->is_openxr()) {
        return m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    return false;
}

Vector2f VR::get_joystick_axis(SourceHandle handle) const {
    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    const auto deadzone = m_joystick_deadzone->value();

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto out = Vector2f{data.x, data.y};
        return glm::length(out) > deadzone ? out : Vector2f{};
    }

    if (get_runtime()->is_openxr()) {
        Vector2f out{};

        if (handle == (SourceHandle)VRRuntime::Hand::LEFT) {
            out = m_openxr->get_left_stick_axis();
        } else if (handle == (SourceHandle)VRRuntime::Hand::RIGHT) {
            out = m_openxr->get_right_stick_axis();
        }

        return glm::length(out) > deadzone ? out : Vector2f{};
    }

    return Vector2f{};
}

void VR::set_reticle(bool visible, VRRuntime::Hand hand, float distance_m, float width_degrees, const float* origin, const float* direction, int sprite) {
    if (m_openxr == nullptr || !get_runtime()->is_openxr()) {
        return;
    }
    {
        std::scoped_lock _{m_openxr->reticle_ray_mtx};
        const bool given = visible && origin != nullptr && direction != nullptr && std::isfinite(origin[0]) && std::isfinite(origin[1]) &&
                           std::isfinite(origin[2]) && std::isfinite(direction[0]) && std::isfinite(direction[1]) && std::isfinite(direction[2]);
        const float length = given ? std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]) : 0.0f;
        m_openxr->reticle_has_ray = given && length > 1e-4f;
        m_openxr->reticle_sprite = sprite == runtimes::OpenXR::sprite_laser_dot ? sprite : std::clamp(sprite, 0, runtimes::OpenXR::reticle_game_sprites - 1);
        if (m_openxr->reticle_has_ray) {
            for (int i = 0; i < 3; ++i) {
                m_openxr->reticle_origin[i] = origin[i];
                m_openxr->reticle_direction[i] = direction[i] / length;
            }
        }
    }
    m_openxr->reticle_hand = (int)hand;
    m_openxr->reticle_distance = std::clamp(distance_m, 0.2f, 100.0f);
    m_openxr->reticle_degrees = std::clamp(width_degrees, 0.2f, 40.0f);
    m_openxr->reticle_visible = visible;
}

void VR::set_laser_ray(bool on, const float* origin, const float* direction, uint32_t frame) {
    if (!on || origin == nullptr || direction == nullptr) {
        vrmod::LaserSight::get().set_ray(false, glm::vec3{0.0f}, glm::vec3{0.0f}, 0);
        return;
    }
    vrmod::LaserSight::get().set_ray(true, glm::vec3{origin[0], origin[1], origin[2]}, glm::vec3{direction[0], direction[1], direction[2]}, frame);
}

bool VR::laser_hit(bool& hit, float& distance) {
    const auto latest = vrmod::LaserSight::get().latest();
    hit = latest.hit;
    distance = latest.distance;
    return latest.fresh;
}

void VR::set_laser_beam(bool visible, const float* start, const float* end) {
    if (m_openxr == nullptr || !get_runtime()->is_openxr()) {
        return;
    }
    std::scoped_lock _{m_openxr->reticle_ray_mtx};
    const auto finite = [](const float* v) { return v != nullptr && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); };
    m_openxr->laser_beam_visible = visible && finite(start) && finite(end);
    if (m_openxr->laser_beam_visible) {
        for (int i = 0; i < 3; ++i) {
            m_openxr->laser_beam_start[i] = start[i];
            m_openxr->laser_beam_end[i] = end[i];
        }
    }
}

void VR::set_laser_beam_view(bool visible, const float* start, const float* direction, float length, uint32_t frame) {
    if (!visible || start == nullptr || direction == nullptr) {
        vrmod::LaserSight::get().set_beam(false, glm::vec3{0.0f}, glm::vec3{0.0f}, 0.0f, 0);
        return;
    }
    vrmod::LaserSight::get().set_beam(true, glm::vec3{start[0], start[1], start[2]}, glm::vec3{direction[0], direction[1], direction[2]}, length, frame);
}

bool VR::reticle_blocked() {
    return m_d3d12.openxr().reticle_is_blocked();
}

void VR::retry_reticle() {
    m_d3d12.openxr().reticle_retry();
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, SourceHandle source) {
    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Poses
//////////////////////////////////////////////////////////////////////////////////////////////////

Matrix4x4f VR::get_eye_transform(VRRuntime::Eye eye) const {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }
    std::shared_lock _{get_runtime()->eyes_mtx};
    auto m = get_runtime()->eyes[(size_t)eye];
    // (As the eye being drawn: Eye Separation scale, 0 = both eyes at the head centre.)
    const auto scale = m_eye_separation_scale->value();
    m[3] = Vector4f{Vector3f{m[3]} * scale, 1.0f};
    return m;
}

Matrix4x4f VR::get_current_eye_transform(bool flip) const {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    // Mono: the middle of the head (both eyes see the same picture).
    if (m_mono_active.load()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto eye = get_current_render_eye();

    if (flip) {
        eye = eye == VRRuntime::Eye::LEFT ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
    }

    auto m = get_runtime()->eyes[(size_t)eye];

    // Eye Separation scale (diagnostic): 0 = both eyes at the head centre, 1 = real IPD.
    const auto scale = m_eye_separation_scale->value();
    m[3] = Vector4f{Vector3f{m[3]} * scale, 1.0f};

    return m;
}

void VR::trace(std::string_view where, uint32_t frame_index, int eye) {
    if (m_trace_remaining.load() <= 0) {
        return;
    }

    spdlog::info("[VR-TRACE] {:<14} tid {:>6} frame {:>8} parity {} eye {} | engine {} render {} present {}",
        where, GetCurrentThreadId(), frame_index, frame_index % 2, eye < 0 ? "-" : (eye == 0 ? "L" : "R"),
        m_engine_frame_count.load(), m_render_frame_count.load(), m_presenter_frame_count.load());
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) const {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto eye = get_current_render_eye();

    if (flip) {
        eye = eye == VRRuntime::Eye::LEFT ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
    }

    return get_runtime()->projections[(size_t)eye];
}

Vector4f VR::get_position(uint32_t index) const {
    std::shared_lock _{get_runtime()->pose_mtx};
    return get_position_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        // index 1/2 are hands for callers; map them to the tracked device indices
        if (index > 0 && index <= m_controllers.size()) {
            index = (uint32_t)m_controllers[index - 1];
        }

        const auto& pose = m_openvr->render_poses[index];
        auto result = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking})[3];
        result.w = 1.0f;
        return result;
    }

    if (get_runtime()->is_openxr()) {
        if (index == 0) {
            return Vector4f{*(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f};
        }

        if (index <= 2) {
            return Vector4f{*(Vector3f*)&m_openxr->hands[index - 1].location.pose.position, 1.0f};
        }
    }

    return Vector4f{};
}

Vector4f VR::get_velocity(uint32_t index) const {
    std::shared_lock _{get_runtime()->pose_mtx};

    if (get_runtime()->is_openvr()) {
        if (index > 0 && index <= m_controllers.size()) {
            index = (uint32_t)m_controllers[index - 1];
        }

        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& v = m_openvr->render_poses[index].vVelocity;
        return Vector4f{v.v[0], v.v[1], v.v[2], 0.0f};
    }

    if (get_runtime()->is_openxr() && index > 0 && index <= 2) {
        return Vector4f{*(Vector3f*)&m_openxr->hands[index - 1].velocity.linearVelocity, 0.0f};
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    std::shared_lock _{get_runtime()->pose_mtx};

    if (get_runtime()->is_openvr()) {
        if (index > 0 && index <= m_controllers.size()) {
            index = (uint32_t)m_controllers[index - 1];
        }

        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& v = m_openvr->render_poses[index].vAngularVelocity;
        return Vector4f{v.v[0], v.v[1], v.v[2], 0.0f};
    }

    if (get_runtime()->is_openxr() && index > 0 && index <= 2) {
        return Vector4f{*(Vector3f*)&m_openxr->hands[index - 1].velocity.angularVelocity, 0.0f};
    }

    return Vector4f{};
}

Matrix4x4f VR::get_rotation(uint32_t index) const {
    return glm::extractMatrixRotation(get_transform(index));
}

Matrix4x4f VR::get_transform(uint32_t index) const {
    std::shared_lock _{get_runtime()->pose_mtx};

    if (get_runtime()->is_openvr()) {
        if (index > 0 && index <= m_controllers.size()) {
            index = (uint32_t)m_controllers[index - 1];
        }

        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        const auto& pose = m_openvr->render_poses[index];
        return glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
    }

    if (get_runtime()->is_openxr()) {
        const XrPosef* pose = nullptr;

        if (index == 0) {
            pose = &m_openxr->view_space_location.pose;
        } else if (index <= 2) {
            pose = &m_openxr->hands[index - 1].location.pose;
        }

        if (pose != nullptr) {
            auto mat = Matrix4x4f{*(glm::quat*)&pose->orientation};
            mat[3] = Vector4f{*(Vector3f*)&pose->position, 1.0f};
            return mat;
        }
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_aim_transform(uint32_t index) const {
    if (get_runtime()->is_openxr() && m_openxr != nullptr && index > 0 && index <= 2) {
        std::shared_lock _{get_runtime()->pose_mtx};
        const auto& location = m_openxr->hands[index - 1].aim_location;
        constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & valid) == valid) {
            auto mat = Matrix4x4f{*(glm::quat*)&location.pose.orientation};
            mat[3] = Vector4f{*(Vector3f*)&location.pose.position, 1.0f};
            return mat;
        }
    }
    return get_transform(index);
}

float VR::get_floor_eye_height() const {
    return get_runtime()->is_openxr() && get_runtime()->loaded && m_openxr != nullptr ? m_openxr->floor_eye_height.load() : -1.0f;
}

VR::FingerState VR::get_finger_state(bool left) const {
    if (!get_runtime()->is_openxr() || !get_runtime()->loaded || m_openxr == nullptr) {
        return {};
    }
    const auto in = m_openxr->get_finger_input(left ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT);
    return FingerState{in.trigger, in.grip, in.trigger_touch, in.thumb_touch};
}

namespace {
    VR::TrackedJoint to_tracked_joint(XrSpaceLocationFlags flags, const XrPosef& pose) {
        VR::TrackedJoint joint{};
        joint.position_valid = (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        joint.orientation_valid = (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        if (joint.position_valid) {
            joint.position = Vector3f{pose.position.x, pose.position.y, pose.position.z};
        }
        if (joint.orientation_valid) {
            joint.orientation = glm::quat{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
        }
        return joint;
    }
}

bool VR::get_body_tracking(BodyTrackingState& out) const {
    out = {};
    if (!get_runtime()->is_openxr() || !get_runtime()->loaded || m_openxr == nullptr || m_openxr->body_tracker == XR_NULL_HANDLE) {
        return false;
    }

    std::shared_lock _{get_runtime()->pose_mtx};
    const auto& body = m_openxr->body_joints;
    out.supported = true;
    out.full_body = m_openxr->body_full_body_supported;
    out.active = body.active;
    out.high_fidelity = body.high_fidelity;
    out.confidence = body.confidence;
    out.joint_count = body.joint_count;
    for (uint32_t i = 0; i < body.joint_count && i < out.joints.size(); ++i) {
        out.joints[i] = to_tracked_joint(body.active ? body.joints[i].locationFlags : 0, body.joints[i].pose);
    }
    return true;
}

bool VR::get_hand_tracking(bool left, HandTrackingState& out) const {
    out = {};
    const auto index = left ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;
    if (!get_runtime()->is_openxr() || !get_runtime()->loaded || m_openxr == nullptr || m_openxr->hand_trackers[index] == XR_NULL_HANDLE) {
        return false;
    }

    std::shared_lock _{get_runtime()->pose_mtx};
    const auto& hand = m_openxr->hand_joints[index];
    out.supported = true;
    out.active = hand.active;
    out.data_source = hand.data_source;
    for (size_t i = 0; i < out.joints.size() && i < hand.joints.size(); ++i) {
        out.joints[i] = to_tracked_joint(hand.active ? hand.joints[i].locationFlags : 0, hand.joints[i].pose);
    }
    return true;
}

Matrix4x4f VR::get_transform_offset() const {
    auto mat = glm::identity<Matrix4x4f>();
    mat[3] = get_standing_origin();
    mat[3].w = 1.0f;
    return mat;
}

Vector4f VR::get_standing_origin() const {
    std::shared_lock _{get_runtime()->pose_mtx};
    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    std::unique_lock _{get_runtime()->pose_mtx};
    m_standing_origin = origin;
    // (A recenter: an open menu's panel is put in front of you again; fix196: the mod's own menu's too.)
    m_openxr->panel_recenter = true;
    m_openxr->fw_menu_recenter = true;
    static int logged = 0;
    if (logged++ < 20) {
        spdlog::info("[VR] standing origin set (a recenter): {:.2f} {:.2f} {:.2f}", origin.x, origin.y, origin.z);
    }
}

glm::quat VR::get_rotation_offset() const {
    std::shared_lock _{m_rotation_mtx};
    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{m_rotation_mtx};
    m_rotation_offset = offset;
}

void VR::recenter_view() {
    // The headset's current position becomes the centre. (The rotation offset below is kept for
    // reference but isn't applied to poses; turning is handled by the game integration.)
    set_standing_origin(get_position(0));

    // keep only the yaw of the current head rotation
    const auto rot = get_rotation(0);
    const auto forward = glm::normalize(Vector3f{rot[2].x, 0.0f, rot[2].z});
    const auto yaw_only = glm::quatLookAtRH(-forward, Vector3f{0.0f, 1.0f, 0.0f});

    set_rotation_offset(glm::normalize(glm::inverse(yaw_only)));
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// UI / config
//////////////////////////////////////////////////////////////////////////////////////////////////

// (fix196) The eye mode as one choice (the three saved settings behind it unchanged: MonoMode, AlternateEyeRendering, FullStereo).
int VR::eye_mode_choice() const {
    if (m_mono->value()) {
        return 2;
    }
    if (m_aer->value()) {
        return 3;
    }
    return m_full_stereo->value() ? 0 : 1;
}

void VR::set_eye_mode_choice(int choice) {
    switch (choice) {
    case 0:
        m_mono->value() = false;
        m_aer->value() = false;
        m_full_stereo->value() = true;
        break;
    case 1:
        m_mono->value() = false;
        m_aer->value() = false;
        m_full_stereo->value() = false;
        break;
    case 2:
        m_mono->value() = true;
        m_aer->value() = false;
        break;
    default:
        m_mono->value() = false;
        m_aer->value() = true;
        break;
    }
    spdlog::info("[VR] (fix196) eye mode chosen in the menu: {} (from the next frame pair)",
        choice == 0 ? "full stereo" : choice == 1 ? "stereo, the eyes taking turns" : choice == 2 ? "mono" : "AER");
}

// (fix198) The eye mode chosen in a menu, refused as the eye-mode button refuses it (fix165): while video memory has been over its budget
// 3 s, since a second picture and DLSS instance would push more of the game's data out of it. Two short buzzes as the button's refusal
// gives (felt in the game's menu, where no line shows). (fix198, the review) Only a switch to a heavier mode than the one chosen now is
// refused (full stereo, then the eyes taking turns or AER, then mono): a lighter one can only help.
namespace {
int eye_mode_weight(int choice) {
    return choice == 0 ? 2 : (choice == 2 ? 0 : 1);
}
const char* eye_mode_name(int choice) {
    return choice == 0 ? "full stereo" : choice == 1 ? "stereo, the eyes taking turns" : choice == 2 ? "mono" : "AER";
}
} // namespace

bool VR::eye_mode_refused(int choice, const char* where) {
    choice = std::clamp(choice, 0, 3);
    const auto runtime = get_runtime();
    if (eye_mode_weight(choice) <= eye_mode_weight(eye_mode_choice()) || runtime == nullptr || !runtime->is_openxr() ||
        !m_d3d12.memory_over_for(3000)) {
        return false;
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t since = m_d3d12.m_mem_over_since_ms.load();
    std::string why = std::format("Not switched to {}: video memory had been over its budget for {} s ({} of {} MB). Try again once it "
                                  "has been under it for 2 s.",
        eye_mode_name(choice), since != 0 ? (now_ms - since) / 1000 : 0, m_d3d12.m_mem_used_mb.load(), m_d3d12.m_mem_budget_mb.load());
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) < 50) {
        spdlog::info("[VR] (fix198) the eye mode chosen in {} ({}) refused: {}", where, eye_mode_name(choice), why);
    }
    {
        std::scoped_lock _{m_eye_mode_refused_mtx};
        m_eye_mode_refused_until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        m_eye_mode_refused_why = std::move(why);
    }
    m_refused_buzz_asked = true; // (its two pulses started on the game thread: on_wait_rendering)
    return true;
}

void VR::choose_eye_mode(int choice) {
    set_eye_mode_choice(std::clamp(choice, 0, 3));
    {
        std::scoped_lock _{m_eye_mode_refused_mtx};
        m_eye_mode_refused_until = {};
    }
    if (g_framework != nullptr) {
        g_framework->request_save_config();
    }
}

bool VR::try_eye_mode_choice(int choice, const char* where) {
    if (eye_mode_refused(choice, where)) {
        return false;
    }
    choose_eye_mode(choice);
    return true;
}

bool VR::eye_mode_refused_lately() const {
    std::scoped_lock _{m_eye_mode_refused_mtx};
    return std::chrono::steady_clock::now() < m_eye_mode_refused_until;
}

std::string VR::eye_mode_refusal() const {
    std::scoped_lock _{m_eye_mode_refused_mtx};
    return std::chrono::steady_clock::now() < m_eye_mode_refused_until ? m_eye_mode_refused_why : std::string{};
}

// (fix198, the review) Only noted here (any thread: a menu's handler may run on its own): on_begin_rendering applies it on the game thread
// once it has stayed 750 ms and no game menu is shown. The time is kept only when the value asked for changes (a menu may ask again).
void VR::apply_resolution_scale() {
    const float value = m_resolution_scale->value();
    if (!std::isfinite(value) || value <= 0.0f) {
        return;
    }
    if (m_res_asked_value.load() != value) {
        m_res_asked_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    m_res_asked_value = value; // (after its time: on_begin_rendering reads the value, then the time)
}

// (fix196) The Main tab: the eye mode and the sharpness (part 1), left-handed (part 4).
void VR::on_draw_main_ui(int part) {
    // (fix198) With the game's Settings > VR tab working, the eye mode, the sharpness and left-handed are there (the same settings); here
    // only what concerns this menu: the runtime's absence, the VectorXR note. (fix198, the review) Drawn here too in the Advanced tab's
    // "Main VR settings" group, when asked for (VRConfig::s_main_rows_here: the VectorXR note is then left to Main).
    const bool in_game_tab = VRConfig::main_rows_in_game_tab();
    const bool on_main     = !VRConfig::s_main_rows_here.load();
    if (part == 1) {
        if (get_runtime() == nullptr || !get_runtime()->loaded) {
            // (fix196, the review) Why, in one line.
            std::string why{"start the headset's software, such as Virtual Desktop, before the game"};
            if (m_openxr != nullptr && m_openxr->error) {
                why = m_openxr->dll_missing ? std::string{"openxr_loader.dll was not found next to the mod"} : "OpenXR: " + *m_openxr->error;
            } else if (m_openvr != nullptr && m_openvr->error) {
                why = m_openvr->dll_missing ? std::string{"openvr_api.dll was not found"} : "OpenVR: " + *m_openvr->error;
            }
            ImGui::TextWrapped("No VR runtime loaded (%s).", why.c_str());
            return;
        }
        if (!in_game_tab) {
            static const char* const kEyeModes[] = {
                "Full stereo",
                "Stereo, eyes take turns",
                "Mono",
                "AER (one eye a frame)",
            };
            int choice = eye_mode_choice();
            if (ImGui::Combo("Eye mode", &choice, kEyeModes, IM_ARRAYSIZE(kEyeModes))) {
                try_eye_mode_choice(choice, "the F11 menu"); // (fix198: refused as the eye-mode button refuses it)
            }
        }
        if (const std::string refusal = eye_mode_refusal(); !refusal.empty()) { // (fix198, the review: a copy, made under its lock)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", refusal.c_str());
            ImGui::PopStyleColor();
        }
        // (fix196, the review) What runs now, when it is not what was chosen.
        {
            const int chosen = eye_mode_choice();
            std::string now{};
            if (chosen == 0 && !is_native()) {
                const char* running = is_mono() ? "mono" : is_aer() ? "AER" : "the eyes take turns";
                const char* why = !get_runtime()->is_openxr() ? "full stereo needs OpenXR"
                                  : is_mono()                 ? "a game menu is shown, or video memory is full"
                                                              : "full stereo can't run here right now: a game menu, the hand scanner, build "
                                                                "mode or a loading screen";
                now = std::format("Now: {} ({}).", running, why);
            } else if ((chosen == 1 || chosen == 3) && is_mono()) {
                now = "Now: mono (a game menu is shown, or video memory is full).";
            }
            if (!now.empty() && !in_game_tab) {
                menu_ui::hint(now.c_str());
            }
        }
        if (!in_game_tab) {
            menu_ui::hint("Full stereo: real depth; the heaviest (the world drawn twice a frame). Eyes take turns: depth, fewer fps lost, each eye "
                          "a new picture every other frame. Mono: no depth, the most fps. AER: one eye drawn a frame, light and smooth. The left "
                          "grip held + a right stick click switches too. Mono and AER need OpenXR.");
        }
        // (fix197) VectorXR's Frame Generation: a note when it makes frames while the eyes are in AER (it cannot work with it), and its state.
        if (on_main && m_vectorxr_layer.load()) {
            if (vectorxr_aer_conflict()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                ImGui::TextWrapped("VectorXR's Frame Generation can't work with AER (alternate-eye rendering): use mono or full stereo.");
                ImGui::PopStyleColor();
            } else {
                std::string summary{};
                {
                    std::scoped_lock _{m_vectorxr_mtx};
                    summary = m_vectorxr_summary;
                }
                menu_ui::hint(("VectorXR: " + (summary.empty() ? std::string{"its layer is loaded (no status read yet)"} : summary)).c_str());
            }
        }

        if (in_game_tab) {
            return;
        }
        if (get_runtime()->is_openxr()) {
            // (A slider of its own over the same setting, 0.5 to 1.5: the full range is in the Debug tab.)
            float scale = m_resolution_scale->value();
            ImGui::PushID("main_resolution");
            if (ImGui::SliderFloat("Sharpness (resolution)", &scale, 0.5f, 1.5f, "%.2f")) {
                m_resolution_scale->value() = std::clamp(scale, 0.5f, 1.5f);
                g_framework->request_save_config();
            }
            // Asked for once the slider is let go after a change (or a typed value is entered), not at each step of a drag; (fix198, the
            // review) applied 750 ms on, once no game menu is shown.
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                apply_resolution_scale();
            }
            ImGui::PopID();
            const std::string sharpness = std::format("1.0 by default: the headset's own size. Higher: sharper, but fewer fps and more video "
                                                      "memory. Lower: more fps, but blurrier (0.8 is about 64% of the pixels: 1.0 has about "
                                                      "1.56 times those of 0.8). Applied a moment after you let go (with a game menu shown, "
                                                      "as it closes). Each eye is drawn at {} x {} now.",
                get_runtime()->get_width(), get_runtime()->get_height());
            menu_ui::hint(sharpness.c_str());
        } else {
            menu_ui::hint("The resolution is set in SteamVR with this runtime.");
        }
    } else if (part == 4 && !in_game_tab) {
        m_swap_controllers->draw("Left-handed");
        menu_ui::hint("The controllers swap jobs: the left one fires, aims and turns, the right one walks (the face buttons keep their letters).");
    }
}

// (fix196) The Advanced tab: the rest of the headset's settings a player may change (part 1 the eye modes, part 2 the controllers and the
// monitor).
void VR::on_draw_advanced_ui(int part) {
    if (part == 1 && menu_ui::group("Eye modes and recentring")) {
        m_mono_in_menus->draw("Mono while a game menu is shown");
        menu_ui::hint("Menus run faster in mono and use less video memory; your eye mode comes back as the menu closes (with full stereo, "
                      "once the menu's pictures are let go: up to 3 s).");
        if (m_aer_in_button->draw("Include AER in the eye-mode button")) {
            g_framework->request_save_config();
        }
        menu_ui::hint("Off: the left grip held + a right stick click switches between stereo and mono only. On: stereo, mono and AER in turn.");
        m_mono_key->draw("Eye-mode key (keyboard)");
        m_recenter_view_key->draw("Recenter key (keyboard)");
        m_set_standing_key->draw("Set standing origin key (keyboard)");
        if (ImGui::Button("Set standing height")) {
            auto origin = get_standing_origin();
            origin.y = get_position(0).y;
            set_standing_origin(origin);
        }
        ImGui::SameLine();
        if (ImGui::Button("Set standing origin")) {
            set_standing_origin(get_position(0));
        }
        ImGui::SameLine();
        if (ImGui::Button("Recenter view")) {
            recenter_view();
        }
        menu_ui::hint("\"Recenter now\" (in the game's Settings > VR; on this menu's Main tab when that tab is not there) recentres as most "
                      "players need; these set the headset's own origin parts.");
    }
    if (part == 2 && menu_ui::group("Controllers (headset side)")) {
        m_motion_controls_inactivity_timer->draw("Motion controls idle time (s)");
        menu_ui::hint("How long after the controllers were last used they still count as in use (after that a gamepad or the keyboard and mouse work as on a flat screen).");
        m_joystick_deadzone->draw("Thumbstick dead zone");
        menu_ui::hint("How far a thumbstick must move before it counts (raise it if a stick drifts).");
    }
    if (part == 2 && menu_ui::group("Monitor")) {
        m_desktop_fix->draw("Desktop recording fix");
        menu_ui::hint("The monitor shows every frame evenly, for recording (costs a full-size copy every two frames).");
    }
}

// (fix196) The Debug tab: the runtime, the stereo tests and the raw eye switches.
void VR::on_draw_debug_ui() {
    if (menu_ui::group("Headset and runtime")) {
        auto display_error = [](auto& runtime, const char* dll_name) {
            if (runtime == nullptr || (!runtime->error && runtime->loaded)) {
                return;
            }

            if (runtime->error && runtime->dll_missing) {
                ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name);
            } else if (runtime->error) {
                ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
            }
        };

        display_error(m_openxr, "openxr_loader.dll");
        display_error(m_openvr, "openvr_api.dll");

        m_requested_runtime->draw("Preferred Runtime (restart to apply)");

        if (!get_runtime()->loaded) {
            ImGui::TextWrapped("No VR runtime loaded.");
        } else {
            ImGui::TextWrapped("VR Runtime: %s", get_runtime()->name().data());
            ImGui::TextWrapped("Render Resolution: %d x %d", get_runtime()->get_width(), get_runtime()->get_height());
            ImGui::TextWrapped("Frames: engine %u, render %u, present %u", m_engine_frame_count.load(), m_render_frame_count.load(),
                m_presenter_frame_count.load());

            if (get_runtime()->is_openxr()) {
                m_resolution_scale->draw("OpenXR Resolution Scale (full range)");
                // Applied once the slider is let go (or a typed value is entered; or, if the menu was closed mid-drag, when it is next
                // shown), not at every step of a drag.
                if (!ImGui::IsItemActive() && m_openxr->resolution_scale != m_resolution_scale->value()) {
                    m_wants_swapchain_rebuild = true;
                }

                if (ImGui::TreeNode("Bindings")) {
                    m_openxr->display_bindings_editor();
                    ImGui::TreePop();
                }

                const double min_ = 0.0;
                const double max_ = 5.0;
                ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);
            } else {
                ImGui::TextWrapped("Resolution can be changed in SteamVR");
            }

            // (fix189) Not offered with OpenXR: remaking it (reinitialize_openxr) frees the runtime while the present thread may still be
            // using it (on_present holds the pointer get_runtime() gave it), a crash. Restarting the game is the safe way.
            if (get_runtime()->is_openxr()) {
                ImGui::TextDisabled("Reinitialize Runtime: not available with OpenXR (restart the game instead)");
            } else if (ImGui::Button("Reinitialize Runtime")) {
                get_runtime()->wants_reinitialize = true;
            }
        }
    }

    if (menu_ui::group("Stereo (diagnostics)")) {
        ImGui::TextWrapped("If near objects (your body, gun, crosshair) look doubled or impossible to focus, try these live and see what "
                           "changes (close this menu to look).");
        m_swap_eyes->draw("Swap Eyes");
        ImGui::SameLine();
        ImGui::TextDisabled("(if this fixes it, the eyes were reversed)");
        if (m_symmetric_projection->draw("Symmetric eye projection (each eye drawn centred; off: the part no eye sees trimmed, sharper, but "
                                         "blocky lighting glitches)")) {
            spdlog::info("[QuickTest] Symmetric eye projection -> {}", m_symmetric_projection->value() ? "on" : "off");
        }
        menu_ui::hint("The eye mode's three switches as they are saved (\"Eye mode\", in the game's Settings > VR, sets them together):");
        if (m_mono->draw("Mono (one picture for both eyes, a new one every frame; no stereo depth)")) {
            spdlog::info("[VR] mono {} (from the next frame pair)", m_mono->value() ? "on" : "off");
        }
        if (m_aer->draw("AER (one eye drawn each frame, each sent with the other eye's last picture: a new headset frame every frame)")) {
            spdlog::info("[VR] AER {} (from the next frame pair; mono first if both)", m_aer->value() ? "on" : "off");
        }
        if (m_full_stereo->draw("Full stereo (in stereo: both eyes drawn in every frame, as two views of the world)")) {
            spdlog::info("[VR] full stereo {} (from the next frame pair, in stereo)", m_full_stereo->value() ? "on" : "off");
        }
        if (m_pipelined_pacing->draw("Pipelined pacing (the game does not wait for the headset before each frame pair)")) {
            m_pipelined_failed = false;
            spdlog::info("[VR] pipelined pacing {}", m_pipelined_pacing->value() ? "on" : "off");
        }
        m_eye_separation_scale->draw("Eye Separation Scale");
        ImGui::TextDisabled("0 = no stereo depth. If things still look doubled at 0, it is a\n"
                            "temporal effect (DLSS / FSR / TAA history), not the eye offset.");

        if (ImGui::Button("Log Eye Timing (40 frames)")) {
            spdlog::info("[VR-TRACE] start (swap eyes {}, separation {:.2f})", m_swap_eyes->value(), m_eye_separation_scale->value());
            m_trace_remaining = 40;
        }

        if (ImGui::Button("Save Stereo Screenshot")) {
            ++m_screenshot_number;
            m_screenshot_armed = true;
            m_screenshot_done[0] = m_screenshot_done[1] = false;
            m_screenshot_not_before = {};
        }
        ImGui::SameLine();
        if (m_screenshot_armed) {
            ImGui::TextWrapped("#%d: close this menu and hold still, it saves 2 s later", m_screenshot_number);
        } else if (m_screenshot_number > 0) {
            ImGui::TextWrapped("Saved sfvr\\stereo_%d_left.jpg / _right.jpg", m_screenshot_number);
        } else {
            ImGui::TextWrapped("Saves both eye images to the sfvr folder (numbered, nothing is overwritten)");
        }
    }
}

void VR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (m_motion_controls_inactivity_timer->value() <= 10.0f) {
        m_motion_controls_inactivity_timer->value() = 30.0f;
    }

    // (fix198) The resolution's new default, 1.0: a config that still holds the old default 0.8 starts at 1.0, once (the mark saved with
    // it); a value chosen later, 0.8 too, stays as chosen.
    if (!m_resolution_default_1->value()) {
        if (std::abs(m_resolution_scale->value() - 0.8f) < 0.001f) {
            m_resolution_scale->value() = 1.0f;
            spdlog::info("[VR] (fix198) resolution scale: the old default 0.8 moved to the new default 1.0 (once; choose 0.8 again in the menu to keep it)");
        }
        m_resolution_default_1->value() = true;
        if (g_framework != nullptr) {
            g_framework->request_save_config();
        }
    }

    // Runtime already up (config reloaded): apply resolution scale on the next frame.
    if (get_runtime()->is_openxr() && get_runtime()->loaded && m_init_finished &&
        m_openxr->resolution_scale != m_resolution_scale->value()) {
        m_wants_swapchain_rebuild = true;
    }
}

void VR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

// (fix197) VectorXR's status file, read every 3 s on a thread of its own (never the game's or the present thread), once its layer is seen.
// (Each poll on its own: nothing it meets ends the thread or reaches the game; the review.)
void VR::start_vectorxr_watch() {
    if (m_vectorxr_watching.exchange(true)) {
        return;
    }
    try {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                try {
                    const auto vr = VR::get(); // (a copy for this poll)
                    if (vr == nullptr) {
                        return;
                    }
                    vr->check_vectorxr_status();
                } catch (...) {
                    // (Left for the next poll.)
                }
            }
        }).detach();
    } catch (...) {
        m_vectorxr_watching = false;
        spdlog::warn("[VR] (fix197) VectorXR: the thread that reads its status file could not be started; its state is not followed");
    }
}

// (fix197) VectorXR's published status (its documented status file, %LOCALAPPDATA%\VectorXR\api\status.json: written about once a second by
// the layer in the game with a live session, replaced in one step; read whole with read, write and delete sharing so its next write is
// never held up). Only our process's, and only while current (written within the last 3 s). Read only. A file that cannot be read this
// poll (being replaced, half-written, an unexpected shape) changes nothing: what was known stays until a poll reads it.
void VR::check_vectorxr_status() {
    std::string summary{"no status file (Frame Generation off, or not started yet)"};
    std::string fps{}; // (shown in the menu, not compared: it moves all the time)
    bool generating = false;
    int multiplier = 0;
    // (fix200) What this poll says of Frame Generation for the session: -1 nothing (no file, another program's, stale: as it was), 0 off
    // for this game (its part of the file absent: it cannot start before the game restarts), 1 on in any state (off by its own key,
    // waiting, standing by, paused, generating: it may make frames).
    int framegen_known = -1;
    static int missing_polls = 0; // (this thread only)
    static bool open_error_told = false;
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::wstring path = std::wstring{local} + L"\\VectorXR\\api\\status.json";
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                // (Being replaced or removed this moment, or held: as it was, tried again at the next poll.)
                if (!open_error_told) {
                    open_error_told = true;
                    spdlog::info("[VR] (fix197) VectorXR: its status file could not be opened this time (error {}); tried again every 3 s", error);
                }
                return;
            }
            // (Not there: only after two polls in a row counted as no file; a moment's absence as it is replaced is not.)
            if (++missing_polls < 2) {
                return;
            }
        } else {
            missing_polls = 0;
            std::string text{};
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 256 * 1024) {
                text.resize((size_t)size.QuadPart);
                DWORD read = 0;
                if (!ReadFile(file, text.data(), (DWORD)text.size(), &read, nullptr)) {
                    read = 0;
                }
                text.resize(read);
            }
            CloseHandle(file);
            // (Its fields read loosely: a value of another type than expected counts as missing; "generating" may be a number.)
            const auto text_of = [](const nlohmann::json& j, const char* key, const char* otherwise) {
                const auto it = j.find(key);
                return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{otherwise};
            };
            const auto number_of = [](const nlohmann::json& j, const char* key, double otherwise) {
                const auto it = j.find(key);
                return it != j.end() && it->is_number() ? it->get<double>() : otherwise;
            };
            // (fix200) A session's identity (its sessionId hashed; 0 none: an empty or missing one).
            const auto session_hash = [](const std::string& id) -> uint64_t {
                if (id.empty()) {
                    return 0;
                }
                const uint64_t h = (uint64_t)std::hash<std::string>{}(id);
                return h != 0 ? h : 1;
            };
            const auto flag_of = [](const nlohmann::json& j, const char* key) {
                const auto it = j.find(key);
                return it != j.end() && ((it->is_boolean() && it->get<bool>()) || (it->is_number() && it->get<double>() != 0.0));
            };
            bool readable = false;
            try {
                const auto doc = nlohmann::json::parse(text, nullptr, false);
                if (!doc.is_discarded() && doc.is_object() && text_of(doc, "schema", "") == "vectorxr.status") {
                    readable = true;
                    const auto pid = (int64_t)number_of(doc, "processId", 0.0);
                    const auto updated = (int64_t)number_of(doc, "updatedAtUnixMilliseconds", 0.0);
                    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto fg_it = doc.find("framegen");
                    if (pid != (int64_t)GetCurrentProcessId()) {
                        summary = "its status file is another program's";
                    } else if (updated <= 0 || now_ms - updated >= 3000) {
                        summary = "its status file is stale";
                    } else if (fg_it == doc.end() || !fg_it->is_object()) {
                        summary = "Frame Generation off for this game";
                        // (fix200) Taken as off only from a session first seen after the latest instance was made (one named, not the
                        // session named when it was made: a file left from before it says nothing of the new session), and only when the
                        // part is absent (another shape: not taken as off).
                        const uint64_t session = session_hash(text_of(doc, "sessionId", ""));
                        m_vectorxr_last_session = session;
                        framegen_known = fg_it == doc.end() && session != 0 && session != m_vectorxr_instance_session.load() ? 0 : -1;
                    } else {
                        m_vectorxr_last_session = session_hash(text_of(doc, "sessionId", "")); // (fix200)
                        framegen_known = 1;
                        const auto& fg = *fg_it;
                        const std::string state = text_of(fg, "state", "unknown");
                        generating = flag_of(fg, "generating");
                        multiplier = generating ? (int)number_of(fg, "multiplier", 0.0) : 0;
                        summary = std::format("Frame Generation {} ({}{}, {} {})", state, generating ? "generating " : "not generating",
                            generating ? std::format("x{}", multiplier) : std::string{}, text_of(fg, "mode", "?"), text_of(fg, "frameRate", "?"));
                        fps = std::format(", {:.0f} game fps -> {:.0f} headset fps", number_of(fg, "appFps", 0.0), number_of(fg, "outputFps", 0.0));
                    }
                }
            } catch (...) {
                readable = false;
            }
            if (!readable) {
                return; // (half-written or another shape: as it was, tried again at the next poll)
            }
        }
    }

    // (fix200) For the Virtual Desktop fix (OpenXR::vectorxr_framegen): off only when our current status file says Frame Generation is
    // off for this game; on again as soon as one says it is on (a new session made with it).
    if (framegen_known >= 0) {
        const bool now_on = framegen_known == 1;
        if (runtimes::OpenXR::vectorxr_framegen.exchange(now_on) != now_on) {
            static int flips = 0; // (this thread only)
            if (flips++ < 10) {
                spdlog::info("[VR] (fix200) VectorXR's Frame Generation {}: the Virtual Desktop fix's empty layer {}", now_on ? "is on for this session"
                    : "is off for this game (its status file says so)", now_on ? "is never put before the world's picture"
                    : "goes first behind menus again where needed");
            }
        }
    }

    const bool changed_state = generating != m_vectorxr_generating.load() || multiplier != m_vectorxr_multiplier.load();
    m_vectorxr_generating = generating;
    m_vectorxr_multiplier = multiplier;
    static std::string previous{}; // (this thread only)
    {
        std::scoped_lock _{m_vectorxr_mtx};
        m_vectorxr_summary = summary + fps;
    }
    // (Logged as it changes: whether it generates, its multiplier, or what the file says; the fps alone changing is not logged.)
    static int logs = 0;
    if ((changed_state || previous != summary) && logs < 60) {
        ++logs;
        spdlog::info("[VR] (fix197) VectorXR: {}{}", summary, fps);
    }
    previous = summary;
    // AER with Frame Generation making frames: warned each time it starts, at most 10 times (Frame Generation takes the first projection
    // layer's views as one moment; AER's carry two).
    const bool conflict = generating && is_aer();
    static int conflict_logs = 0;
    if (conflict && !m_vectorxr_conflict_told && conflict_logs < 10) {
        ++conflict_logs;
        spdlog::warn("[VR] (fix197) VectorXR's Frame Generation is making frames while the eyes are in AER (one eye new each frame, the other's "
                     "older picture with it): it can't work with that; use mono or full stereo");
    }
    m_vectorxr_conflict_told = conflict;
}

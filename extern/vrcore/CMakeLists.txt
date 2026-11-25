# vrcore - open replacement for starfield2vr's private vrframework submodule.
# Builds a static library "vr_framework" (alias vr_framework::vr_framework) from
# REFramework-derived code (MIT) plus new code, and fetches its dependencies.
cmake_minimum_required(VERSION 3.27)

project(vrcore LANGUAGES C CXX)

include(FetchContent)

# Some pinned dependencies declare very old minimum CMake versions (needed for CMake 4.x).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# --- Dependencies (pinned to the commits REFramework uses) ------------------------------------

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG 7c02e204c92545f869e2f04edaab1f19fe8b19fd
)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm
    GIT_TAG bf71a834948186f4097caa076cd2663c69a10e1e
)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG 85b2fe8486190fa9326565a2fb5fccb6caea4396
)

set(BDD_INCLUDE_TOOL OFF CACHE BOOL "" FORCE)
set(BDD_INCLUDE_ISAGENERATOR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(bddisasm
    GIT_REPOSITORY https://github.com/bitdefender/bddisasm
    GIT_TAG 70db095765ab2066dd88dfb7bbcc42259ed167c5
)

set(KANANLIB_FETCH_BDDISASM OFF CACHE BOOL "" FORCE)
set(KANANLIB_FETCH_SPDLOG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(kananlib
    GIT_REPOSITORY https://github.com/cursey/kananlib
    GIT_TAG 43cb5353615dcbc5d4945b1b9ab685b11e8b0d37
)

set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook
    GIT_TAG b046e123dc69821f2c375161e0adef3c6d9c9db4
)

set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK
    GIT_TAG 458984d7f59d1ae6dc1b597d94b02e4f7132eaba
)

set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(directxtk12
    GIT_REPOSITORY https://github.com/microsoft/DirectXTK12
    GIT_TAG be5dfc7e391aefaa4eeab8e8a08f7e2669e56cbb
)

# OpenVR 1.16.8, the version REFramework vendors. Only headers and the import library are used.
# SOURCE_SUBDIR points at a folder without a CMakeLists.txt so the repo is only downloaded.
FetchContent_Declare(openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr
    GIT_TAG v1.16.8
    GIT_SHALLOW ON
    SOURCE_SUBDIR headers
)

FetchContent_MakeAvailable(spdlog glm imgui bddisasm kananlib safetyhook openxr directxtk12 openvr)

# --- ImGui (DX12 + Win32 backends) -----------------------------------------------------------

add_library(vrcore_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(vrcore_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_features(vrcore_imgui PUBLIC cxx_std_20)
if(DEFINED IMGUI_USER_CONFIG_PATH)
    target_compile_definitions(vrcore_imgui PUBLIC "IMGUI_USER_CONFIG=\"${IMGUI_USER_CONFIG_PATH}\"")
endif()
target_link_libraries(vrcore_imgui PUBLIC d3d12 dxgi d3dcompiler)

# --- OpenVR import target ----------------------------------------------------------------------

add_library(vrcore_openvr INTERFACE)
target_include_directories(vrcore_openvr INTERFACE ${openvr_SOURCE_DIR}/headers)
target_link_libraries(vrcore_openvr INTERFACE ${openvr_SOURCE_DIR}/lib/win64/openvr_api.lib)

# --- vrcore ----------------------------------------------------------------------------------------

file(GLOB_RECURSE vrcore_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE vrcore_headers CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)

add_library(vr_framework STATIC ${vrcore_sources} ${vrcore_headers})
add_library(vr_framework::vr_framework ALIAS vr_framework)

target_compile_features(vr_framework PUBLIC cxx_std_23)

target_include_directories(vr_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${openxr_SOURCE_DIR}/src
)

target_compile_definitions(vr_framework PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    GLM_ENABLE_EXPERIMENTAL
)

if(MSVC)
    target_compile_options(vr_framework PRIVATE /bigobj /EHa)
endif()

target_link_libraries(vr_framework PUBLIC
    spdlog::spdlog
    glm::glm
    vrcore_imgui
    kananlib
    safetyhook
    openxr_loader
    vrcore_openvr
    DirectXTK12
    d3d12
    dxgi
    d3dcompiler
    shlwapi
    dxguid
    delayimp
    windowscodecs
)

# The OpenVR and OpenXR loaders are optional at runtime: the mod ships with one or the other.
target_link_options(vr_framework INTERFACE
    "/DELAYLOAD:openxr_loader.dll"
    "/DELAYLOAD:openvr_api.dll"
)

# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-modelviewer repo as
# windows/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base`, `sr_common`, and `model_renderer` targets.
#
# windows/main.cpp + xr_session.cpp originated from displayxr-demo-gaussiansplat;
# the window/OpenXR/HUD/input glue is reused as-is and the renderer call sites
# are retargeted to model_common/ModelRenderer (glTF PBR). See ../PORTING.md
# for what's done (Windows v1) and what's still open (textures/IBL, macOS).

cmake_minimum_required(VERSION 3.21)

if(NOT WIN32)
    return()
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found — skipping model_viewer_handle_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# ── OpenXR loader ──────────────────────────────────────────────────────────
# Dev-build dependency rule (runtime CLAUDE.md § "Demo-repo dev-build rule"):
# a fresh clone must configure with only VS 2022 + Ninja + the Vulkan SDK, so
# the loader is self-provisioned here, never taken from a hardcoded SDK path.
# Resolution order:
#   1. find_package(OpenXR CONFIG): -DOpenXR_ROOT=<dir> / -DOpenXR_DIR=<dir> /
#      CMAKE_PREFIX_PATH. CI (build-windows.yml) takes this path with its own
#      download of the SAME pinned zip.
#   2. -DOpenXR_ROOT=<dir> laid out as lib/ + bin/ without a CMake config.
#   3. FetchContent of the pinned prebuilt Khronos loader zip (offline:
#      -DFETCHCONTENT_SOURCE_DIR_OPENXR_LOADER=<extracted zip dir>).
# Three pins must stay equal: this one == build-windows.yml's $openxrVersion
# == XR_CURRENT_API_VERSION in openxr_includes/openxr/openxr.h (checked below).
set(MODELVIEWER_OPENXR_LOADER_VERSION "1.1.51")
set(MODELVIEWER_OPENXR_LOADER_SHA256
    "01f1efbe1ff72da8dca2366c7ee2bc1d23f5bb00df3bedafd9337e55ba32521a")

file(STRINGS "${CMAKE_SOURCE_DIR}/openxr_includes/openxr/openxr.h" _xr_ver_line
    REGEX "^#define XR_CURRENT_API_VERSION XR_MAKE_VERSION")
string(REGEX MATCH "XR_MAKE_VERSION\\(([0-9]+), *([0-9]+), *([0-9]+)\\)" _ "${_xr_ver_line}")
set(_xr_header_ver "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
if(NOT _xr_header_ver STREQUAL MODELVIEWER_OPENXR_LOADER_VERSION)
    message(FATAL_ERROR
        "OpenXR loader pin (${MODELVIEWER_OPENXR_LOADER_VERSION}) != vendored "
        "openxr_includes header rev (${_xr_header_ver}). Bump "
        "MODELVIEWER_OPENXR_LOADER_VERSION + _SHA256 here AND $openxrVersion in "
        ".github/workflows/build-windows.yml together with the headers.")
endif()

set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "OpenXR loader: find_package (${OpenXR_DIR}, version ${OpenXR_VERSION})")
    if(DEFINED OpenXR_VERSION AND NOT OpenXR_VERSION VERSION_EQUAL MODELVIEWER_OPENXR_LOADER_VERSION)
        message(WARNING "OpenXR loader ${OpenXR_VERSION} found at ${OpenXR_DIR}, but this "
            "repo pins ${MODELVIEWER_OPENXR_LOADER_VERSION}. Drop -DOpenXR_ROOT/-DOpenXR_DIR "
            "to let the build provision the pinned loader itself.")
    endif()
endif()

if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS "${OpenXR_ROOT}/x64/lib" "${OpenXR_ROOT}/lib" "${OpenXR_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS "${OpenXR_ROOT}/x64/bin" "${OpenXR_ROOT}/bin"
            NO_DEFAULT_PATH
        )
        if(OPENXR_LOADER_DLL)
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        else()
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        endif()
        message(STATUS "OpenXR loader: OpenXR_ROOT (${OPENXR_LOADER_LIB})")
    endif()
endif()

if(NOT OPENXR_FOUND)
    # The Khronos release zip (x64 / Win32 / ARM64 + UWP variants) carries a
    # top-level OpenXRConfig.cmake that dispatches to the matching arch's
    # lib/cmake/openxr. No CMakeLists.txt at its root, so MakeAvailable only
    # downloads + extracts (into build/_deps, reused by every later configure).
    include(FetchContent)
    set(_oxr_url "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-${MODELVIEWER_OPENXR_LOADER_VERSION}/openxr_loader_windows-${MODELVIEWER_OPENXR_LOADER_VERSION}.zip")
    message(STATUS "OpenXR loader: provisioning ${MODELVIEWER_OPENXR_LOADER_VERSION} via FetchContent")
    FetchContent_Declare(openxr_loader
        URL      "${_oxr_url}"
        URL_HASH SHA256=${MODELVIEWER_OPENXR_LOADER_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(openxr_loader)
    find_package(OpenXR CONFIG
        PATHS "${openxr_loader_SOURCE_DIR}"
        NO_DEFAULT_PATH
    )
    if(OpenXR_FOUND)
        set(OPENXR_FOUND TRUE)
        message(STATUS "OpenXR loader: ${openxr_loader_SOURCE_DIR}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR
        "OpenXR loader not found and could not be provisioned. Offline: extract "
        "openxr_loader_windows-${MODELVIEWER_OPENXR_LOADER_VERSION}.zip somewhere and "
        "pass -DFETCHCONTENT_SOURCE_DIR_OPENXR_LOADER=<that dir> (or -DOpenXR_ROOT=<that dir>).")
endif()

add_executable(model_viewer_handle_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    resource.rc
)

target_include_directories(model_viewer_handle_vk_win PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
)

target_link_libraries(model_viewer_handle_vk_win PRIVATE
    sr_common_base
    sr_common
    model_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d11
    d2d1
    dwrite
    comdlg32
    # The undock launch contract: winhttp/bcrypt/shell32 arrive transitively
    # from displayxr::common (PUBLIC), but shell32 is named here too because
    # this TU calls CommandLineToArgvW / DragQueryFile directly — a transitive
    # dependency is the LIB's contract, not ours, and it has changed before.
    shell32
)

if(MSVC)
    # /utf-8: tell MSVC the source files are UTF-8 so multi-byte glyphs like
    # "…" in HUD button labels aren't mangled at compile time.
    target_compile_options(model_viewer_handle_vk_win PRIVATE /utf-8)

    file(RELATIVE_PATH _manifest_rel
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/model_viewer_openxr_ext_vk.manifest")
    set_target_properties(model_viewer_handle_vk_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${_manifest_rel}"
    )
endif()

# Copy OpenXR loader DLL next to exe. Use $<TARGET_FILE:...> rather than the
# IMPORTED_LOCATION property: the Khronos OpenXR CONFIG package only sets the
# *config-specific* IMPORTED_LOCATION_<CONFIG> (e.g. IMPORTED_LOCATION_RELWITHDEBINFO),
# leaving the generic IMPORTED_LOCATION unset. get_target_property(... IMPORTED_LOCATION)
# would return NOTFOUND, silently skipping the copy and shipping an exe with no
# loader next to it (the installer's `File openxr_loader.dll` then fails). The
# generator expression resolves the correct per-config DLL path.
if(TARGET OpenXR::openxr_loader)
    add_custom_command(TARGET model_viewer_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenXR::openxr_loader>"
            "$<TARGET_FILE_DIR:model_viewer_handle_vk_win>"
        COMMENT "Copying OpenXR loader DLL"
    )
endif()

# Vendor weaver DLLs (e.g. SimulatedRealityVulkanBeta.dll) are intentionally
# NOT bundled — they ship with the vendor plug-in installer and must not
# appear in a vendor-neutral DisplayXR demo.

# Copy bundled default model next to the exe, if present.
set(BUNDLED_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/assets/sample.glb")
if(EXISTS "${BUNDLED_MODEL}")
    add_custom_command(TARGET model_viewer_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_MODEL}" "$<TARGET_FILE_DIR:model_viewer_handle_vk_win>/sample.glb"
        COMMENT "Copying bundled sample.glb next to exe"
    )
endif()

# Material grid (issue #70): every supported material feature in one scene.
# Shipped next to the binary so it is one File > Open away, but deliberately NOT
# the startup scene — the bundled helmet stays the default.
set(MATERIAL_GRID "${CMAKE_SOURCE_DIR}/assets/material_grid.glb")
if(EXISTS "${MATERIAL_GRID}")
    add_custom_command(TARGET model_viewer_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MATERIAL_GRID}" "$<TARGET_FILE_DIR:model_viewer_handle_vk_win>/material_grid.glb"
        COMMENT "Copying material_grid.glb next to exe"
    )

# The KHR_materials_coat and fuzz/diffuse-roughness sweeps (#81, #84), carried
# next to the exe on the same terms as the material grid above. They are small
# (about 118 KB each against the bundled model's 3.7 MB), and shipping them means
# anyone reviewing the draft-extension work — including outside this repo — can
# open them from an installed build with Ctrl+O instead of needing a source
# checkout and a generator run.
foreach(_sweep coat_test diffuse_fuzz_test)
    add_custom_command(TARGET model_viewer_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/${_sweep}.glb"
                "$<TARGET_FILE_DIR:model_viewer_handle_vk_win>/${_sweep}.glb"
        COMMENT "Copying ${_sweep}.glb next to exe")
endforeach()
endif()

# Copy the DisplayXR shell sidecar (.displayxr.json + icons) next to the exe
# so the installer / shell scanner picks it up.
displayxr_install_manifest(model_viewer_handle_vk_win
    "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")

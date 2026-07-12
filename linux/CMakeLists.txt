# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-modelviewer repo as
# linux/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base` and `model_renderer` targets.
#
# linux/main.cpp is the Linux entry point (issue #40, M8 Linux epic
# runtime#699): system Vulkan (native ICD, no MoltenVK) + OpenXR session +
# ModelRenderer, in HOSTED-NULL windowing mode — the runtime self-creates the
# presentation window (no XR_EXT_*_window_binding). This is a BUILD-GREEN
# target (compiled on CI, not run — CI has no GPU/display). The faithful
# XR_DXR_xlib_window_binding arm is Phase-3b/hardware-gated; see
# ../PORTING.md and the runtime's docs/guides/linux-demo-port.md.

cmake_minimum_required(VERSION 3.21)

if(NOT (UNIX AND NOT APPLE))
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CONFIG package first (a dev/CI loader built to
# CMAKE_PREFIX_PATH, e.g. /tmp/openxr-install), else a plain find_library.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package (OpenXR::openxr_loader)")
endif()

if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}")
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Build the loader from source and pass "
                        "-DCMAKE_PREFIX_PATH=<install> (scripts/build_linux.sh does this).")
endif()

add_executable(model_viewer_handle_vk_linux
    main.cpp
    # The single stb_image implementation TU (displayxr::common only ships it
    # for Win/macOS; the model_common loaders take declarations only).
    stb_image_impl_linux.cpp
)

target_include_directories(model_viewer_handle_vk_linux PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/model_common
)

target_link_libraries(model_viewer_handle_vk_linux PRIVATE
    sr_common_base
    model_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
)

# Copy the OpenXR loader .so next to the exe when it's an imported target, so a
# dev run finds it without LD_LIBRARY_PATH munging.
if(TARGET OpenXR::openxr_loader)
    add_custom_command(TARGET model_viewer_handle_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenXR::openxr_loader>"
            "$<TARGET_FILE_DIR:model_viewer_handle_vk_linux>"
        COMMENT "Copying OpenXR loader next to model_viewer_handle_vk_linux")
endif()

# Copy the bundled default model next to the exe (auto-loaded at startup).
# Shared with the macOS build (macos/assets/sample.glb).
set(BUNDLED_MODEL "${CMAKE_SOURCE_DIR}/macos/assets/sample.glb")
if(EXISTS "${BUNDLED_MODEL}")
    add_custom_command(TARGET model_viewer_handle_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_MODEL}"
            "$<TARGET_FILE_DIR:model_viewer_handle_vk_linux>/sample.glb"
        COMMENT "Copying bundled sample.glb next to exe")
endif()

# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build — lives inside displayxr-demo-modelviewer repo as
# macos/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base` and `model_renderer` targets.
#
# NOTE: macos/main.mm is the PORTING BASELINE copied from
# displayxr-demo-gaussiansplat. The Cocoa/MoltenVK/OpenXR/HUD/input glue is
# reusable as-is; the renderer call sites still target the GS API and must be
# retargeted to ModelRenderer. See ../PORTING.md.

cmake_minimum_required(VERSION 3.21)

if(NOT APPLE)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — system install (brew), find_package, or user-provided root.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
endif()

if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}"
        )
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install via: brew install openxr-loader")
endif()

add_executable(model_viewer_handle_vk_macos
    main.mm
    # Vulkan host-readback path for the 'I' key atlas capture (mirrors the
    # Windows target — lives in common/ but isn't part of sr_common_base).
    ${CMAKE_SOURCE_DIR}/common/atlas_capture_vk.cpp
    # stb_image (read) implementation. model_loader.cpp takes stb declarations
    # only; on Windows the impl comes from common/d3d11_renderer.cpp, which is
    # Windows-only — macOS supplies it through this dedicated TU instead.
    ${CMAKE_SOURCE_DIR}/common/stb_image_impl_macos.cpp
)

target_include_directories(model_viewer_handle_vk_macos PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/model_common
)

target_link_libraries(model_viewer_handle_vk_macos PRIVATE
    sr_common_base
    model_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework UniformTypeIdentifiers"
)

# Copy bundled default model next to the exe (auto-loaded at startup), if present.
set(BUNDLED_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/assets/sample.glb")
if(EXISTS "${BUNDLED_MODEL}")
    add_custom_command(TARGET model_viewer_handle_vk_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_MODEL}" "$<TARGET_FILE_DIR:model_viewer_handle_vk_macos>/sample.glb"
        COMMENT "Copying bundled sample.glb next to exe"
    )
endif()

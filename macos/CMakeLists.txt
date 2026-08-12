# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-modelviewer repo as
# macos/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base` and `model_renderer` targets.
#
# macos/main.mm is the macOS entry point: Cocoa window + MoltenVK + OpenXR
# session + HUD/input, with the renderer call sites wired to ModelRenderer
# (loadModel / hasModel / getRobustSceneBounds, .glb/.gltf NSOpenPanel +
# drag-drop). No transparent-background mode on macOS (Windows-only).

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
    # stb_image (read) implementation TU comes from displayxr::common
    # (stb_image_impl_macos.cpp) — model_loader.cpp takes declarations only
    # (#396 W4).
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

# Material grid (issue #70): every supported material feature in one scene.
# Shipped next to the binary so it is one File > Open away, but deliberately NOT
# the startup scene — the bundled helmet stays the default.
set(MATERIAL_GRID "${CMAKE_SOURCE_DIR}/assets/material_grid.glb")
if(EXISTS "${MATERIAL_GRID}")
    add_custom_command(TARGET model_viewer_handle_vk_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MATERIAL_GRID}" "$<TARGET_FILE_DIR:model_viewer_handle_vk_macos>/material_grid.glb"
        COMMENT "Copying material_grid.glb next to exe"
    )

# The KHR_materials_coat and fuzz/diffuse-roughness sweeps (#81, #84), carried
# next to the exe on the same terms as the material grid above. They are small
# (about 118 KB each against the bundled model's 3.7 MB), and shipping them means
# anyone reviewing the draft-extension work — including outside this repo — can
# open them from an installed build with Ctrl+O instead of needing a source
# checkout and a generator run.
foreach(_sweep coat_test diffuse_fuzz_test)
    add_custom_command(TARGET model_viewer_handle_vk_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/${_sweep}.glb"
                "$<TARGET_FILE_DIR:model_viewer_handle_vk_macos>/${_sweep}.glb"
        COMMENT "Copying ${_sweep}.glb next to exe")
endforeach()
endif()

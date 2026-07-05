# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists.txt for the displayxr-demo-modelviewer standalone
# repo. Orchestrates the shared `common` and `model_common` libraries, then
# dispatches to macos/ or windows/ for the platform-specific executable.

cmake_minimum_required(VERSION 3.21)
project(displayxr-demo-modelviewer LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared libraries (built once, linked by the platform executable).
add_subdirectory(common)
add_subdirectory(model_common)

# Platform-specific executable.
if(APPLE)
    add_subdirectory(macos)
elseif(WIN32)
    add_subdirectory(windows)
elseif(UNIX)
    # Linux (build-green, hosted-NULL): issue #40, M8 Linux epic runtime#699.
    add_subdirectory(linux)
else()
    message(FATAL_ERROR "displayxr-demo-modelviewer supports macOS, Windows, and Linux only.")
endif()

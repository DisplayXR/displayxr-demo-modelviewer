// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  stb_image (read) implementation translation unit for macOS targets.
 *
 * model_loader.cpp decodes glTF textures with stb_image but takes the
 * *declarations only* (TINYGLTF_NO_STB_IMAGE + #include "stb_image.h"),
 * expecting the implementation to be linked in from elsewhere. On Windows that
 * implementation lives in common/d3d11_renderer.cpp (part of the D3D11-only
 * sr_common library). That file never builds on Apple, so macOS executables
 * need this dedicated TU to supply the read symbols (stbi_load_from_memory,
 * stbi_image_free, stbi_failure_reason, …).
 *
 * Mirrors how common/atlas_capture_macos.mm supplies
 * STB_IMAGE_WRITE_IMPLEMENTATION on macOS. Exactly one TU per link target may
 * define STB_IMAGE_IMPLEMENTATION; on macOS this is the only one (the Windows
 * definer, d3d11_renderer.cpp, is gated out).
 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

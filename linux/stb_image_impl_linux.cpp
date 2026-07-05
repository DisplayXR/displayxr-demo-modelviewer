// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// The single stb_image implementation translation unit for the Linux
// executable. displayxr::common ships the stb *implementation* only in its
// platform-gated sources (Windows: common/d3d11_renderer.cpp; macOS:
// common/stb_image_impl_macos.cpp), so on Linux neither is compiled and the
// model_common loaders (model_loader_gltf.cpp / model_loader_material.cpp,
// which take stb_image.h *declarations* only and call stbi_load_from_memory)
// would link with an unresolved symbol. Provide the impl here — exactly the
// role stb_image_impl_macos.cpp plays on Apple.
//
// tinygltf is built with TINYGLTF_NO_STB_IMAGE / TINYGLTF_NO_STB_IMAGE_WRITE
// (see model_loader_gltf.cpp), so this is the ONLY STB_IMAGE_IMPLEMENTATION in
// the link — no double-definition.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

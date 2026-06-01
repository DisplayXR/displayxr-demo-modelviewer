// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  glTF 2.0 loader implementation (tinygltf).
 *
 * This is the single translation unit that defines the tinygltf
 * implementation. SKELETON: parses the file and counts primitives + computes
 * the position AABB; does not yet extract vertex/material data. See
 * ../PORTING.md.
 */

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE 0
#include <tiny_gltf.h>

#include "model_loader.h"

#include <cstdio>
#include <cstring>
#include <string>

bool model_loader_load(const char* gltfPath, ModelData& out) {
    if (!gltfPath) return false;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    const std::string path = gltfPath;
    const bool isBinary =
        path.size() >= 4 &&
        (std::strcmp(path.c_str() + path.size() - 4, ".glb") == 0 ||
         std::strcmp(path.c_str() + path.size() - 4, ".GLB") == 0);

    const bool ok = isBinary
        ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
        : loader.LoadASCIIFromFile(&model, &err, &warn, path);

    if (!warn.empty()) std::fprintf(stderr, "[model_loader] warn: %s\n", warn.c_str());
    if (!ok) {
        std::fprintf(stderr, "[model_loader] error: %s\n",
                     err.empty() ? "unknown parse error" : err.c_str());
        return false;
    }

    uint32_t prims = 0;
    for (const auto& mesh : model.meshes) prims += (uint32_t)mesh.primitives.size();
    out.primitiveCount = prims;

    // TODO(port): walk model.nodes/meshes/accessors → interleaved vertices +
    // indices; collect materials + texture image sources; compute the real
    // AABB from POSITION accessor min/max. Then upload in model_renderer.cpp.

    std::fprintf(stderr, "[model_loader] parsed '%s': %u meshes, %u primitives\n",
                 gltfPath, (unsigned)model.meshes.size(), prims);
    return true;
}

// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Thin CPU-side glTF 2.0 loader wrapper (tinygltf).
 *
 * Parses a .glb/.gltf into a flat, GPU-upload-friendly form. This is the
 * vendor-neutral analog of 3dgs_common/gs_scene_loader.h. The Vulkan upload
 * itself lives in model_renderer.cpp.
 *
 * SKELETON: model_loader_load() currently only confirms the file parses and
 * reports a primitive count. Flesh out ModelData (interleaved vertices,
 * indices, materials, textures, node transforms) during the port — or
 * replace this wrapper wholesale with vkglTF::Model from Vulkan-glTF-PBR,
 * which folds parsing + Vulkan upload into one class. See ../PORTING.md.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ModelData {
    uint32_t primitiveCount = 0;
    // TODO(port): std::vector<Vertex> vertices; std::vector<uint32_t> indices;
    // std::vector<Material> materials; std::vector<TextureSource> textures;
    // node hierarchy + per-node local transforms; min/max AABB.
    float bboxMin[3] = {0, 0, 0};
    float bboxMax[3] = {0, 0, 0};
};

// Parse a glTF 2.0 file (.glb or .gltf). Returns false on parse failure.
bool model_loader_load(const char* gltfPath, ModelData& out);

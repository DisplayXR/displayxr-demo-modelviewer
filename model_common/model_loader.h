// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU-side glTF 2.0 loader (tinygltf) → flat GPU-upload-friendly form.
 *
 * Vendor-neutral analog of 3dgs_common/gs_scene_loader.h. Walks the default
 * scene's node hierarchy, bakes each node's world transform, and flattens
 * every mesh primitive into a single interleaved vertex buffer + index buffer
 * with per-primitive draw ranges, materials, and model matrices.
 *
 * v1 scope: static geometry (position + normal + uv0), metallic-roughness
 * material FACTORS only. Texture image decode/upload is a follow-up — tinygltf
 * is built here with no stb coupling (TINYGLTF_NO_STB_IMAGE*) to avoid a
 * duplicate-symbol clash with common/ (d3d11_renderer.cpp already defines the
 * stb implementation). Skinning/animation/morph targets are also follow-ups.
 * See ../PORTING.md.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ModelVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

struct ModelMaterial {
    float baseColorFactor[4] = {1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3] = {0, 0, 0};
    int   baseColorTexture = -1;  // reserved for the texture follow-up
};

struct ModelPrimitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int      material = -1;        // index into ModelData::materials, or -1
    float    modelMatrix[16];      // baked node world transform (column-major)
};

struct ModelData {
    std::vector<ModelVertex>    vertices;
    std::vector<uint32_t>       indices;
    std::vector<ModelMaterial>  materials;
    std::vector<ModelPrimitive> primitives;

    uint32_t primitiveCount = 0;
    // World-space AABB over all primitives (node transforms applied).
    float bboxMin[3] = {0, 0, 0};
    float bboxMax[3] = {0, 0, 0};
    bool  hasBBox = false;
};

// Parse a glTF 2.0 file (.glb or .gltf). Returns false on parse failure or if
// no drawable geometry was found.
bool model_loader_load(const char* gltfPath, ModelData& out);

// ── Path helpers (replace the GS scene-loader's .ply/.spz equivalents) ────
// True if the file exists and has a supported (.glb / .gltf) extension.
bool model_validate_file(const std::string& path);
// Filename without directory.
std::string model_basename(const std::string& path);
// Human-readable file size, e.g. "12.3 MB" (or "unknown" on error).
std::string model_filesize_str(const std::string& path);

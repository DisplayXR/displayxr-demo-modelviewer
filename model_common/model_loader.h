// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU-side glTF 2.0 loader (tinygltf) → flat GPU-upload-friendly form.
 *
 * Vendor-neutral analog of 3dgs_common/gs_scene_loader.h. Walks the default
 * scene's node hierarchy, bakes each node's world transform, flattens every
 * mesh primitive into one interleaved vertex buffer + index buffer, and
 * decodes material textures (base-color, metallic-roughness, normal,
 * occlusion, emissive) to RGBA8.
 *
 * Scope: static geometry (position + normal + uv0). Skinning/animation/morph
 * targets are follow-ups. tinygltf's bundled stb is compiled file-local
 * (STB_IMAGE_STATIC) so it doesn't clash with common/'s stb implementation.
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

// Decoded RGBA8 texture image. Indices below reference ModelData::textures.
struct ModelTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
};

struct ModelMaterial {
    float baseColorFactor[4] = {1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3] = {0, 0, 0};
    // Texture indices into ModelData::textures, or -1 when absent (the
    // renderer then binds a glTF-correct default: white, or flat normal).
    int baseColorTex = -1;
    int metallicRoughnessTex = -1;
    int normalTex = -1;
    int occlusionTex = -1;
    int emissiveTex = -1;
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
    std::vector<ModelTexture>   textures;
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
bool model_validate_file(const std::string& path);
std::string model_basename(const std::string& path);
std::string model_filesize_str(const std::string& path);

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
 * Scope: position + normal + uv0 geometry, plus node/TRS animation (Phase 1 —
 * the node graph + animations[] are retained for per-frame world-matrix
 * recompute). Skinning/morph targets are follow-ups. tinygltf's bundled stb is
 * compiled file-local
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
    int      node = -1;            // owning node (index into ModelData::nodes);
                                   // used to re-fetch the animated world matrix
    float    modelMatrix[16];      // baked node world transform (column-major).
                                   // Static fast-path value; overwritten per
                                   // frame when an animation drives this node.
};

// ── Animation (Phase 1: node TRS only; no skinning/morph) ────────────────────
// The node hierarchy is retained so world matrices can be recomputed per frame.
// Header stays glm-free (included by platform code); transforms are plain float
// arrays, consistent with ModelPrimitive::modelMatrix.

struct ModelNode {
    int parent = -1;               // -1 = root
    std::vector<int> children;     // indices into ModelData::nodes
    int mesh = -1;                 // index into the source mesh list, or -1
    // Base local TRS (the bind-pose values from the glTF node). Animation
    // channels override these per frame; untargeted components keep the base.
    float translation[3] = {0, 0, 0};
    float rotation[4]    = {0, 0, 0, 1};   // quaternion (x, y, z, w)
    float scale[3]       = {1, 1, 1};
    bool  hasMatrix = false;       // node specified an explicit local matrix
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // used if hasMatrix
};

enum class AnimInterp { Linear, Step, CubicSpline };
enum class AnimPath   { Translation, Rotation, Scale, Weights };  // Weights = morph (Phase 3)

struct AnimSampler {
    std::vector<float> input;      // keyframe times (seconds), ascending
    std::vector<float> output;     // flattened values; element stride from path.
                                   // CUBICSPLINE packs 3 elems/key: in,val,out.
    AnimInterp interp = AnimInterp::Linear;
};

struct AnimChannel {
    int       targetNode = -1;     // index into ModelData::nodes
    AnimPath  path = AnimPath::Translation;
    int       sampler = -1;        // index into Animation::samplers
};

struct Animation {
    std::string name;
    std::vector<AnimSampler> samplers;
    std::vector<AnimChannel> channels;
    float duration = 0.0f;         // max last-input time across samplers (seconds)
};

struct ModelData {
    std::vector<ModelVertex>    vertices;
    std::vector<uint32_t>       indices;
    std::vector<ModelTexture>   textures;
    std::vector<ModelMaterial>  materials;
    std::vector<ModelPrimitive> primitives;

    // Node graph + clips (retained for per-frame animation). Empty when the
    // model has no animations → renderer keeps the once-baked static matrices.
    std::vector<ModelNode>      nodes;
    std::vector<Animation>      animations;
    std::vector<int>            rootNodes;   // scene roots (indices into nodes)

    uint32_t primitiveCount = 0;
    // World-space AABB over all primitives (bind-pose node transforms applied).
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

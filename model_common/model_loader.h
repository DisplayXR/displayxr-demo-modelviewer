// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
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
    float    pos[3];
    float    normal[3];
    float    uv[2];
    // Skinning (Phase 2). All-zero weights0 ⇒ vertex is not skinned; the
    // renderer flags non-skinned primitives so the shader keeps the static
    // push-constant model path. JOINTS_0 (u8/u16) is widened to u16; WEIGHTS_0
    // (float or normalized int) is decoded to float at load.
    uint16_t joints0[4]  = {0, 0, 0, 0};
    float    weights0[4] = {0, 0, 0, 0};
    // glTF TANGENT: xyz = tangent, w = bitangent handedness (±1). All-zero when
    // the asset has none, which the shader treats as "fall back to the
    // screen-space-derivative frame". A real tangent matters for anisotropy,
    // whose whole direction is DEFINED in this frame — the derivative frame
    // flips across UV seams and degenerates at poles (issue #70).
    float    tangent[4]  = {0, 0, 0, 0};
};

// Decoded RGBA8 texture image. Indices below reference ModelData::textures.
struct ModelTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
};

// Texture slot order, shared by the loader and the renderer's descriptor set.
enum ModelTexSlot {
    MTS_BASE_COLOR = 0, MTS_MR, MTS_NORMAL, MTS_OCCLUSION, MTS_EMISSIVE,
    MTS_CLEARCOAT, MTS_CLEARCOAT_ROUGH, MTS_SHEEN_COLOR, MTS_SHEEN_ROUGH,
    MTS_SPECULAR, MTS_SPECULAR_COLOR, MTS_TRANSMISSION, MTS_THICKNESS,
    MTS_SCATTER_STRENGTH, MTS_MULTISCATTER_COLOR,
    MTS_COAT_COLOR, MTS_COAT_ANISOTROPY,
    MTS_DIFFUSE_ROUGHNESS, MTS_FUZZ, MTS_COAT_NORMAL,
    MTS_COUNT
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

    // glTF: a material is SINGLE-sided unless it says otherwise, and a
    // single-sided material's back faces must not be rendered (spec 3.9.4).
    // The default here is `true` — the opposite of glTF's — on purpose: only
    // the glTF backend actually knows this flag, and STL/OBJ/FBX/USD assets
    // carry no reliable winding, so they keep the renderer's historical
    // two-sided behaviour rather than silently losing faces. The glTF loader
    // overwrites it from `material.doubleSided`.
    bool doubleSided = true;

    // ── KHR_materials_* (issue #70 phase 2) ──────────────────────────────────
    // Defaults are the glTF-specified "extension absent" values, so a material
    // that declares none of these behaves exactly as it did before the
    // extensions existed. Texture-driven variants of these factors
    // (clearcoatTexture, sheenColorTexture, …) are not read yet — factors only.
    float ior = 1.5f;                       // KHR_materials_ior (1.5 = f0 0.04)
    float specularFactor = 1.0f;            // KHR_materials_specular
    float specularColorFactor[3] = {1, 1, 1};
    float clearcoatFactor = 0.0f;           // KHR_materials_clearcoat
    float clearcoatRoughness = 0.0f;
    float sheenColorFactor[3] = {0, 0, 0};  // KHR_materials_sheen (black = off)
    float sheenRoughness = 0.0f;
    float emissiveStrength = 1.0f;          // KHR_materials_emissive_strength
    float anisotropyStrength = 0.0f;        // KHR_materials_anisotropy
    float anisotropyRotation = 0.0f;        // radians
    float iridescenceFactor = 0.0f;         // KHR_materials_iridescence (0 = off)
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMin = 100.0f; // nanometres
    float iridescenceThicknessMax = 400.0f;
    float transmissionFactor = 0.0f;        // KHR_materials_transmission (0 = opaque)
    float volumeThickness = 0.0f;           // KHR_materials_volume (0 = thin surface)
    float attenuationColor[3] = {1, 1, 1};
    // glTF's default is +Infinity, i.e. no absorption. 0 is the sentinel for
    // that here so the shader can guard with a finite comparison.
    float attenuationDistance = 0.0f;

    // KHR_materials_scatter (draft; issue #79). Subsurface / multiple scattering,
    // the extension that closes the SSS gap. Defaults are the spec's: strength 0
    // means the material is unaffected, so an asset without the extension shades
    // exactly as it did before.
    float scatterStrength = 0.0f;             // [0,1]; 0 = no scattering
    float multiscatterColor[3] = {1, 1, 1};   // multi-scatter albedo (linear)
    float scatterAnisotropy = 0.0f;           // (-1,1) Henyey-Greenstein g

    // KHR_materials_coat (draft; issue #81). A superset of KHR_materials_clearcoat
    // that the spec maps onto it 1:1, so coatFactor/coatRoughnessFactor and their
    // textures land in the clearcoat fields above and one shader lobe serves both.
    // These are the properties coat ADDS.
    //
    // The defaults here are the values that make a clearcoat-only asset shade
    // exactly as it did before coat existed, which is NOT the same as the spec's
    // defaults: coatIor 1.5 gives f0 = ((1.5-1)/(1.5+1))^2 = 0.04, the constant
    // the clearcoat lobe used to hardcode, but coatDarkening must default to 0
    // here where the spec says 1.0. Darkening is physically correct and the spec
    // turns it on; KHR_materials_clearcoat never had it, so applying it to a
    // clearcoat asset would change that asset's appearance. hasCoat is what
    // distinguishes the two cases — the parser sets darkening to the spec's 1.0
    // only when KHR_materials_coat is actually present.
    float coatIor = 1.5f;
    float coatDarkening = 0.0f;         // spec default 1.0, but only when hasCoat
    float coatAnisotropyStrength = 0.0f;
    float coatAnisotropyRotation = 0.0f;  // radians
    float coatColor[3] = {1, 1, 1};     // linear
    bool  hasCoat = false;

    // KHR_materials_diffuse_roughness (draft; issue #84). Roughness of the
    // DIFFUSE substrate, independent of the specular roughness above. 0 is
    // Lambertian, i.e. the behaviour of every asset that does not use it.
    //
    // NOTE the spec contradicts itself on the default: the README property table
    // says 0.0 and the JSON schema says 1.0. 0.0 is the only value that leaves
    // existing assets alone, so that is what this takes. Raised upstream.
    float diffuseRoughness = 0.0f;

    // KHR_materials_fuzz (draft; issue #84). Intended to REPLACE
    // KHR_materials_sheen: same Charlie-family lobe, but sitting above the coat
    // rather than below it, and with a real weight so the layer can be darker
    // than what it covers (black soot) instead of a black colour simply
    // disabling it.
    //
    // fuzzColor and fuzzRoughness are NOT stored here. They use the identical
    // texture channels as their sheen counterparts (RGB sRGB, alpha), so the
    // loader writes them into sheenColorFactor / sheenRoughness and their
    // texture slots, and `hasFuzz` tells the shader to read those lanes as fuzz
    // and to layer them above the coat. Only the weight is genuinely new.
    float fuzzFactor = 0.0f;
    bool  hasFuzz = false;

    // KHR_texture_transform: a per-TEXTURE-SLOT UV transform. Slot order is
    // ModelTexSlot below, which ModelRenderer::MaterialTexSlot mirrors (a
    // static_assert keeps the two from drifting).
    //
    // This became load-bearing with KHR_materials_scatter (#79): the conformance
    // asset rotates scatterStrengthTexture 90 degrees, and a 90-degree rotation
    // changes essentially every texel of it (measured mean |a - rot90(a)| = 127
    // on 0..255). Sampling it untransformed is simply wrong.
    struct UvTransform {
        float offset[2] = {0.0f, 0.0f};
        float scale[2]  = {1.0f, 1.0f};
        float rotation  = 0.0f;          // radians
    };

    // Texture-driven variants of the factors above. Indices into
    // ModelData::textures, or -1. Each samples the channel the extension
    // specifies and MULTIPLIES the corresponding factor, per glTF.
    int clearcoatTex = -1;           // R
    int clearcoatRoughnessTex = -1;  // G
    int sheenColorTex = -1;          // RGB (sRGB-encoded)
    int sheenRoughnessTex = -1;      // A
    int specularTex = -1;            // A
    int specularColorTex = -1;       // RGB (sRGB-encoded)
    int transmissionTex = -1;        // R
    int thicknessTex = -1;           // G
    int scatterStrengthTex = -1;     // A (KHR_materials_scatter)
    int multiscatterColorTex = -1;   // RGB (sRGB-encoded)
    int coatColorTex = -1;           // RGB (sRGB-encoded) (KHR_materials_coat)
    int coatAnisotropyTex = -1;      // B = strength, RG = rotation vector
    int diffuseRoughnessTex = -1;    // R (KHR_materials_diffuse_roughness)
    int fuzzTex = -1;                // R (KHR_materials_fuzz)
    int coatNormalTex = -1;          // tangent-space normal for the coat layer

    // Per-slot UV transforms, indexed by ModelTexSlot. Identity by default, so a
    // texture without the extension samples exactly as it did before.
    UvTransform uvXf[MTS_COUNT];
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
    int      skin = -1;            // glTF skin index, or -1 (not skinned). When
                                   // ≥0 the renderer uses an identity model
                                   // matrix and the joint-matrix SSBO instead.
    int      jointBase = 0;        // offset of this skin's joints in the flat
                                   // joint-matrix array (ModelData::totalJoints).
    uint32_t firstVertex = 0;      // base vertex index (a primitive's verts are
    uint32_t vertexCount = 0;      // contiguous) — the range the morph blend writes.
    int      morph = -1;           // index into ModelData::morphs, or -1 (no targets).
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
    // Live morph weights (Phase 3). Size = the node's mesh's morph-target count;
    // seeded from node/mesh defaults, overwritten per frame by a Weights channel.
    std::vector<float> weights;
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

// ── Skinning (Phase 2) ───────────────────────────────────────────────────────
// Per frame the renderer computes jointMatrix[i] = nodeWorld[joints[i]] *
// inverseBind[i] (the skinned mesh node's own transform is intentionally NOT
// applied — glTF ignores it for skinned meshes; vertices stay in skin space and
// the draw uses an identity model matrix).
struct ModelSkin {
    std::vector<int>   joints;       // node indices (this skin's joint list)
    std::vector<float> inverseBind;  // 16 floats/joint (MAT4, column-major);
                                     // identity per joint if the accessor is absent
};

// ── Morph targets (Phase 3) ──────────────────────────────────────────────────
// Per-vertex position/normal deltas for each target of one primitive. The
// renderer blends morphed = base + Σ weightᵢ·deltaᵢ on the CPU each frame.
// Deltas are flat: target t, vertex v, component c → [(t*vertexCount + v)*3 + c].
struct ModelMorph {
    uint32_t targetCount = 0;
    uint32_t vertexCount = 0;
    std::vector<float> posDeltas;    // targetCount * vertexCount * 3
    std::vector<float> nrmDeltas;    // same layout; empty when no NORMAL deltas
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

    // Skins (Phase 2). Empty when the model has no skinned meshes. Joint
    // matrices for every skin are packed back-to-back; a primitive's jointBase
    // is its skin's offset into that flat array.
    std::vector<ModelSkin>      skins;
    uint32_t totalJoints = 0;                // sum of joint counts across skins

    // Morph targets (Phase 3). Empty when the model has none. A primitive's
    // ModelPrimitive::morph indexes into this; its owning node holds the weights.
    std::vector<ModelMorph>     morphs;

    uint32_t primitiveCount = 0;
    // World-space AABB over all primitives (bind-pose node transforms applied).
    float bboxMin[3] = {0, 0, 0};
    float bboxMax[3] = {0, 0, 0};
    bool  hasBBox = false;

    // glTF extensions the file declares in `extensionsUsed` that this renderer
    // does NOT implement (issue #70). Recorded so the viewer can say so out
    // loud: an unimplemented KHR_materials_* extension is not a load failure —
    // the base metallic-roughness layer still renders, which is the correct
    // fallback — but it IS a silent difference from what the author saw, and a
    // material-fidelity demo must never let that pass unremarked. Empty for a
    // file that uses nothing we lack. Populated by the glTF backend only.
    std::vector<std::string> unsupportedExtensions;
};

// Parse a glTF 2.0 file (.glb or .gltf). Returns false on parse failure or if
// no drawable geometry was found.
bool model_loader_load(const char* gltfPath, ModelData& out);

// ── Path helpers (replace the GS scene-loader's .ply/.spz equivalents) ────
bool model_validate_file(const std::string& path);
std::string model_basename(const std::string& path);
std::string model_filesize_str(const std::string& path);

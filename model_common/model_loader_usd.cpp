// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  USD(Z) loader backend (tinyusdz / tydra) → ModelData.
 *
 * Routed to from model_loader_load() for .usdz/.usd/.usda/.usdc. We load the
 * stage and let tydra's RenderSceneConverter flatten it into a RenderScene of
 * triangulated, single-indexable meshes + resolved UsdPreviewSurface materials
 * (tydra's `triangulate` + `build_vertex_indices` both default to true, so every
 * mesh's points/normals/texcoords share one vertex index — `faceVertexIndices()`
 * indexes all three). USD is right-handed Y-up like glTF, so no axis flip.
 *
 * USD is PBR-native (UsdPreviewSurface = metallic-roughness): diffuseColor →
 * baseColor, metallic/roughness/emissiveColor map straight across — no Phong
 * shim. Textures resolve via the shader param's texture_id → RenderScene
 * textures → images → embedded buffer (USDZ) or asset path, decoded through the
 * shared model_loader_material helper. Normal-map + combined metallic-roughness
 * textures are a follow-up (UsdPreviewSurface keeps metal/rough as separate
 * single-channel maps; factors are honoured today).
 */

#include "tinyusdz.hh"
#include "tydra/render-data.hh"

#include "model_loader_backends.h"
#include "model_loader_material.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

namespace tt = tinyusdz::tydra;

// Resolve a tydra shader-param texture_id → ModelData texture index (or -1):
// texture → image → embedded buffer bytes (USDZ) or external asset path.
int loadUsdTexture(const tt::RenderScene& scene, int32_t texId,
                   const std::filesystem::path& dir, ModelData& out) {
    if (texId < 0 || texId >= (int32_t)scene.textures.size()) return -1;
    const int64_t imgId = scene.textures[(size_t)texId].texture_image_id;
    if (imgId < 0 || imgId >= (int64_t)scene.images.size()) return -1;
    const tt::TextureImage& img = scene.images[(size_t)imgId];

    // Embedded, still-encoded image data (the common USDZ case) → decode.
    if (!img.decoded && img.buffer_id >= 0 && img.buffer_id < (int64_t)scene.buffers.size()) {
        const std::vector<uint8_t>& data = scene.buffers[(size_t)img.buffer_id].data;
        if (!data.empty()) {
            const int idx = material_load_texture_memory(data.data(), (int)data.size(), out);
            if (idx >= 0) return idx;
        }
    }
    // External asset (typical for .usda referencing image files).
    if (!img.asset_identifier.empty()) {
        int idx = material_load_texture_file(img.asset_identifier, out);
        if (idx >= 0) return idx;
        return material_load_texture_file((dir / img.asset_identifier).string(), out);
    }
    return -1;
}

void accumulateBBox(ModelData& out, const float* p) {
    if (!out.hasBBox) {
        out.bboxMin[0] = out.bboxMax[0] = p[0];
        out.bboxMin[1] = out.bboxMax[1] = p[1];
        out.bboxMin[2] = out.bboxMax[2] = p[2];
        out.hasBBox = true;
    } else {
        out.bboxMin[0] = std::min(out.bboxMin[0], p[0]); out.bboxMax[0] = std::max(out.bboxMax[0], p[0]);
        out.bboxMin[1] = std::min(out.bboxMin[1], p[1]); out.bboxMax[1] = std::max(out.bboxMax[1], p[1]);
        out.bboxMin[2] = std::min(out.bboxMin[2], p[2]); out.bboxMax[2] = std::max(out.bboxMax[2], p[2]);
    }
}

}  // namespace

bool model_load_usd(const char* path, ModelData& out) {
    if (!path) return false;
    const std::filesystem::path modelDir = std::filesystem::path(path).parent_path();

    tinyusdz::Stage stage;
    std::string warn, err;
    if (!tinyusdz::LoadUSDFromFile(path, &stage, &warn, &err)) {  // auto-detects usdz/usda/usdc
        std::fprintf(stderr, "[model_loader/usd] '%s': %s\n", path,
                     err.empty() ? "load failed" : err.c_str());
        return false;
    }
    if (!warn.empty()) std::fprintf(stderr, "[model_loader/usd] warn: %s\n", warn.c_str());

    tt::RenderScene scene;
    tt::RenderSceneConverter converter;
    tt::RenderSceneConverterEnv env(stage);
    env.set_search_paths({ modelDir.string() });
    if (!converter.ConvertToRenderScene(env, &scene)) {
        std::fprintf(stderr, "[model_loader/usd] '%s': RenderScene convert failed: %s\n",
                     path, converter.GetError().c_str());
        return false;
    }

    // ── Materials (RenderScene index ↔ ModelData index, 1:1; trailing default) ──
    for (const tt::RenderMaterial& rm : scene.materials) {
        ModelMaterial mm{};
        mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
        mm.baseColorFactor[3] = 1.0f;
        mm.metallic = 0.0f;
        mm.roughness = 0.8f;
        if (rm.surfaceShader.has_value()) {
            const tt::PreviewSurfaceShader& sh = *rm.surfaceShader;
            const auto dc = sh.diffuseColor.value;
            mm.baseColorFactor[0] = dc[0]; mm.baseColorFactor[1] = dc[1]; mm.baseColorFactor[2] = dc[2];
            mm.baseColorFactor[3] = sh.opacity.value;
            mm.metallic  = sh.metallic.value;
            mm.roughness = sh.roughness.value;
            const auto ec = sh.emissiveColor.value;
            mm.emissive[0] = ec[0]; mm.emissive[1] = ec[1]; mm.emissive[2] = ec[2];
            if (sh.diffuseColor.is_texture())
                mm.baseColorTex = loadUsdTexture(scene, sh.diffuseColor.texture_id, modelDir, out);
            if (sh.emissiveColor.is_texture())
                mm.emissiveTex = loadUsdTexture(scene, sh.emissiveColor.texture_id, modelDir, out);
        }
        out.materials.push_back(mm);
    }
    const int defaultMat = (int)out.materials.size();
    {
        ModelMaterial mm{};
        mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
        mm.baseColorFactor[3] = 1.0f;
        mm.metallic = 0.0f; mm.roughness = 0.8f;
        out.materials.push_back(mm);
    }

    // ── Meshes → one ModelPrimitive each (single material per RenderMesh) ──
    for (const tt::RenderMesh& mesh : scene.meshes) {
        const size_t vcount = mesh.points.size();
        const std::vector<uint32_t>& faceIdx = mesh.faceVertexIndices();
        if (vcount == 0 || faceIdx.empty()) continue;

        const float* pos = reinterpret_cast<const float*>(mesh.points.data());  // vec3 = 3 tight floats

        const bool hasN  = !mesh.normals.empty() && mesh.normals.vertex_count() >= vcount;
        const float* nrm = hasN ? reinterpret_cast<const float*>(mesh.normals.get_data().data()) : nullptr;

        auto uvIt = mesh.texcoords.find(0);
        const bool hasUV = uvIt != mesh.texcoords.end() &&
                           uvIt->second.vertex_count() >= vcount;
        const float* uv  = hasUV ? reinterpret_cast<const float*>(uvIt->second.get_data().data()) : nullptr;

        ModelPrimitive mp{};
        mp.firstVertex = (uint32_t)out.vertices.size();
        mp.firstIndex  = (uint32_t)out.indices.size();
        mp.material = (mesh.material_id >= 0 && mesh.material_id < (int)scene.materials.size())
                          ? mesh.material_id : defaultMat;
        mp.node = -1; mp.skin = -1; mp.morph = -1;
        std::memset(mp.modelMatrix, 0, sizeof(mp.modelMatrix));
        mp.modelMatrix[0] = mp.modelMatrix[5] = mp.modelMatrix[10] = mp.modelMatrix[15] = 1.0f;

        for (size_t v = 0; v < vcount; ++v) {
            ModelVertex mv{};
            mv.pos[0] = pos[v*3+0]; mv.pos[1] = pos[v*3+1]; mv.pos[2] = pos[v*3+2];
            if (nrm) { mv.normal[0] = nrm[v*3+0]; mv.normal[1] = nrm[v*3+1]; mv.normal[2] = nrm[v*3+2]; }
            else     { mv.normal[1] = 1.0f; }
            if (uv)  { mv.uv[0] = uv[v*2+0]; mv.uv[1] = 1.0f - uv[v*2+1]; }  // USD V bottom-up → flip
            out.vertices.push_back(mv);
            accumulateBBox(out, mv.pos);
        }
        for (uint32_t idx : faceIdx) {
            if (idx < vcount) out.indices.push_back(mp.firstVertex + idx);
        }
        mp.vertexCount = (uint32_t)vcount;
        mp.indexCount  = (uint32_t)out.indices.size() - mp.firstIndex;
        if (mp.indexCount > 0) out.primitives.push_back(mp);
    }

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader/usd] '%s': no drawable triangle geometry\n", path);
        return false;
    }
    std::fprintf(stderr,
        "[model_loader/usd] '%s': %zu meshes, %u prims, %zu verts, %zu materials, %zu textures\n",
        path, scene.meshes.size(), out.primitiveCount, out.vertices.size(),
        out.materials.size(), out.textures.size());
    return true;
}

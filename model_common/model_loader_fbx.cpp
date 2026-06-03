// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  FBX loader backend (ufbx) → ModelData.
 *
 * Routed to from model_loader_load() for .fbx. ufbx parses the scene with axis +
 * unit conversion to glTF conventions (right-handed Y-up, metres), so FBX's
 * usual cm / Z-up content lands correctly. For each mesh instance we bake the
 * node's geometry_to_world into the positions (identity model matrix downstream,
 * matching the other backends), triangulate every face, and split by material
 * via ufbx's `material_parts` → one ModelPrimitive per (instance, material).
 *
 * Materials prefer the PBR maps (base_color / metalness / roughness / emission /
 * normal); when a value is absent we fall back to the legacy FBX (Phong) maps,
 * shimming specular_exponent → roughness via model_loader_material.h. Textures
 * come from the embedded blob when present, else the resolved file path. Static
 * geometry only — skinning / animation are a deferred phase.
 *
 * This TU compiles the vendored ufbx implementation indirectly: ufbx.c is its
 * own C translation unit in the build; here we only use the public API.
 */

#include "third_party/ufbx.h"

#include "model_loader_backends.h"
#include "model_loader_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string toStr(const ufbx_string& s) {
    return (s.data && s.length) ? std::string(s.data, s.length) : std::string();
}

// Decode a ufbx texture → index into out.textures (or -1). Embedded blob first,
// then the resolved absolute path, then the relative path against the model dir.
int loadUfbxTexture(const ufbx_texture* tex, const std::filesystem::path& dir, ModelData& out) {
    if (!tex) return -1;
    if (tex->content.size > 0)
        return material_load_texture_memory(
            static_cast<const unsigned char*>(tex->content.data), (int)tex->content.size, out);
    const std::string fn = toStr(tex->filename);
    if (!fn.empty()) {
        int idx = material_load_texture_file(fn, out);
        if (idx >= 0) return idx;
    }
    const std::string rel = toStr(tex->relative_filename);
    if (!rel.empty())
        return material_load_texture_file((dir / rel).string(), out);
    return -1;
}

ModelVertex makeVertex(const ufbx_mesh* mesh, uint32_t ci,
                       const ufbx_matrix& world, const ufbx_matrix& nrmMat) {
    ModelVertex mv{};
    const ufbx_vec3 p  = ufbx_get_vertex_vec3(&mesh->vertex_position, ci);
    const ufbx_vec3 pw = ufbx_transform_position(&world, p);
    mv.pos[0] = (float)pw.x; mv.pos[1] = (float)pw.y; mv.pos[2] = (float)pw.z;

    if (mesh->vertex_normal.exists) {
        const ufbx_vec3 n  = ufbx_get_vertex_vec3(&mesh->vertex_normal, ci);
        ufbx_vec3 nw = ufbx_transform_direction(&nrmMat, n);
        const double l = std::sqrt(nw.x*nw.x + nw.y*nw.y + nw.z*nw.z);
        if (l > 1e-12) { nw.x /= l; nw.y /= l; nw.z /= l; }
        mv.normal[0] = (float)nw.x; mv.normal[1] = (float)nw.y; mv.normal[2] = (float)nw.z;
    } else {
        mv.normal[1] = 1.0f;
    }
    if (mesh->vertex_uv.exists) {
        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ci);
        mv.uv[0] = (float)uv.x;
        mv.uv[1] = 1.0f - (float)uv.y;  // FBX V is bottom-up → flip to top-down
    }
    return mv;
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

bool model_load_fbx(const char* path, ModelData& out) {
    if (!path) return false;
    const std::filesystem::path modelDir = std::filesystem::path(path).parent_path();

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;  // match glTF
    opts.target_unit_meters = 1.0f;                  // normalise cm/inch → metres
    opts.generate_missing_normals = true;

    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(path, &opts, &err);
    if (!scene) {
        std::fprintf(stderr, "[model_loader/fbx] '%s': %s\n", path, err.description.data);
        return false;
    }

    // Resolve a ufbx material → ModelData material index, caching the mapping.
    std::unordered_map<const ufbx_material*, int> matIndex;
    int defaultMat = -1;
    auto resolveMaterial = [&](const ufbx_material* m) -> int {
        if (!m) {
            if (defaultMat < 0) {
                ModelMaterial mm{};
                mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
                mm.baseColorFactor[3] = 1.0f;
                mm.metallic = 0.0f; mm.roughness = 0.8f;
                defaultMat = (int)out.materials.size();
                out.materials.push_back(mm);
            }
            return defaultMat;
        }
        auto it = matIndex.find(m);
        if (it != matIndex.end()) return it->second;

        ModelMaterial mm{};
        // Base color: PBR first, else legacy diffuse.
        if (m->pbr.base_color.has_value) {
            mm.baseColorFactor[0] = (float)m->pbr.base_color.value_vec4.x;
            mm.baseColorFactor[1] = (float)m->pbr.base_color.value_vec4.y;
            mm.baseColorFactor[2] = (float)m->pbr.base_color.value_vec4.z;
            mm.baseColorFactor[3] = (float)m->pbr.base_color.value_vec4.w;
        } else if (m->fbx.diffuse_color.has_value) {
            mm.baseColorFactor[0] = (float)m->fbx.diffuse_color.value_vec4.x;
            mm.baseColorFactor[1] = (float)m->fbx.diffuse_color.value_vec4.y;
            mm.baseColorFactor[2] = (float)m->fbx.diffuse_color.value_vec4.z;
            mm.baseColorFactor[3] = 1.0f;
        }
        mm.metallic  = m->pbr.metalness.has_value ? (float)m->pbr.metalness.value_real : 0.0f;
        mm.roughness = m->pbr.roughness.has_value
                         ? (float)m->pbr.roughness.value_real
                         : material_shininess_to_roughness((float)m->fbx.specular_exponent.value_real);
        // Emission: PBR color×factor, else legacy.
        if (m->pbr.emission_color.has_value) {
            const float f = m->pbr.emission_factor.has_value ? (float)m->pbr.emission_factor.value_real : 1.0f;
            mm.emissive[0] = (float)m->pbr.emission_color.value_vec4.x * f;
            mm.emissive[1] = (float)m->pbr.emission_color.value_vec4.y * f;
            mm.emissive[2] = (float)m->pbr.emission_color.value_vec4.z * f;
        } else if (m->fbx.emission_color.has_value) {
            mm.emissive[0] = (float)m->fbx.emission_color.value_vec4.x;
            mm.emissive[1] = (float)m->fbx.emission_color.value_vec4.y;
            mm.emissive[2] = (float)m->fbx.emission_color.value_vec4.z;
        }
        // Textures (no clean combined metallic-roughness in FBX → factors only).
        const ufbx_texture* baseTex = m->pbr.base_color.texture ? m->pbr.base_color.texture
                                                                : m->fbx.diffuse_color.texture;
        const ufbx_texture* nrmTex  = m->pbr.normal_map.texture ? m->pbr.normal_map.texture
                                                                : m->fbx.normal_map.texture;
        mm.baseColorTex = loadUfbxTexture(baseTex, modelDir, out);
        mm.normalTex    = loadUfbxTexture(nrmTex, modelDir, out);
        mm.emissiveTex  = loadUfbxTexture(m->pbr.emission_color.texture, modelDir, out);

        const int idx = (int)out.materials.size();
        out.materials.push_back(mm);
        matIndex.emplace(m, idx);
        return idx;
    };

    // Walk mesh instances; bake node world transform; split by material part.
    std::vector<uint32_t> tri;
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];
        tri.resize(mesh->max_face_triangles * 3);

        for (size_t ni = 0; ni < mesh->instances.count; ++ni) {
            const ufbx_node* node = mesh->instances.data[ni];
            const ufbx_matrix world  = node->geometry_to_world;
            const ufbx_matrix nrmMat = ufbx_matrix_for_normals(&world);

            for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
                const ufbx_mesh_part& part = mesh->material_parts.data[pi];
                if (part.num_triangles == 0) continue;

                const ufbx_material* mat =
                    (part.index < node->materials.count) ? node->materials.data[part.index] : nullptr;

                ModelPrimitive mp{};
                mp.firstVertex = (uint32_t)out.vertices.size();
                mp.firstIndex  = (uint32_t)out.indices.size();
                mp.material = resolveMaterial(mat);
                mp.node = -1; mp.skin = -1; mp.morph = -1;
                std::memset(mp.modelMatrix, 0, sizeof(mp.modelMatrix));
                mp.modelMatrix[0] = mp.modelMatrix[5] = mp.modelMatrix[10] = mp.modelMatrix[15] = 1.0f;

                for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                    const ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                    const uint32_t nTri = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                    for (uint32_t t = 0; t < nTri * 3; ++t) {
                        const ModelVertex v = makeVertex(mesh, tri[t], world, nrmMat);
                        const uint32_t gi = (uint32_t)out.vertices.size();
                        out.vertices.push_back(v);
                        out.indices.push_back(gi);
                        accumulateBBox(out, v.pos);
                    }
                }
                mp.vertexCount = (uint32_t)out.vertices.size() - mp.firstVertex;
                mp.indexCount  = (uint32_t)out.indices.size()  - mp.firstIndex;
                if (mp.indexCount > 0) out.primitives.push_back(mp);
            }
        }
    }

    const size_t meshCount = scene->meshes.count;
    ufbx_free_scene(scene);

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader/fbx] '%s': no drawable triangle geometry\n", path);
        return false;
    }
    std::fprintf(stderr,
        "[model_loader/fbx] '%s': %zu meshes, %u prims, %zu verts, %zu materials, %zu textures\n",
        path, meshCount, out.primitiveCount, out.vertices.size(),
        out.materials.size(), out.textures.size());
    return true;
}

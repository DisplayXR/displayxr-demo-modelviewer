// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  glTF 2.0 loader implementation (tinygltf).
 *
 * Single translation unit that defines the tinygltf implementation. Built with
 * NO stb coupling (see model_loader.h for why): texture image pixels are not
 * decoded in v1 — only material factors are read.
 */

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>

#include "model_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

// Compose a node's local transform: explicit matrix if present, else T*R*S.
glm::mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        return glm::make_mat4(node.matrix.data());  // column-major in glTF too
    }
    glm::mat4 m(1.0f);
    if (node.translation.size() == 3) {
        m = glm::translate(m, glm::vec3((float)node.translation[0],
                                        (float)node.translation[1],
                                        (float)node.translation[2]));
    }
    if (node.rotation.size() == 4) {
        // glTF quaternion is (x, y, z, w); glm::quat ctor is (w, x, y, z).
        glm::quat q((float)node.rotation[3], (float)node.rotation[0],
                    (float)node.rotation[1], (float)node.rotation[2]);
        m = m * glm::mat4_cast(q);
    }
    if (node.scale.size() == 3) {
        m = glm::scale(m, glm::vec3((float)node.scale[0],
                                    (float)node.scale[1],
                                    (float)node.scale[2]));
    }
    return m;
}

// Read accessor element `i`, component `c` as float (handles the float and
// normalized-integer position/normal/uv cases we care about).
const unsigned char* accessorPtr(const tinygltf::Model& m,
                                 const tinygltf::Accessor& acc,
                                 size_t& strideOut) {
    const tinygltf::BufferView& bv = m.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = m.buffers[bv.buffer];
    const int compCount = tinygltf::GetNumComponentsInType(acc.type);
    const int compBytes = tinygltf::GetComponentSizeInBytes(acc.componentType);
    strideOut = bv.byteStride ? bv.byteStride : (size_t)(compCount * compBytes);
    return buf.data.data() + bv.byteOffset + acc.byteOffset;
}

void readVec(const tinygltf::Model& m, int accessorIdx, int comps,
             std::vector<float>& out) {
    out.clear();
    if (accessorIdx < 0) return;
    const tinygltf::Accessor& acc = m.accessors[accessorIdx];
    size_t stride = 0;
    const unsigned char* base = accessorPtr(m, acc, stride);
    out.resize(acc.count * comps);
    for (size_t i = 0; i < acc.count; ++i) {
        const float* f = reinterpret_cast<const float*>(base + i * stride);
        for (int c = 0; c < comps; ++c) out[i * comps + c] = f[c];
    }
}

uint32_t readIndex(const tinygltf::Model& m, const tinygltf::Accessor& acc,
                   const unsigned char* base, size_t stride, size_t i) {
    const unsigned char* p = base + i * stride;
    switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return *p;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
        default: return 0;
    }
}

void processNode(const tinygltf::Model& m, int nodeIdx, const glm::mat4& parent,
                 ModelData& out) {
    const tinygltf::Node& node = m.nodes[nodeIdx];
    const glm::mat4 world = parent * nodeLocalMatrix(node);

    if (node.mesh >= 0 && node.mesh < (int)m.meshes.size()) {
        const tinygltf::Mesh& mesh = m.meshes[node.mesh];
        for (const auto& prim : mesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;

            auto itPos = prim.attributes.find("POSITION");
            if (itPos == prim.attributes.end()) continue;
            auto itNrm = prim.attributes.find("NORMAL");
            auto itUv  = prim.attributes.find("TEXCOORD_0");

            std::vector<float> pos, nrm, uv;
            readVec(m, itPos->second, 3, pos);
            readVec(m, itNrm != prim.attributes.end() ? itNrm->second : -1, 3, nrm);
            readVec(m, itUv  != prim.attributes.end() ? itUv->second  : -1, 2, uv);

            const size_t vcount = pos.size() / 3;
            const uint32_t vertexBase = (uint32_t)out.vertices.size();

            for (size_t i = 0; i < vcount; ++i) {
                ModelVertex v{};
                v.pos[0] = pos[i * 3 + 0]; v.pos[1] = pos[i * 3 + 1]; v.pos[2] = pos[i * 3 + 2];
                if (!nrm.empty()) { v.normal[0] = nrm[i*3+0]; v.normal[1] = nrm[i*3+1]; v.normal[2] = nrm[i*3+2]; }
                else              { v.normal[0] = 0; v.normal[1] = 1; v.normal[2] = 0; }
                if (!uv.empty())  { v.uv[0] = uv[i*2+0]; v.uv[1] = uv[i*2+1]; }
                out.vertices.push_back(v);

                // AABB in world space.
                glm::vec4 wp = world * glm::vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f);
                if (!out.hasBBox) {
                    out.bboxMin[0] = out.bboxMax[0] = wp.x;
                    out.bboxMin[1] = out.bboxMax[1] = wp.y;
                    out.bboxMin[2] = out.bboxMax[2] = wp.z;
                    out.hasBBox = true;
                } else {
                    out.bboxMin[0] = std::min(out.bboxMin[0], wp.x); out.bboxMax[0] = std::max(out.bboxMax[0], wp.x);
                    out.bboxMin[1] = std::min(out.bboxMin[1], wp.y); out.bboxMax[1] = std::max(out.bboxMax[1], wp.y);
                    out.bboxMin[2] = std::min(out.bboxMin[2], wp.z); out.bboxMax[2] = std::max(out.bboxMax[2], wp.z);
                }
            }

            ModelPrimitive mp{};
            mp.firstIndex = (uint32_t)out.indices.size();
            mp.material = prim.material;
            std::memcpy(mp.modelMatrix, glm::value_ptr(world), 16 * sizeof(float));

            if (prim.indices >= 0) {
                const tinygltf::Accessor& iacc = m.accessors[prim.indices];
                size_t istride = 0;
                const unsigned char* ibase = accessorPtr(m, iacc, istride);
                if (istride == 0) istride = tinygltf::GetComponentSizeInBytes(iacc.componentType);
                for (size_t i = 0; i < iacc.count; ++i)
                    out.indices.push_back(vertexBase + readIndex(m, iacc, ibase, istride, i));
                mp.indexCount = (uint32_t)iacc.count;
            } else {
                // Non-indexed: emit a sequential index range.
                for (size_t i = 0; i < vcount; ++i)
                    out.indices.push_back(vertexBase + (uint32_t)i);
                mp.indexCount = (uint32_t)vcount;
            }

            if (mp.indexCount > 0) out.primitives.push_back(mp);
        }
    }

    for (int child : node.children) processNode(m, child, world, out);
}

}  // namespace

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

    // Materials (factors only in v1).
    out.materials.reserve(model.materials.size());
    for (const auto& mat : model.materials) {
        ModelMaterial mm{};
        const auto& pbr = mat.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4)
            for (int i = 0; i < 4; ++i) mm.baseColorFactor[i] = (float)pbr.baseColorFactor[i];
        mm.metallic = (float)pbr.metallicFactor;
        mm.roughness = (float)pbr.roughnessFactor;
        if (mat.emissiveFactor.size() == 3)
            for (int i = 0; i < 3; ++i) mm.emissive[i] = (float)mat.emissiveFactor[i];
        mm.baseColorTexture = pbr.baseColorTexture.index;  // recorded, unused in v1
        out.materials.push_back(mm);
    }

    // Walk the default scene (or scene 0).
    const int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
    const glm::mat4 ident(1.0f);
    if (sceneIdx >= 0 && sceneIdx < (int)model.scenes.size()) {
        for (int n : model.scenes[sceneIdx].nodes) processNode(model, n, ident, out);
    } else {
        for (int n = 0; n < (int)model.nodes.size(); ++n) processNode(model, n, ident, out);
    }

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader] '%s' has no drawable triangle geometry\n", gltfPath);
        return false;
    }

    std::fprintf(stderr,
        "[model_loader] '%s': %u prims, %zu verts, %zu indices, %zu materials\n",
        gltfPath, out.primitiveCount, out.vertices.size(), out.indices.size(),
        out.materials.size());
    return true;
}

bool model_validate_file(const std::string& path) {
    if (path.empty()) return false;
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".glb" && ext != ".gltf") return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

std::string model_basename(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

std::string model_filesize_str(const std::string& path) {
    try {
        auto size = std::filesystem::file_size(path);
        char buf[32];
        if (size >= 1024ull * 1024 * 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024 * 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f MB", (double)size / (1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f KB", (double)size / 1024.0);
            return buf;
        }
        return std::to_string(size) + " B";
    } catch (...) {
        return "unknown";
    }
}

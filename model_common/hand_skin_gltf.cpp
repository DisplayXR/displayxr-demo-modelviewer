// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The rigged hand's glTF reader (tinygltf; the implementation is
 *         compiled once, in model_loader_gltf.cpp). See hand_skin.h.
 */

#include "hand_skin.h"

// Same configuration as model_loader_gltf.cpp (which compiles the
// implementation): the header's TinyGLTF constructor depends on it.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <cmath>
#include <cstring>

namespace modelviewer {

namespace {

//! Read accessor element `i` as up to 4 floats (normalised integers -> [0,1],
//! plain integers as their value).
bool readElem(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i, float* out, int comps) {
    if (a.bufferView < 0 || a.bufferView >= (int)m.bufferViews.size()) return false;
    const tinygltf::BufferView& bv = m.bufferViews[a.bufferView];
    if (bv.buffer < 0 || bv.buffer >= (int)m.buffers.size()) return false;
    const tinygltf::Buffer& buf = m.buffers[bv.buffer];
    const int compSize = tinygltf::GetComponentSizeInBytes((uint32_t)a.componentType);
    const int n = tinygltf::GetNumComponentsInType((uint32_t)a.type);
    if (compSize <= 0 || n <= 0 || n < comps) return false;
    const size_t stride = bv.byteStride ? bv.byteStride : (size_t)compSize * (size_t)n;
    const size_t off = bv.byteOffset + a.byteOffset + i * stride;
    if (off + (size_t)compSize * (size_t)n > buf.data.size()) return false;
    const unsigned char* p = buf.data.data() + off;
    for (int c = 0; c < comps; ++c) {
        const unsigned char* q = p + (size_t)c * (size_t)compSize;
        switch (a.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: { float f; std::memcpy(&f, q, 4); out[c] = f; break; }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: out[c] = a.normalized ? (float)q[0] / 255.f : (float)q[0]; break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t u; std::memcpy(&u, q, 2);
            out[c] = a.normalized ? (float)u / 65535.f : (float)u;
            break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t u; std::memcpy(&u, q, 4); out[c] = (float)u; break; }
        default: return false;
        }
    }
    return true;
}

} // namespace

bool HandSkinLoadGlb(const uint8_t* data, size_t size, HandSkinMesh& out, std::string* err) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model m;
    std::string e, w;
    if (!loader.LoadBinaryFromMemory(&m, &e, &w, data, (unsigned int)size, "")) {
        if (err) *err = "glb parse: " + e;
        return false;
    }
    if (m.skins.size() != 1 || m.meshes.empty()) {
        if (err) *err = "expected one skin and a mesh";
        return false;
    }
    const tinygltf::Skin& skin = m.skins[0];
    const size_t bones = skin.joints.size();
    if (bones == 0 || bones > (size_t)kHandSkinMaxBones || skin.inverseBindMatrices < 0) {
        if (err) *err = "skin has no joints / inverse bind matrices";
        return false;
    }
    HandSkinMesh h;
    std::vector<float> heads(bones * 3);
    const tinygltf::Accessor& ibmA = m.accessors[skin.inverseBindMatrices];
    for (size_t b = 0; b < bones; ++b) {
        h.boneNames.push_back(m.nodes[skin.joints[b]].name);
        float M[16];
        for (int k = 0; k < 16; ++k) M[k] = 0.f;
        // MAT4 = 16 floats column-major; readElem caps at 4, so read raw.
        {
            const tinygltf::BufferView& bv = m.bufferViews[ibmA.bufferView];
            const tinygltf::Buffer& buf = m.buffers[bv.buffer];
            const size_t stride = bv.byteStride ? bv.byteStride : 64;
            const size_t off = bv.byteOffset + ibmA.byteOffset + b * stride;
            if (ibmA.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || off + 64 > buf.data.size()) {
                if (err) *err = "bad inverse bind matrix accessor";
                return false;
            }
            std::memcpy(M, buf.data.data() + off, 64);
        }
        // Bone head = translation of inverse(IBM): -A^{-1} t for the affine IBM.
        const float a = M[0], b1 = M[4], c = M[8];
        const float d = M[1], e1 = M[5], f = M[9];
        const float g = M[2], hh = M[6], i = M[10];
        const float det = a * (e1 * i - f * hh) - b1 * (d * i - f * g) + c * (d * hh - e1 * g);
        if (std::fabs(det) < 1.0e-12f) {
            if (err) *err = "singular inverse bind matrix";
            return false;
        }
        const float inv[9] = {
            (e1 * i - f * hh) / det, -(b1 * i - c * hh) / det, (b1 * f - c * e1) / det,
            -(d * i - f * g) / det, (a * i - c * g) / det, -(a * f - c * d) / det,
            (d * hh - e1 * g) / det, -(a * hh - b1 * g) / det, (a * e1 - b1 * d) / det,
        };   // row-major inverse of the row-major [a b c; d e f; g h i]
        const float t[3] = {M[12], M[13], M[14]};
        for (int r = 0; r < 3; ++r)
            heads[b * 3 + r] = -(inv[r * 3 + 0] * t[0] + inv[r * 3 + 1] * t[1] + inv[r * 3 + 2] * t[2]);
    }

    for (const tinygltf::Mesh& mesh : m.meshes) {
        for (const tinygltf::Primitive& prim : mesh.primitives) {
            if (prim.mode != -1 && prim.mode != TINYGLTF_MODE_TRIANGLES) continue;
            auto attr = [&](const char* name) -> const tinygltf::Accessor* {
                auto it = prim.attributes.find(name);
                return it == prim.attributes.end() ? nullptr : &m.accessors[it->second];
            };
            const tinygltf::Accessor* pa = attr("POSITION");
            const tinygltf::Accessor* na = attr("NORMAL");
            const tinygltf::Accessor* ja = attr("JOINTS_0");
            const tinygltf::Accessor* wa = attr("WEIGHTS_0");
            const tinygltf::Accessor* ca = attr("COLOR_0");
            if (!pa || !na || !ja || !wa || prim.indices < 0) {
                if (err) *err = "primitive lacks POSITION/NORMAL/JOINTS_0/WEIGHTS_0/indices";
                return false;
            }
            const size_t base = h.vertexCount();
            const size_t nv = pa->count;
            if (base + nv > 65535) {
                if (err) *err = "too many vertices for 16-bit indices";
                return false;
            }
            for (size_t v = 0; v < nv; ++v) {
                float p[4], n[4], jj[4], ww[4], col[4] = {1.f, 1.f, 1.f, 1.f};
                if (!readElem(m, *pa, v, p, 3) || !readElem(m, *na, v, n, 3) || !readElem(m, *ja, v, jj, 4) ||
                    !readElem(m, *wa, v, ww, 4)) {
                    if (err) *err = "vertex attribute read failed";
                    return false;
                }
                if (ca) {
                    const int cc = tinygltf::GetNumComponentsInType((uint32_t)ca->type);
                    if (!readElem(m, *ca, v, col, cc >= 4 ? 4 : 3)) col[3] = 1.f;
                    if (cc < 4) col[3] = 1.f;
                }
                h.pos.insert(h.pos.end(), p, p + 3);
                h.nrm.insert(h.nrm.end(), n, n + 3);
                h.alpha.push_back(col[3]);
                float wsum = ww[0] + ww[1] + ww[2] + ww[3];
                if (!(wsum > 0.f)) { ww[0] = 1.f; ww[1] = ww[2] = ww[3] = 0.f; wsum = 1.f; }
                for (int k = 0; k < 4; ++k) {
                    h.joints.push_back((uint8_t)jj[k]);
                    h.weights.push_back(ww[k] / wsum);
                }
            }
            const tinygltf::Accessor& ia = m.accessors[prim.indices];
            for (size_t k = 0; k < ia.count; ++k) {
                float f;
                if (!readElem(m, ia, k, &f, 1) || (size_t)f >= nv) {
                    if (err) *err = "index read failed";
                    return false;
                }
                h.indices.push_back((uint16_t)(base + (size_t)f));
            }
        }
    }
    if (h.indices.empty()) {
        if (err) *err = "no triangles";
        return false;
    }
    if (!HandSkinFinalize(h, heads, err)) return false;
    out = std::move(h);
    return true;
}

} // namespace modelviewer

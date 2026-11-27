// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit tests for integer-stored and sparse vertex attributes (#184).
 *
 * The glTF backend keeps a primitive only when gltf_check::decodableAttribute()
 * accepts each float attribute, then reads it with dxr::gltf::ReadFloats (the
 * same call readVec() makes). These cases build small tinygltf documents by
 * hand (no file, no parser): a u16 normalized TEXCOORD_0, BYTE / SHORT
 * normalized NORMAL and POSITION read through a byteStride and an accessor
 * byteOffset, non-normalized KHR_mesh_quantization data, a sparse morph delta
 * with no bufferView, and the accessors that must still be refused. No
 * framework: one console exe, exit 0 = every case passed.
 */

#include "gltf_accessor_check.h"

#define TINYGLTF_NO_STB_IMAGE  // declarations only; no tinygltf implementation needed
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>
#include "gltf_accessor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void
Check(bool cond, const std::string& what)
{
    ++g_checks;
    if (cond) {
        std::printf("  ok    %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

constexpr int kF = TINYGLTF_COMPONENT_TYPE_FLOAT;
constexpr int kB = TINYGLTF_COMPONENT_TYPE_BYTE;
constexpr int kUB = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
constexpr int kS = TINYGLTF_COMPONENT_TYPE_SHORT;
constexpr int kUS = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
constexpr int kUI = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;

// The componentType sets primitiveProblem() passes (glTF core +
// KHR_mesh_quantization).
const std::initializer_list<int> kPosTypes = {kF, kB, kUB, kS, kUS};
const std::initializer_list<int> kUvTypes = {kF, kB, kUB, kS, kUS};
const std::initializer_list<int> kNrmTypes = {kF, kB, kS};

// A document whose buffer 0 is built up by Put(); each Put() returns the byte
// offset it wrote at. View() adds a bufferView over [offset, offset+length).
struct Doc {
    tinygltf::Model m;

    Doc() { m.buffers.resize(1); }

    template <class T>
    size_t
    Put(const std::vector<T>& v)
    {
        auto& d = m.buffers[0].data;
        while (d.size() % 4) d.push_back(0);
        const size_t off = d.size();
        d.resize(off + v.size() * sizeof(T));
        std::memcpy(d.data() + off, v.data(), v.size() * sizeof(T));
        return off;
    }

    int
    View(size_t offset, size_t length, size_t stride = 0)
    {
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = offset;
        bv.byteLength = length;
        bv.byteStride = stride;
        m.bufferViews.push_back(bv);
        return (int)m.bufferViews.size() - 1;
    }

    int
    Accessor(int type, size_t count, int compType, bool normalized, int bufferView,
             size_t byteOffset = 0)
    {
        tinygltf::Accessor a;
        a.type = type;
        a.componentType = compType;
        a.normalized = normalized;
        a.count = count;
        a.bufferView = bufferView;
        a.byteOffset = byteOffset;
        m.accessors.push_back(a);
        return (int)m.accessors.size() - 1;
    }
};

bool
Near(float a, float b)
{
    return std::fabs(a - b) < 1e-6f;
}

// decodableAttribute() verdict, and (when accepted) ReadFloats == want.
void
Expect(const Doc& d, int acc, int type, std::initializer_list<int> types, bool intNormalized,
       bool wantOk, const std::vector<float>& want, const char* what)
{
    const bool ok =
        gltf_check::decodableAttribute(d.m, acc, type, types, intNormalized) != nullptr;
    std::string line = std::string(wantOk ? "accepted: " : "refused:  ") + what;
    if (ok != wantOk) {
        Check(false, line + "   [verdict was " + (ok ? "accepted" : "refused") + "]");
        return;
    }
    if (!ok) {
        Check(true, line);
        return;
    }
    std::vector<float> got;
    std::string err;
    const int comps = tinygltf::GetNumComponentsInType((uint32_t)type);
    if (!dxr::gltf::ReadFloats(d.m, acc, comps, got, &err)) {
        Check(false, line + "   [ReadFloats failed: " + err + "]");
        return;
    }
    bool same = got.size() == want.size();
    for (size_t i = 0; same && i < got.size(); ++i) same = Near(got[i], want[i]);
    if (!same) {
        std::string vals;
        for (float f : got) vals += " " + std::to_string(f);
        Check(false, line + "   [read:" + vals + "]");
        return;
    }
    Check(true, line);
}

} // namespace

int
main()
{
    std::printf("glTF integer / sparse vertex attribute decode\n");

    // ── u16 normalized TEXCOORD_0 (the issue's fixture) ──────────────────
    {
        Doc d;
        const size_t off = d.Put(std::vector<uint16_t>{0, 65535, 32768, 16384, 65535, 0});
        const int bv = d.View(off, 12);
        const int uv = d.Accessor(TINYGLTF_TYPE_VEC2, 3, kUS, true, bv);
        Expect(d, uv, TINYGLTF_TYPE_VEC2, kUvTypes, false, true,
               {0.0f, 1.0f, 32768 / 65535.0f, 16384 / 65535.0f, 1.0f, 0.0f},
               "TEXCOORD_0 VEC2 USHORT normalized -> c/65535");
    }
    {
        Doc d;
        const size_t off = d.Put(std::vector<uint8_t>{0, 255, 51, 204});
        const int bv = d.View(off, 4);
        const int uv = d.Accessor(TINYGLTF_TYPE_VEC2, 2, kUB, true, bv);
        Expect(d, uv, TINYGLTF_TYPE_VEC2, kUvTypes, false, true, {0.0f, 1.0f, 0.2f, 0.8f},
               "TEXCOORD_0 VEC2 UBYTE normalized -> c/255");
    }
    {
        // KHR_mesh_quantization: a non-normalized integer is cast as-is (the
        // texture transform / node scale is what maps it into range).
        Doc d;
        const size_t off = d.Put(std::vector<uint16_t>{0, 1, 1000, 65535});
        const int bv = d.View(off, 8);
        const int uv = d.Accessor(TINYGLTF_TYPE_VEC2, 2, kUS, false, bv);
        Expect(d, uv, TINYGLTF_TYPE_VEC2, kUvTypes, false, true, {0.0f, 1.0f, 1000.0f, 65535.0f},
               "TEXCOORD_0 VEC2 USHORT not normalized -> cast as-is");
    }

    // ── BYTE normalized NORMAL through a padded byteStride ───────────────
    {
        // VEC3 BYTE is 3 bytes; glTF aligns vertex attributes to 4, so the
        // view's byteStride is 4 and the 4th byte of each element is padding
        // that must not be read.
        Doc d;
        const size_t off = d.Put(std::vector<int8_t>{127, 0, -127, 99,   //
                                                    -128, 64, 0, 99,     //
                                                    0, 0, 127, 99});
        const int bv = d.View(off, 12, 4);
        const int n = d.Accessor(TINYGLTF_TYPE_VEC3, 3, kB, true, bv);
        Expect(d, n, TINYGLTF_TYPE_VEC3, kNrmTypes, true, true,
               {1.0f, 0.0f, -1.0f, -1.0f, 64 / 127.0f, 0.0f, 0.0f, 0.0f, 1.0f},
               "NORMAL VEC3 BYTE normalized, byteStride 4 -> max(c/127,-1)");
    }
    {
        Doc d;
        const size_t off = d.Put(std::vector<int8_t>{127, 0, 0, 0});
        const int bv = d.View(off, 4, 4);
        const int n = d.Accessor(TINYGLTF_TYPE_VEC3, 1, kB, false, bv);
        Expect(d, n, TINYGLTF_TYPE_VEC3, kNrmTypes, true, false, {},
               "NORMAL VEC3 BYTE not normalized (a unit vector must be)");
    }
    {
        Doc d;
        const size_t off = d.Put(std::vector<uint8_t>{255, 0, 0, 0});
        const int bv = d.View(off, 4, 4);
        const int n = d.Accessor(TINYGLTF_TYPE_VEC3, 1, kUB, true, bv);
        Expect(d, n, TINYGLTF_TYPE_VEC3, kNrmTypes, true, false, {},
               "NORMAL VEC3 UNSIGNED_BYTE (not a NORMAL type)");
    }

    // ── SHORT POSITION, interleaved, with an accessor byteOffset ─────────
    {
        // Two interleaved attributes in one 8-byte stride: SHORT xyz + pad at
        // +0, and this accessor starts one element in (byteOffset 8).
        Doc d;
        const size_t off = d.Put(std::vector<int16_t>{1, 2, 3, 0,             //
                                                     32767, -32768, 0, 0,     //
                                                     -16384, 100, -1, 0});
        const int bv = d.View(off, 24, 8);
        const int p = d.Accessor(TINYGLTF_TYPE_VEC3, 2, kS, true, bv, 8);
        Expect(d, p, TINYGLTF_TYPE_VEC3, kPosTypes, false, true,
               {1.0f, -1.0f, 0.0f, -16384 / 32767.0f, 100 / 32767.0f, -1 / 32767.0f},
               "POSITION VEC3 SHORT normalized, byteStride 8, byteOffset 8 -> max(c/32767,-1)");
        const int q = d.Accessor(TINYGLTF_TYPE_VEC3, 3, kS, false, bv);
        Expect(d, q, TINYGLTF_TYPE_VEC3, kPosTypes, false, true,
               {1, 2, 3, 32767, -32768, 0, -16384, 100, -1},
               "POSITION VEC3 SHORT not normalized -> cast as-is");
    }
    {
        Doc d;
        const size_t off = d.Put(std::vector<float>{1.5f, -2.0f, 3.25f, 0.0f, 0.5f, -0.5f});
        const int bv = d.View(off, 24);
        const int p = d.Accessor(TINYGLTF_TYPE_VEC3, 2, kF, false, bv);
        Expect(d, p, TINYGLTF_TYPE_VEC3, kPosTypes, false, true, {1.5f, -2.0f, 3.25f, 0.0f, 0.5f, -0.5f},
               "POSITION VEC3 FLOAT (control) -> unchanged");
        const int u = d.Accessor(TINYGLTF_TYPE_VEC3, 2, kUI, false, bv);
        Expect(d, u, TINYGLTF_TYPE_VEC3, kPosTypes, false, false, {},
               "POSITION VEC3 UNSIGNED_INT (not an attribute type)");
        const int w = d.Accessor(TINYGLTF_TYPE_VEC2, 2, kF, false, bv);
        Expect(d, w, TINYGLTF_TYPE_VEC3, kPosTypes, false, false, {}, "POSITION declared VEC2");
    }

    // ── Bounds ───────────────────────────────────────────────────────────
    {
        Doc d;
        const size_t off = d.Put(std::vector<uint16_t>{1, 2, 3, 4, 5, 6});
        const int bv = d.View(off, 12);
        const int uv = d.Accessor(TINYGLTF_TYPE_VEC2, 4, kUS, true, bv);
        Expect(d, uv, TINYGLTF_TYPE_VEC2, kUvTypes, false, false, {},
               "TEXCOORD_0 4 elements in a 3-element bufferView");
        const int uv2 = d.Accessor(TINYGLTF_TYPE_VEC2, 3, kUS, true, bv, 2);
        Expect(d, uv2, TINYGLTF_TYPE_VEC2, kUvTypes, false, false, {},
               "TEXCOORD_0 byteOffset 2 pushes the last element past the view");
        const int bvShort = d.View(off, 12, 2);
        const int uv3 = d.Accessor(TINYGLTF_TYPE_VEC2, 2, kUS, true, bvShort);
        Expect(d, uv3, TINYGLTF_TYPE_VEC2, kUvTypes, false, false, {},
               "TEXCOORD_0 byteStride 2 smaller than the 4-byte element");
        const int bvPast = d.View(off, 64);
        const int uv4 = d.Accessor(TINYGLTF_TYPE_VEC2, 1, kUS, true, bvPast);
        Expect(d, uv4, TINYGLTF_TYPE_VEC2, kUvTypes, false, false, {},
               "TEXCOORD_0 in a bufferView that overruns the buffer");
    }

    // ── Sparse morph-target delta ────────────────────────────────────────
    {
        // 4 vertices, no bufferView (all zeros), 2 sparse overrides of
        // SHORT normalized deltas at vertices 3 and 1.
        Doc d;
        const size_t ioff = d.Put(std::vector<uint8_t>{3, 1});
        const size_t voff = d.Put(std::vector<int16_t>{32767, 0, -32767, 0, 16384, 0});
        const int ibv = d.View(ioff, 2);
        const int vbv = d.View(voff, 12);
        const int dp = d.Accessor(TINYGLTF_TYPE_VEC3, 4, kS, true, -1);
        auto& a = d.m.accessors[(size_t)dp];
        a.sparse.isSparse = true;
        a.sparse.count = 2;
        a.sparse.indices.bufferView = ibv;
        a.sparse.indices.byteOffset = 0;
        a.sparse.indices.componentType = kUB;
        a.sparse.values.bufferView = vbv;
        a.sparse.values.byteOffset = 0;
        Expect(d, dp, TINYGLTF_TYPE_VEC3, kNrmTypes, false, true,
               {0, 0, 0, 0, 16384 / 32767.0f, 0, 0, 0, 0, 1.0f, 0, -1.0f},
               "morph POSITION, sparse over no bufferView -> zeros + overrides");

        // Same, but one sparse index names vertex 4 of 4.
        Doc e = d;
        e.m.buffers[0].data[ioff] = 4;
        Expect(e, dp, TINYGLTF_TYPE_VEC3, kNrmTypes, false, false, {},
               "morph POSITION, sparse index 4 >= count 4");

        // Sparse on top of a FLOAT base.
        Doc f;
        const size_t boff = f.Put(std::vector<float>{1, 1, 1, 2, 2, 2, 3, 3, 3});
        const size_t sio = f.Put(std::vector<uint16_t>{1});
        const size_t svo = f.Put(std::vector<float>{-5, -6, -7});
        const int bbv = f.View(boff, 36);
        const int sibv = f.View(sio, 2);
        const int svbv = f.View(svo, 12);
        const int fp = f.Accessor(TINYGLTF_TYPE_VEC3, 3, kF, false, bbv);
        auto& fa = f.m.accessors[(size_t)fp];
        fa.sparse.isSparse = true;
        fa.sparse.count = 1;
        fa.sparse.indices.bufferView = sibv;
        fa.sparse.indices.byteOffset = 0;  // tinygltf leaves these uninitialised
        fa.sparse.indices.componentType = kUS;
        fa.sparse.values.bufferView = svbv;
        fa.sparse.values.byteOffset = 0;
        Expect(f, fp, TINYGLTF_TYPE_VEC3, kPosTypes, false, true, {1, 1, 1, -5, -6, -7, 3, 3, 3},
               "POSITION FLOAT with a sparse override of vertex 1");
    }

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

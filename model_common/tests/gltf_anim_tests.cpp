// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit tests for gltf_accessor_check.h — the animation-channel checks.
 *
 * The glTF backend reads a channel's sampler accessors by pointer arithmetic
 * and the renderer indexes the output by key every frame, so a channel is only
 * kept when animationChannelProblem() returns nullptr. These cases build small
 * tinygltf documents by hand (no file, no parser) and check the verdict for
 * each way a channel can be unreadable, plus well-formed controls that must
 * pass. No framework: one console exe, exit 0 = every case passed.
 */

#include "gltf_accessor_check.h"

#define TINYGLTF_NO_STB_IMAGE  // declarations only; no tinygltf implementation needed
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <cstdio>
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

// One 4 KiB buffer behind one bufferView, three nodes: 0 plain, 1 with two
// morph weights, 2 plain. Accessors are added per case.
struct Doc {
    tinygltf::Model m;
    std::vector<size_t> morph;

    Doc()
    {
        tinygltf::Buffer buf;
        buf.data.assign(4096, 0);
        m.buffers.push_back(buf);
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = 0;
        bv.byteLength = 4096;
        m.bufferViews.push_back(bv);
        m.nodes.resize(3);
        morph = {0, 2, 0};
    }

    int
    Accessor(int type, size_t count, int compType = kF, int bufferView = 0, size_t byteOffset = 0)
    {
        tinygltf::Accessor a;
        a.type = type;
        a.componentType = compType;
        a.count = count;
        a.bufferView = bufferView;
        a.byteOffset = byteOffset;
        m.accessors.push_back(a);
        return (int)m.accessors.size() - 1;
    }
};

// Animation with one sampler (input -> output) and one channel on `node`.
tinygltf::Animation
OneChannel(int input, int output, int node, const char* path, const char* interp = "LINEAR",
           int sampler = 0)
{
    tinygltf::Animation a;
    tinygltf::AnimationSampler s;
    s.input = input;
    s.output = output;
    s.interpolation = interp;
    a.samplers.push_back(s);
    tinygltf::AnimationChannel c;
    c.sampler = sampler;
    c.target_node = node;
    c.target_path = path;
    a.channels.push_back(c);
    return a;
}

void
Expect(Doc& d, const tinygltf::Animation& a, bool wantOk, const char* what)
{
    const char* why = gltf_check::animationChannelProblem(d.m, a, a.channels[0], d.morph);
    std::string line = std::string(wantOk ? "accepted: " : "dropped:  ") + what;
    if (why) line += "   [" + std::string(why) + "]";
    Check(wantOk ? why == nullptr : why != nullptr, line);
}

} // namespace

int
main()
{
    std::printf("glTF animation channel checks\n");

    // ── Well-formed controls: must be kept ───────────────────────────────
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 0, "translation"), true, "LINEAR translation, 4 keys");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC4, 12);
        Expect(d, OneChannel(in, out, 0, "rotation", "CUBICSPLINE"), true,
               "CUBICSPLINE rotation, 4 keys x 3 elements");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 1);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 1);
        Expect(d, OneChannel(in, out, 2, "scale", "STEP"), true, "STEP scale, single key");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 3);
        const int out = d.Accessor(TINYGLTF_TYPE_SCALAR, 6);
        Expect(d, OneChannel(in, out, 1, "weights"), true, "weights, 3 keys x 2 morph targets");
    }

    // ── Sampler with no bufferView ───────────────────────────────────────
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4, kF, -1);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "input accessor has no bufferView");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4, kF, -1);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "output accessor has no bufferView");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        Expect(d, OneChannel(in, 99, 0, "translation"), false, "output accessor index out of range");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        // 4096-byte view: 342 VEC3 floats end at 4104 bytes.
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 342);
        Expect(d, OneChannel(in, out, 0, "translation"), false,
               "output accessor extends past its bufferView");
    }
    {
        Doc d;
        d.m.bufferViews[0].byteLength = 8192;  // view claims more than the buffer holds
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "bufferView extends past its buffer");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 0);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 0);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "input with no keyframes");
    }

    // ── Output shorter than the input needs ──────────────────────────────
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 8);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 7);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "LINEAR output 7 < input 8");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC4, 4);
        Expect(d, OneChannel(in, out, 0, "rotation", "CUBICSPLINE"), false,
               "CUBICSPLINE output 4 < 3 x input 4");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 3);
        const int out = d.Accessor(TINYGLTF_TYPE_SCALAR, 5);
        Expect(d, OneChannel(in, out, 1, "weights"), false, "weights output 5 < 3 keys x 2 targets");
    }

    // ── Wrong output type ────────────────────────────────────────────────
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC4, 4);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "translation output is VEC4");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 0, "rotation"), false, "rotation output is VEC3");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_VEC2, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "input is VEC2, not SCALAR");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 0, "translation"), false, "input is UNSIGNED_INT, not FLOAT");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC4, 4, TINYGLTF_COMPONENT_TYPE_SHORT);
        Expect(d, OneChannel(in, out, 0, "rotation"), false,
               "rotation output is normalized SHORT (not decoded)");
    }

    // ── Out-of-range indices ─────────────────────────────────────────────
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 4);
        const int out = d.Accessor(TINYGLTF_TYPE_VEC3, 4);
        Expect(d, OneChannel(in, out, 3, "translation"), false, "target node 3 of 3 nodes");
        Expect(d, OneChannel(in, out, -1, "translation"), false, "target node -1 (absent)");
        Expect(d, OneChannel(in, out, 0, "translation", "LINEAR", 1), false, "sampler 1 of 1 sampler");
        Expect(d, OneChannel(in, out, 0, "translation", "LINEAR", -1), false, "sampler -1");
    }
    {
        Doc d;
        const int in = d.Accessor(TINYGLTF_TYPE_SCALAR, 3);
        const int out = d.Accessor(TINYGLTF_TYPE_SCALAR, 3);
        Expect(d, OneChannel(in, out, 0, "weights"), false, "weights on a node with no morph targets");
    }

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

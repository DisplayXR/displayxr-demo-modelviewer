// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Range checks on a parsed glTF document (see gltf_accessor_check.h).
 */

#include "gltf_accessor_check.h"

// Same configuration as model_loader_gltf.cpp (which owns the tinygltf
// implementation), so every TU sees one definition of tinygltf's classes.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <algorithm>

namespace gltf_check {

const tinygltf::Accessor* checkedAccessor(const tinygltf::Model& m, int idx, int type,
                                          std::initializer_list<int> compTypes) {
    if (idx < 0 || idx >= (int)m.accessors.size()) return nullptr;
    const tinygltf::Accessor& acc = m.accessors[(size_t)idx];
    if (acc.type != type) return nullptr;
    if (std::find(compTypes.begin(), compTypes.end(), acc.componentType) == compTypes.end())
        return nullptr;
    // accessorPtr() dereferences the bufferView even for an empty accessor.
    if (acc.bufferView < 0 || acc.bufferView >= (int)m.bufferViews.size()) return nullptr;
    const tinygltf::BufferView& bv = m.bufferViews[(size_t)acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= (int)m.buffers.size()) return nullptr;
    const size_t bufSize = m.buffers[(size_t)bv.buffer].data.size();
    if (bv.byteOffset > bufSize || bv.byteLength > bufSize - bv.byteOffset) return nullptr;
    if (acc.count == 0) return &acc;
    const size_t elem =
        (size_t)tinygltf::GetNumComponentsInType((uint32_t)acc.type) *
        (size_t)tinygltf::GetComponentSizeInBytes((uint32_t)acc.componentType);
    const size_t stride = bv.byteStride ? bv.byteStride : elem;
    if (acc.byteOffset > bv.byteLength) return nullptr;
    const size_t avail = bv.byteLength - acc.byteOffset;
    if (elem > avail || (acc.count - 1) > (avail - elem) / stride) return nullptr;
    return &acc;
}

const char* animationChannelProblem(const tinygltf::Model& m, const tinygltf::Animation& a,
                                    const tinygltf::AnimationChannel& c,
                                    const std::vector<size_t>& nodeMorphWeights) {
    constexpr int kF = TINYGLTF_COMPONENT_TYPE_FLOAT;

    if (c.sampler < 0 || c.sampler >= (int)a.samplers.size())
        return "sampler index is out of range";
    if (c.target_node < 0 || c.target_node >= (int)m.nodes.size() ||
        c.target_node >= (int)nodeMorphWeights.size())
        return "target node is out of range";

    int outType = 0;
    size_t valuesPerKey = 1;
    if (c.target_path == "translation" || c.target_path == "scale") {
        outType = TINYGLTF_TYPE_VEC3;
    } else if (c.target_path == "rotation") {
        outType = TINYGLTF_TYPE_VEC4;
    } else if (c.target_path == "weights") {
        outType = TINYGLTF_TYPE_SCALAR;
        valuesPerKey = nodeMorphWeights[(size_t)c.target_node];
        if (valuesPerKey == 0) return "weights target node has no mesh with morph targets";
    } else {
        return "target path is not supported";
    }

    const tinygltf::AnimationSampler& s = a.samplers[(size_t)c.sampler];
    const tinygltf::Accessor* in = checkedAccessor(m, s.input, TINYGLTF_TYPE_SCALAR, {kF});
    if (!in) return "input accessor is out of range, malformed or not SCALAR FLOAT";
    if (in->count == 0) return "input accessor has no keyframes";

    // Range-check the output for every component type glTF allows here, so a
    // legal-but-undecoded normalized integer output gets its own reason.
    const tinygltf::Accessor* out = checkedAccessor(
        m, s.output, outType,
        {kF, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
         TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT});
    if (!out) return "output accessor is out of range, malformed or of the wrong type for its path";
    if (out->componentType != kF)
        return "output accessor uses normalized integer components, which are not decoded";

    // Elements needed: one value (or, CUBICSPLINE, in-tangent/value/out-tangent)
    // per key, times the morph-weight count for weights. Compared by division
    // so a large count cannot overflow the product.
    const size_t perKey = (s.interpolation == "CUBICSPLINE" ? 3u : 1u) * valuesPerKey;
    if (in->count > out->count / perKey)
        return "output accessor holds fewer values than its input keyframes need";
    return nullptr;
}

}  // namespace gltf_check

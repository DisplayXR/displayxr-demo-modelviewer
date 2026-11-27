// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Range checks on a parsed glTF document, run before anything reads it.
 *
 * The glTF backend (model_loader_gltf.cpp) reads index, joint, weight and
 * animation accessors by raw pointer arithmetic over the buffers the file
 * declares, and float vertex attributes through displayxr-common's validated
 * reader (dxr::gltf::ReadFloats, gltf_accessor.h). These checks decide, before
 * that read, whether an accessor or an animation channel can be read without
 * leaving its bufferView/buffer and decoded as the file describes. They check
 * ranges and types only; they decode nothing. Kept out of model_loader_gltf.cpp (which defines the tinygltf
 * implementation and needs stb) so tests/gltf_anim_tests.cpp can exercise them
 * (and tests/gltf_attr_tests.cpp) on hand-built documents with no file, no
 * image decoder and no GPU.
 */

#pragma once

#include <cstddef>
#include <initializer_list>
#include <vector>

// Forward declarations only: tiny_gltf.h's TinyGLTF class changes with the
// TINYGLTF_NO_STB_IMAGE* macros, and model_loader_gltf.cpp must include it
// exactly once, with its own macro set, before TINYGLTF_IMPLEMENTATION.
namespace tinygltf {
class Model;
struct Accessor;
struct Animation;
struct AnimationChannel;
}  // namespace tinygltf

namespace gltf_check {

// `idx` as an accessor of `type` with one of `compTypes`, whose bufferView and
// buffer exist and whose last element (strided by the bufferView's byteStride,
// else tightly packed) ends inside both; nullptr otherwise.
const tinygltf::Accessor* checkedAccessor(const tinygltf::Model& m, int idx, int type,
                                          std::initializer_list<int> compTypes);

// `idx` as a float vertex attribute (or morph-target delta) that
// dxr::gltf::ReadFloats decodes exactly: an accessor of `type` with one of
// `compTypes` that dxr::gltf::ResolveAccessor accepts (every index and byte
// range in bounds, byteStride >= one element, sparse indices and values in
// range; no bufferView = zeros plus sparse overrides). `intNormalized` = an
// integer componentType must also be `normalized` (NORMAL, TANGENT and morph
// NORMAL hold unit vectors). nullptr otherwise.
const tinygltf::Accessor* decodableAttribute(const tinygltf::Model& m, int idx, int type,
                                             std::initializer_list<int> compTypes,
                                             bool intNormalized);

// Why channel `c` of animation `a` cannot be played, or nullptr when it can.
// `nodeMorphWeights[n]` = how many morph weights node n carries (its mesh's
// morph-target count; 0 = none) and must have one entry per m.nodes. On nullptr:
// the sampler index, the target node and both sampler accessors are in range;
// input is SCALAR FLOAT with at least one key; output is FLOAT of the type the
// path needs (translation/scale VEC3, rotation VEC4, weights SCALAR) and holds
// at least input.count * (3 for CUBICSPLINE, else 1) * values-per-key elements
// (values-per-key = the target's morph-weight count for weights, else 1).
const char* animationChannelProblem(const tinygltf::Model& m, const tinygltf::Animation& a,
                                    const tinygltf::AnimationChannel& c,
                                    const std::vector<size_t>& nodeMorphWeights);

}  // namespace gltf_check

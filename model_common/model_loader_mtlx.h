// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  MaterialX OpenPBR → ModelMaterial, for USD scenes that bind `.mtlx`.
 *
 * Issue #70 phase 2. A USD material authored as `references = @x.mtlx@` is a
 * MaterialX node graph feeding an `open_pbr_surface`. This module parses that
 * graph, applies the inputs a stronger USD layer authors over it, EVALUATES the
 * graph per texel (image / extract / invert / combine3 / normalmap / remap /
 * multiply) and bakes the result into the same ModelMaterial fields and texture
 * slots the glTF backend fills from KHR_materials_* — the renderer is shared.
 *
 * It is the C++ port of scripts/openpbr_mtlx_to_gltf.py's material path and
 * must keep the SAME mapping decisions (documented there and in
 * docs/openpbr-to-gltf.md): base_weight premultiplied; transmissive baseColor =
 * mix(base_color, transmission_color, transmission_weight); subsurface and
 * transmission unioned with KHR_materials_scatter carrying the subsurface
 * share; thin-film micrometres → nanometres; emission_luminance 1:1.
 * A difference between the two is a bug in one of them.
 */

#pragma once

#include "model_loader.h"

#include <map>
#include <string>
#include <vector>

// One input value authored over a MaterialX node by a USD layer: numbers (1-4
// components) or a string (asset path / token).
struct MtlxValue {
    std::vector<float> num;
    std::string str;
    bool isString = false;
};
// node name → input name → value, e.g. {"mtlxopen_pbr_surface": {"coat_weight": 0}}
using MtlxOverrides = std::map<std::string, std::map<std::string, MtlxValue>>;

struct MtlxBakeParams {
    int   udimTile = 1001;        // tile whose textures this material instance samples
    float metersPerUnit = 1.0f;   // scene units → metres (transmission_depth, subsurface_radius)
    float thicknessMeters = 0.0f; // KHR_materials_volume thickness stand-in (thinnest extent)
    float nitsPerUnit = 1.0f;     // emission_luminance that maps to emissiveStrength 1
    int   maxTexture = 1024;      // longest texture edge after box downsampling
};

// Bake the open_pbr_surface reached from surfacematerial `materialName` (or the
// file's first one) in `mtlxPath`. Textures are appended to out.textures.
// Returns false when the file has no usable open_pbr_surface; every input or
// node it cannot represent is reported in `warnings` rather than dropped.
bool mtlx_bake_openpbr(const std::string& mtlxPath, const std::string& materialName,
                       const MtlxOverrides& overrides, const MtlxBakeParams& params,
                       ModelData& out, ModelMaterial& mat,
                       std::vector<std::string>& warnings);

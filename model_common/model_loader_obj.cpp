// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OBJ loader backend (tinyobjloader) → ModelData.  [STUB]
 *
 * Routed to from model_loader_load() for .obj. Not yet implemented — returns
 * false so the dispatcher reports the load cleanly. Planned: tinyobj::LoadObj →
 * one primitive per material, positions/normals/uv0 flattened, legacy .mtl
 * materials shimmed to metallic-roughness via model_loader_material.h. See
 * PORTING.md → Multi-format import.
 */

#include "model_loader_backends.h"

#include <cstdio>

bool model_load_obj(const char* path, ModelData& /*out*/) {
    std::fprintf(stderr, "[model_loader/obj] '%s': OBJ loading not yet implemented\n",
                 path ? path : "(null)");
    return false;
}

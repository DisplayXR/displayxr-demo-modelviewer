// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OBJ / STL / FBX loader backend (Assimp) → ModelData.  [STUB]
 *
 * Routed to from model_loader_load() for .obj/.stl/.fbx. Not yet implemented:
 * returns false so the dispatcher reports an unsupported load cleanly. The
 * planned shape (one aiScene → recurse aiNode, bake world transforms, flatten
 * triangles, shim legacy materials to metallic-roughness, resolve embedded +
 * external textures via the common/ stb impl) is documented in PORTING.md.
 *
 * The real fidelity cost lives here, not in parsing: glTF/USD are PBR-native,
 * but OBJ (Phong/.mtl), STL (no material), and most FBX (legacy Phong/Lambert)
 * must be approximated — so this backend owns the material shim.
 */

#include "model_loader_backends.h"

#include <cstdio>

bool model_load_assimp(const char* path, ModelData& /*out*/) {
    std::fprintf(stderr,
        "[model_loader/assimp] '%s': OBJ/STL/FBX loading not yet implemented\n",
        path ? path : "(null)");
    return false;
}

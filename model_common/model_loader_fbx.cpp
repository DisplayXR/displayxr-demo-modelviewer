// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  FBX loader backend (ufbx) → ModelData.  [STUB]
 *
 * Routed to from model_loader_load() for .fbx. Not yet implemented — returns
 * false so the dispatcher reports the load cleanly. Planned: ufbx_load_file with
 * axis/unit conversion to glTF conventions (Y-up, right-handed, metres), walk
 * scene meshes baking node world transforms, triangulate, one primitive per
 * material, PBR maps preferred with the legacy Phong shim as fallback. Static
 * geometry only; skinning/animation deferred. See PORTING.md → Multi-format
 * import.
 */

#include "model_loader_backends.h"

#include <cstdio>

bool model_load_fbx(const char* path, ModelData& /*out*/) {
    std::fprintf(stderr, "[model_loader/fbx] '%s': FBX loading not yet implemented\n",
                 path ? path : "(null)");
    return false;
}

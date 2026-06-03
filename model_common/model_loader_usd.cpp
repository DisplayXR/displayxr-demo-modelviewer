// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  USD(Z) loader backend (tinyusdz) → ModelData.  [STUB]
 *
 * Routed to from model_loader_load() for .usdz/.usd/.usda/.usdc. Not yet
 * implemented: returns false so the dispatcher reports an unsupported load
 * cleanly. The planned shape (LoadUSDZFromFile → tydra::RenderScene flatten →
 * triangulated meshes + UsdPreviewSurface materials → ModelData) is documented
 * in PORTING.md. USD is PBR-native, so no material shim is needed here — this
 * maps as cleanly as the glTF backend.
 */

#include "model_loader_backends.h"

#include <cstdio>

bool model_load_usd(const char* path, ModelData& /*out*/) {
    std::fprintf(stderr,
        "[model_loader/usd] '%s': USD(Z) loading not yet implemented\n",
        path ? path : "(null)");
    return false;
}

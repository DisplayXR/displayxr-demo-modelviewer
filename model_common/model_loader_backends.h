// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Internal per-format loader backends behind model_loader_load().
 *
 * The public entry point model_loader_load() (model_loader.cpp) inspects the
 * file extension and dispatches to one of these backends. Each backend fills a
 * ModelData from one file and obeys the same contract as the public API:
 * returns false on parse failure or when no drawable geometry was found. All
 * backends funnel into the identical ModelData the renderer already consumes —
 * adding a format is front-end work only; the renderer is untouched.
 *
 * Not part of the public surface; platform code includes model_loader.h.
 */

#pragma once

#include "model_loader.h"

// .glb / .gltf — tinygltf. PBR-native. Implemented (model_loader_gltf.cpp).
bool model_load_gltf(const char* path, ModelData& out);

// .obj / .stl / .fbx — Assimp. Materials shimmed to metallic-roughness.
// Stub today (model_loader_assimp.cpp) — see PORTING.md for the phased fill-in.
bool model_load_assimp(const char* path, ModelData& out);

// .usdz / .usd / .usda / .usdc — tinyusdz. PBR-native (UsdPreviewSurface).
// Stub today (model_loader_usd.cpp).
bool model_load_usd(const char* path, ModelData& out);

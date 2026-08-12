// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// The one place the material-texture slot count is written down.
//
// Included by BOTH model_common/shaders/pbr.frag and model_common/model_renderer.h
// — it is nothing but #defines, so it is simultaneously valid GLSL and valid C++.
// That is the point. The per-material SSBO ends in two arrays of this length
// (MaterialExtGpu::uvXf / uvRot, MatExt.uvXf / uvRot), so the count sets the
// struct's STRIDE. When the two sides disagree the shader reads material k at
// the wrong offset: material 0 still lands correctly and every later one does
// not, which reads as "most of the image changed" rather than as anything to do
// with texture slots. assets/material_grid.glb has 70 materials.
//
// Previously the GLSL side was the literal 15, twice, with nothing tying it to
// the C++ enum — grow MTEX_COUNT and the shader silently kept the old stride.
// See issue #81. model_renderer.h static_asserts MTEX_COUNT, MTS_COUNT and
// sizeof(MaterialExtGpu) against the values here, so a divergence is now a
// compile error on both sides.
//
// NOTE when adding a slot: bump MV_TEX_SLOTS, add the enumerator to BOTH
// ModelTexSlot (model_loader.h) and MaterialTexSlot (model_renderer.h), add the
// sampler binding and XF_* constant to pbr.frag, and extend the view arrays in
// model_renderer.cpp. The static_asserts catch the first three; nothing catches
// a forgotten descriptor write except a black texture.

#define MV_TEX_SLOTS 19

// Number of leading vec4 "p" lanes in the material struct, before the UV arrays.
#define MV_MAT_VEC4S 10

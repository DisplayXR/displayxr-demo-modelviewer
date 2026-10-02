// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Scene-light block layout, shared by pbr.frag and model_renderer.h — only
// #defines, so valid GLSL and valid C++ (the material_slots.glsl pattern, #81:
// a constant that sets a GPU block's size and is spelled in two languages is a
// silent-corruption vector unless both sides read the same bytes).
//
// LightingMode::Scene lights the model with the lights the FILE carries (USD
// UsdLux, glTF KHR_lights_punctual) instead of the viewer's sky rig. Each
// light is MV_SCENE_LIGHT_VEC4S vec4s, world space, metres:
//   [0] xyz = position,                     w = type (MV_LIGHT_*)
//   [1] xyz = emission axis (unit; rect/disk/spot point ALONG it,
//             directional: the direction light TRAVELS), w = spot cos(outer)
//   [2] rgb = colour x intensity J,         w = spot cos(inner)
//   [3] xyz = unused,                       w = sphere radius (specular)
// J is radiant intensity in the sense of irradiance = J * shape / d^2 (d in
// metres), the shape factor being 1 (point/sphere), cos (rect/disk, one-sided)
// or the KHR spot cone. See ModelLight in model_loader.h for the unit mapping.

#define MV_MAX_SCENE_LIGHTS 8
#define MV_SCENE_LIGHT_VEC4S 4

#define MV_LIGHT_POINT       0
#define MV_LIGHT_SPOT        1
#define MV_LIGHT_DIRECTIONAL 2
#define MV_LIGHT_RECT        3

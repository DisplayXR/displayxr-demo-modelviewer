# Roadmap

The initial port — turning the gaussiansplat skeleton into a working glTF 2.0
PBR viewer on Windows + macOS — is **complete and shipped** (v0.1.0 Windows,
v0.2.0 macOS + ZDP clip). This file is now the forward roadmap. For how the
renderer works today, see `CLAUDE.md` → *Renderer conventions*.

## Done (the port)

- **`model_common/`** — real renderer, not stubs. tinygltf load (factors +
  textures, world-baked node transforms, AABB), interleaved vertex/index
  upload, metallic-roughness Cook-Torrance GGX, split-sum IBL (BRDF LUT +
  irradiance + prefiltered-env from a procedural analytic sky), blurred skybox,
  sRGB-correct + tangent-free normal mapping, internal image → per-eye viewport
  blit. View math mirrors gs_renderer's Y-flip.
- **Platform glue** — `windows/main.cpp` + `xr_session.cpp` and `macos/main.mm`
  retargeted to `ModelRenderer` (`loadModel`/`hasModel`/`getRobustSceneBounds`,
  `.glb/.gltf` dialogs + drag-drop, `sample.glb` auto-load, 1.2× auto-fit).
- **ZDP clip planes** — `display3d_compute_view/_views` take
  `(clip_front, clip_back)`; per-eye near/far from `eye.z`. Defaults 0.5/2.0;
  transparent → clip_back 0.
- **Bundled `sample.glb`** = Khronos DamagedHelmet, copied next to the exe.
- **Installer + sidecar** — Windows NSIS + macOS `.pkg`; per-app icon names
  (`model_viewer_icon{,_sbs}.png`). Wired into the meta-installer bundle.
- **CI** — `build-windows.yml` + `build-macos.yml` validate every PR + main
  push on both platforms; tags also build+attach installers and dispatch the
  `versions-bump`.

## Next: animation + skinning (the big one)

glTF carries skeletal animation, node (TRS) animation, and morph targets; the
viewer currently renders the **bind pose** only. Phased plan:

1. **Node/TRS animation (no skin)** — parse `animations[]` channels
   (translation/rotation/scale) + samplers (input/output accessors, LINEAR /
   STEP / CUBICSPLINE). Per frame, sample the active animation → per-node local
   TRS → re-walk the node hierarchy to world matrices. Today `model_loader`
   bakes world transforms **once at load**; this must become a per-frame pass
   (keep the static fast-path when no animation is active). Cheapest first win;
   no shader change (push-constant `model` matrix already per-primitive).
2. **Skinning** — extend `ModelVertex` with `joints0` (u16x4) + `weights0`
   (f32x4) vertex attributes; parse `skins[]` (joints list + inverseBindMatrices
   accessor). Add a **joint-matrix SSBO/UBO** (set 3) and do linear-blend
   skinning in `pbr.vert`. Joint matrices = `globalNode * inverseBind`,
   recomputed per frame from the step-1 node walk. Watch the UBO size cap →
   prefer an SSBO for large skeletons.
3. **Morph targets** — accessor deltas + per-node weights; either CPU-blend into
   a dynamic vertex buffer or pass targets as extra attributes/SSBO. Lower
   priority (fewer sample assets use it).
4. **Playback UI** — HUD line (clip name, time, play/pause), a key to cycle
   `animations[]`, auto-play the first clip on load. Keep it minimal.

Suggested bundled animated sample: Khronos **CesiumMan** or **Fox** (small,
CC-licensed, exercises skin + clips).

## Other follow-ups (smaller)

- **`pickSurface`** — ray/triangle intersection vs the loaded mesh (currently a
  no-op, so double-click focus does nothing). Useful for shell focus + future
  inspect features.
- **Draco** mesh decompression (`KHR_draco_mesh_compression`) — many real-world
  `.glb`s use it; needs the Draco decoder wired into `model_loader`.
- **KTX2 / Basis Universal** textures (`KHR_texture_basisu`) — stb is
  PNG/JPEG-only today; GPU-compressed textures need libktx + a transcoder.
- **`KHR_materials_*`** extensions (clearcoat, transmission, emissive strength)
  — incremental PBR fidelity once the above land.

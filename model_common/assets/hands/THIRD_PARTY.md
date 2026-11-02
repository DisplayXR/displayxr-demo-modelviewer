# Third-party asset: Ultraleap GenericHand

`ultraleap_generic_hand_left.glb` is a modified form of the rigged hand mesh
that the Ultraleap Unity plug-in uses for its "Ghost Hands":

- Source: Ultraleap Unity plug-in (Tracking package 5.4.0),
  `Ultraleap/Tracking/Hands/Runtime/Models/GenericHand/GenericHand.fbx`, with
  the alpha of `GenericHandTransparencyMap.png` from the same folder.
  Upstream: <https://github.com/ultraleap/UnityPlugin>
- Copyright (C) Ultraleap, Inc.
- License: Apache License, Version 2.0
  (<https://www.apache.org/licenses/LICENSE-2.0>; the same text as this
  repository's `LICENSE`).

Changes made (by `scripts/convert_ultraleap_hand.py`, run in Blender 5.2):
converted from FBX to glTF 2.0 binary; the transparency map's alpha baked into
a per-vertex `COLOR_0` alpha; the material, UVs and other colour sets removed;
skin weights limited to the 4 strongest influences per vertex. Geometry,
skeleton and bone names are unchanged. The file is compiled into the viewer
binary (`model_common/CMakeLists.txt`) and drawn by
`model_common/hand_renderer.cpp`; the right hand is its mirror image.

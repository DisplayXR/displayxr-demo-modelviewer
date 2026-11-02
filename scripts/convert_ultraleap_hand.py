# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Convert the Ultraleap Unity plug-in's GenericHand (the "Ghost Hands" mesh)
to the skinned .glb the model viewer embeds
(model_common/assets/hands/ultraleap_generic_hand_left.glb).

Runs inside Blender (5.x: its ufbx-based FBX importer, wm.fbx_import, also
reads the ASCII FBX files the plug-in ships, which the older importer refuses):

    blender -b --factory-startup --python scripts/convert_ultraleap_hand.py -- \\
        <Ultraleap/Tracking/Hands/Runtime/Models/GenericHand/GenericHand.fbx> \\
        <.../GenericHand/GenericHandTransparencyMap.png> \\
        model_common/assets/hands/ultraleap_generic_hand_left.glb

What it does: imports the rigged left hand (skin + 26 Leap-named bones intact),
bakes the transparency map's alpha into a per-vertex COLOR_0 alpha (the
reference shader multiplies by that texture; it fades the forearm cut), drops
the material, UVs and every other colour set, and exports glTF with skinning
(4 influences per vertex). The right hand is the mirror image, built at load
time (model_common/hand_skin.h), as the plug-in does.
"""
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
if len(argv) != 3:
    raise SystemExit("usage: blender -b --python convert_ultraleap_hand.py -- <GenericHand.fbx> <TransparencyMap.png> <out.glb>")
fbx, png, out = argv

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.fbx_import(filepath=fbx)
mesh = [o for o in bpy.data.objects if o.type == 'MESH'][0]

img = bpy.data.images.load(png)
W, H = img.size
px = img.pixels[:]
me = mesh.data
uv = me.uv_layers.active

# Per-vertex alpha: the mean of the texture alpha at each corner's UV.
acc = [0.0] * len(me.vertices)
cnt = [0] * len(me.vertices)
for poly in me.polygons:
    for li in poly.loop_indices:
        u, v = uv.data[li].uv
        x = min(W - 1, max(0, int(u % 1.0 * W)))
        y = min(H - 1, max(0, int(v % 1.0 * H)))
        vi = me.loops[li].vertex_index
        acc[vi] += px[(y * W + x) * 4 + 3]
        cnt[vi] += 1
alpha = [acc[i] / cnt[i] if cnt[i] else 1.0 for i in range(len(acc))]

for ca in list(me.color_attributes):
    me.color_attributes.remove(ca)
attr = me.color_attributes.new(name="Col", type='FLOAT_COLOR', domain='POINT')
for i, a in enumerate(alpha):
    attr.data[i].color = (1.0, 1.0, 1.0, a)
me.color_attributes.active_color = attr
me.color_attributes.render_color_index = me.color_attributes.active_color_index
me.materials.clear()

bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_skins=True, export_animations=False,
                          export_materials='NONE', export_texcoords=False, export_normals=True,
                          export_tangents=False, export_vertex_color='ACTIVE', export_all_vertex_colors=False,
                          export_yup=True, use_selection=False)
print("wrote", out, "-", len(me.vertices), "vertices")

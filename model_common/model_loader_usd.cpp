// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  USD(Z) loader backend (tinyusdz / tydra) → ModelData.
 *
 * Routed to from model_loader_load() for .usdz/.usd/.usda/.usdc. We load the
 * stage and let tydra's RenderSceneConverter flatten it into a RenderScene of
 * triangulated, single-indexable meshes + resolved UsdPreviewSurface materials
 * (tydra's `triangulate` + `build_vertex_indices` both default to true, so every
 * mesh's points/normals/texcoords share one vertex index — `faceVertexIndices()`
 * indexes all three). USD is right-handed Y-up like glTF, so no axis flip.
 *
 * USD is PBR-native (UsdPreviewSurface = metallic-roughness): diffuseColor →
 * baseColor, metallic/roughness/emissiveColor map straight across — no Phong
 * shim. Textures resolve via the shader param's texture_id → RenderScene
 * textures → images → embedded buffer (USDZ) or asset path, decoded through the
 * shared model_loader_material helper. Normal-map + combined metallic-roughness
 * textures are a follow-up (UsdPreviewSurface keeps metal/rough as separate
 * single-channel maps; factors are honoured today).
 *
 * MaterialX OpenPBR (issue #70 phase 2). A material authored as
 * `references = @x.mtlx@</MaterialX/Materials/name>` is resolved HERE, not by
 * tydra: tinyusdz composes such a reference to an empty Material (its MaterialX
 * import puts the shader outside the material and drops the inputs), so we
 * record every .mtlx reference before composition consumes it, then bake the
 * graph with model_loader_mtlx -- including the inputs stronger USD layers
 * author over it (`over "mtlxopen_pbr_surface" { inputs:... }`), which is how
 * the ASWF OpenPBR Shader Playground sets 23 of its 54 looks.
 *
 * Scenes are COMPOSED (subLayers, references, payloads, inherits, variants)
 * rather than loaded as a single layer, node transforms are applied, invisible
 * prims are skipped, GeomSubset materials split a mesh, and a material whose
 * meshes sit on different UDIM tiles is baked once per tile.
 */

#include "tinyusdz.hh"
#include "composition.hh"
#include "prim-types.hh"
#include "tydra/prim-apply.hh"
#include "tydra/render-data.hh"
#include "usdGeom.hh"

#include "model_loader_backends.h"
#include "model_loader_material.h"
#include "model_loader_mtlx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

namespace tt = tinyusdz::tydra;

// Resolve a tydra shader-param texture_id → ModelData texture index (or -1):
// texture → image → buffer. tydra (with load_texture_assets) hands back either
// raw decoded texels or the still-encoded file bytes; we also fall back to an
// external asset path for .usda referencing on-disk images.
int loadUsdTexture(const tt::RenderScene& scene, int32_t texId,
                   const std::filesystem::path& dir, ModelData& out) {
    if (texId < 0 || texId >= (int32_t)scene.textures.size()) return -1;
    const int64_t imgId = scene.textures[(size_t)texId].texture_image_id;
    if (imgId < 0 || imgId >= (int64_t)scene.images.size()) return -1;
    const tt::TextureImage& img = scene.images[(size_t)imgId];

    if (img.buffer_id >= 0 && img.buffer_id < (int64_t)scene.buffers.size()) {
        const std::vector<uint8_t>& data = scene.buffers[(size_t)img.buffer_id].data;
        const size_t rawBytes = (size_t)img.width * img.height * img.channels;
        (void)rawBytes;
        // Decoded texels → repack to RGBA8. tydra may decode to 8/16/32-bit per
        // channel (the AR-QuickLook teapot comes back as 32-bit float), so read
        // each component per `texelComponentType` and normalise to u8.
        const size_t texels = (size_t)img.width * img.height;
        const size_t totalComp = texels * (size_t)img.channels;
        if (img.decoded && totalComp > 0 && data.size() >= totalComp &&
            (data.size() % totalComp) == 0) {
            // Drive conversion off bytes-per-channel, NOT texelComponentType:
            // tydra reports UInt8 for the AR-QuickLook teapot yet the buffer is
            // float32 [0,1] (4 B/channel), so the type field can't be trusted.
            const size_t bpc = data.size() / totalComp;
            const int ch = img.channels;
            auto comp = [&](size_t i) -> uint8_t {       // component i → u8
                const uint8_t* b = data.data() + i * bpc;
                switch (bpc) {
                    case 1: return b[0];                                                  // u8
                    case 2: { uint16_t v; std::memcpy(&v, b, 2); return (uint8_t)(v >> 8); }   // u16 (or half — rare)
                    case 4: { float f;  std::memcpy(&f, b, 4); return (uint8_t)std::clamp(f * 255.0f + 0.5f, 0.0f, 255.0f); }  // float [0,1]
                    case 8: { double d; std::memcpy(&d, b, 8); return (uint8_t)std::clamp(d * 255.0 + 0.5, 0.0, 255.0); }
                    default: return b[0];
                }
            };
            ModelTexture t;
            t.width = img.width; t.height = img.height;
            t.rgba.resize(texels * 4);
            for (size_t p = 0; p < texels; ++p) {
                const size_t base = p * (size_t)ch;
                const uint8_t r = comp(base + 0);
                const uint8_t g = (ch >= 3) ? comp(base + 1) : r;
                const uint8_t b = (ch >= 3) ? comp(base + 2) : r;
                const uint8_t a = (ch == 4) ? comp(base + 3) : ((ch == 2) ? comp(base + 1) : 255);
                t.rgba[p*4+0] = r; t.rgba[p*4+1] = g; t.rgba[p*4+2] = b; t.rgba[p*4+3] = a;
            }
            out.textures.push_back(std::move(t));
            return (int)out.textures.size() - 1;
        }
        // Else the buffer holds still-encoded PNG/JPEG bytes → stb decode.
        if (!data.empty()) {
            const int idx = material_load_texture_memory(data.data(), (int)data.size(), out);
            if (idx >= 0) return idx;
        }
    }
    // External asset (typical for .usda referencing image files on disk).
    if (!img.asset_identifier.empty()) {
        int idx = material_load_texture_file(img.asset_identifier, out);
        if (idx >= 0) return idx;
        return material_load_texture_file((dir / img.asset_identifier).string(), out);
    }
    return -1;
}

void accumulateBBox(ModelData& out, const float* p) {
    if (!out.hasBBox) {
        out.bboxMin[0] = out.bboxMax[0] = p[0];
        out.bboxMin[1] = out.bboxMax[1] = p[1];
        out.bboxMin[2] = out.bboxMax[2] = p[2];
        out.hasBBox = true;
    } else {
        out.bboxMin[0] = std::min(out.bboxMin[0], p[0]); out.bboxMax[0] = std::max(out.bboxMax[0], p[0]);
        out.bboxMin[1] = std::min(out.bboxMin[1], p[1]); out.bboxMax[1] = std::max(out.bboxMax[1], p[1]);
        out.bboxMin[2] = std::min(out.bboxMin[2], p[2]); out.bboxMax[2] = std::max(out.bboxMax[2], p[2]);
    }
}

}  // namespace

namespace {

// ── composition ───────────────────────────────────────────────────────────────

struct MtlxRef { std::string file; std::string material; };

// Every Material that references a .mtlx, keyed by absolute prim path, with the
// file resolved against the directory of the layer that authored it.
void collectMtlxRefs(const tinyusdz::PrimSpec& ps, const std::string& parent,
                     const std::string& rootDir, std::map<std::string, MtlxRef>& out) {
    const std::string path = parent + "/" + ps.name();
    if (ps.metas().references) {
        for (const auto& lo : ps.metas().references.value())
            for (const tinyusdz::Reference& r : lo.second) {
                const std::string a = r.asset_path.GetAssetPath();
                if (a.size() < 5 || a.compare(a.size() - 5, 5, ".mtlx") != 0) continue;
                std::filesystem::path f = std::filesystem::path(ps.get_current_working_path()) / a;
                if (ps.get_current_working_path().empty() || !std::filesystem::exists(f))
                    f = std::filesystem::path(rootDir) / a;
                std::string mat = r.prim_path.prim_part();
                mat = mat.substr(mat.find_last_of('/') + 1);
                out[path] = {f.lexically_normal().string(), mat};
            }
    }
    for (const auto& c : ps.children()) collectMtlxRefs(c, path, rootDir, out);
}

bool composeStage(const std::string& path, tinyusdz::Stage& stage,
                  std::map<std::string, MtlxRef>& mtlx, std::string& err) {
    using namespace tinyusdz;
    const std::string dir = std::filesystem::path(path).parent_path().string();
    Layer layer;
    std::string warn;
    if (!LoadLayerFromFile(path, &layer, &warn, &err)) return false;
    AssetResolutionResolver resolver;
    resolver.set_current_working_path(dir);
    resolver.set_search_paths({dir});
    {
        Layer o;
        if (!CompositeSublayers(resolver, layer, &o, &warn, &err)) return false;
        layer = std::move(o);
    }
    for (const auto& kv : layer.primspecs()) collectMtlxRefs(kv.second, "", dir, mtlx);
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        auto step = [&](bool pending, auto&& fn) -> bool {
            if (!pending) return true;
            Layer o;
            if (!fn(o)) return false;
            layer = std::move(o);
            changed = true;
            return true;
        };
        if (!step(layer.check_unresolved_references(), [&](Layer& o) {
                return CompositeReferences(resolver, layer, &o, &warn, &err); })) return false;
        if (!step(layer.check_unresolved_payload(), [&](Layer& o) {
                return CompositePayload(resolver, layer, &o, &warn, &err); })) return false;
        if (!step(layer.check_unresolved_inherits(), [&](Layer& o) {
                return CompositeInherits(layer, &o, &warn, &err); })) return false;
        if (!step(layer.check_unresolved_variant(), [&](Layer& o) {
                return CompositeVariant(layer, &o, &warn, &err); })) return false;
        if (!changed) break;
    }
    return LayerToStage(std::move(layer), &stage, &warn, &err);
}

// ── stage queries ─────────────────────────────────────────────────────────────

const tinyusdz::Prim* findPrim(const tinyusdz::Stage& stage, const std::string& p) {
    const tinyusdz::Prim* prim = nullptr;
    std::string e;
    if (p.empty() || !stage.find_prim_at_path(tinyusdz::Path(p, ""), prim, &e)) return nullptr;
    return prim;
}

// USD visibility is inherited: invisible if the prim or ANY ancestor says so.
bool isVisible(const tinyusdz::Stage& stage, const std::string& absPath) {
    std::string p = absPath;
    while (!p.empty() && p != "/") {
        if (const tinyusdz::Prim* prim = findPrim(stage, p)) {
            bool invisible = false;
            tt::ApplyToGPrim(stage, *prim, [&](const tinyusdz::Stage&, const tinyusdz::GPrim* gp) {
                const tinyusdz::Animatable<tinyusdz::Visibility>& v = gp->visibility.get_value();
                tinyusdz::Visibility vis;
                if (v.get_scalar(&vis) && vis == tinyusdz::Visibility::Invisible) invisible = true;
                return true;
            });
            if (invisible) return false;
        }
        p = p.substr(0, p.find_last_of('/'));
    }
    return true;
}

// Inputs authored over the MaterialX graph under a Material prim: its children
// are the over'd nodes (`over "mtlxopen_pbr_surface"`), their `inputs:*` the values.
MtlxOverrides readOverrides(const tinyusdz::Stage& stage, const std::string& matPath) {
    MtlxOverrides ov;
    const tinyusdz::Prim* mp = findPrim(stage, matPath);
    if (!mp) return ov;
    for (const tinyusdz::Prim& child : mp->children()) {
        const std::map<std::string, tinyusdz::Property>* props = nullptr;
        if (const auto* m = child.as<tinyusdz::Model>()) props = &m->props;
        else if (const auto* sh = child.as<tinyusdz::Shader>()) props = &sh->props;
        if (!props) continue;
        for (const auto& kv : *props) {
            if (kv.first.rfind("inputs:", 0) != 0 || !kv.second.is_attribute()) continue;
            const tinyusdz::Attribute& a = kv.second.get_attribute();
            MtlxValue v;
            namespace tv = tinyusdz::value;
            if (auto f = a.get_value<float>()) v.num = {*f};
            else if (auto d = a.get_value<double>()) v.num = {(float)*d};
            else if (auto i = a.get_value<int>()) v.num = {(float)*i};
            else if (auto b = a.get_value<bool>()) v.num = {*b ? 1.0f : 0.0f};
            else if (auto c = a.get_value<tv::color3f>()) v.num = {c->r, c->g, c->b};
            else if (auto f3 = a.get_value<tv::float3>()) v.num = {(*f3)[0], (*f3)[1], (*f3)[2]};
            else if (auto f2 = a.get_value<tv::float2>()) v.num = {(*f2)[0], (*f2)[1]};
            else if (auto f4 = a.get_value<tv::float4>()) v.num = {(*f4)[0], (*f4)[1], (*f4)[2], (*f4)[3]};
            else if (auto ap = a.get_value<tv::AssetPath>()) { v.isString = true; v.str = ap->GetAssetPath(); }
            else if (auto tk = a.get_value<tv::token>()) { v.isString = true; v.str = tk->str(); }
            else continue;
            ov[child.element_name()][kv.first.substr(7)] = v;
        }
    }
    return ov;
}

// One drawable: a mesh's triangles (all, or one GeomSubset) with one material.
struct Group {
    size_t mesh;
    std::vector<uint32_t> tris;   // triangle indices into the mesh's faceVertexIndices/3
    int material;                 // RenderScene material id, -1 = default
    int tile = 1001;              // UDIM tile the group's UVs sit on
    bool doubleSided = false;     // USD gprim doubleSided (default false = cull backfaces)
    float uvShift[2] = {0, 0};
};

}  // namespace

bool model_load_usd(const char* path, ModelData& out) {
    if (!path) return false;
    const auto t0 = std::chrono::steady_clock::now();
    const std::filesystem::path modelDir = std::filesystem::path(path).parent_path();
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    tinyusdz::Stage stage;
    std::map<std::string, MtlxRef> mtlxRefs;
    std::string warn, err;
    bool composed = false;
    if (ext != ".usdz") {
        std::string cerr;
        composed = composeStage(path, stage, mtlxRefs, cerr);
        if (!composed)
            std::fprintf(stderr, "[model_loader/usd] composition failed (%s) - loading the root layer only\n",
                         cerr.substr(0, 300).c_str());
    }
    if (!composed) {
        stage = tinyusdz::Stage();
        mtlxRefs.clear();
        if (!tinyusdz::LoadUSDFromFile(path, &stage, &warn, &err)) {  // auto-detects usdz/usda/usdc
            std::fprintf(stderr, "[model_loader/usd] '%s': %s\n", path,
                         err.empty() ? "load failed" : err.c_str());
            return false;
        }
        if (!warn.empty()) std::fprintf(stderr, "[model_loader/usd] warn: %s\n", warn.c_str());
    }

    tt::RenderScene scene;
    tt::RenderSceneConverter converter;
    tt::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = true;  // decode/extract texture images

    // USDZ keeps its textures inside the zip package — a plain disk search path
    // can't reach them, so install the USDZ asset resolver. usdz_asset must stay
    // alive until ConvertToRenderScene() returns (it backs the resolver).
    tinyusdz::USDZAsset usdz_asset;
    tinyusdz::AssetResolutionResolver arr;
    if (ext == ".usdz") {
        std::string w2, e2;
        if (tinyusdz::ReadUSDZAssetInfoFromFile(path, &usdz_asset, &w2, &e2) &&
            tinyusdz::SetupUSDZAssetResolution(arr, &usdz_asset)) {
            env.asset_resolver = arr;
        } else {
            std::fprintf(stderr, "[model_loader/usd] USDZ asset resolution setup failed: %s\n",
                         e2.c_str());
            env.set_search_paths({ modelDir.string() });
        }
    } else {
        env.set_search_paths({ modelDir.string() });
    }

    if (!converter.ConvertToRenderScene(env, &scene)) {
        std::fprintf(stderr, "[model_loader/usd] '%s': RenderScene convert failed: %s\n",
                     path, converter.GetError().c_str());
        return false;
    }

    const float mpu = (float)stage.metas().metersPerUnit.get_value();  // scene units → metres

    // ── world transform per mesh, from tydra's node tree (row-vector matrices) ──
    std::map<int, tinyusdz::value::matrix4d> meshXf;
    std::function<void(const tt::Node&)> walk = [&](const tt::Node& n) {
        if (n.category == tt::NodeCategory::Geom && n.id >= 0) meshXf[n.id] = n.global_matrix;
        for (const auto& c : n.children) walk(c);
    };
    for (const auto& n : scene.nodes) walk(n);

    // ── pass 1: drawable groups (visibility, GeomSubsets, UDIM tile) ──
    std::vector<Group> groups;
    size_t hidden = 0;
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const tt::RenderMesh& mesh = scene.meshes[mi];
        const std::vector<uint32_t>& fvi = mesh.faceVertexIndices();
        if (mesh.points.empty() || fvi.size() < 3) continue;
        if (composed && !isVisible(stage, mesh.abs_path)) { ++hidden; continue; }
        const size_t ntri = fvi.size() / 3;
        std::vector<char> claimed(ntri, 0);
        std::vector<Group> mine;
        for (const auto& kv : mesh.material_subsetMap) {
            Group g{mi, {}, kv.second.material_id};
            for (int t : kv.second.triangulatedIndices)
                if (t >= 0 && (size_t)t < ntri && !claimed[(size_t)t]) { g.tris.push_back((uint32_t)t); claimed[(size_t)t] = 1; }
            if (!g.tris.empty()) mine.push_back(std::move(g));
        }
        Group rest{mi, {}, mesh.material_id};
        for (size_t t = 0; t < ntri; ++t) if (!claimed[t]) rest.tris.push_back((uint32_t)t);
        if (!rest.tris.empty()) mine.push_back(std::move(rest));

        // UDIM: the group's tile is the median UV's; its UVs move back to [0,1).
        auto uvIt = mesh.texcoords.find(0);
        const bool hasUV = uvIt != mesh.texcoords.end() && uvIt->second.vertex_count() >= mesh.points.size();
        const float* uv = hasUV ? reinterpret_cast<const float*>(uvIt->second.get_data().data()) : nullptr;
        for (Group& g : mine) {
            g.doubleSided = mesh.doubleSided;
            if (uv) {
                std::vector<float> us, vs;
                for (uint32_t t : g.tris) for (int k = 0; k < 3; ++k) {
                    uint32_t vi = fvi[t * 3 + k];
                    us.push_back(uv[vi * 2]); vs.push_back(uv[vi * 2 + 1]);
                }
                auto med = [](std::vector<float>& a) { std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end()); return a[a.size() / 2]; };
                const float tu = std::max(0.0f, std::floor(med(us))), tv = std::max(0.0f, std::floor(med(vs)));
                g.uvShift[0] = tu; g.uvShift[1] = tv;
                g.tile = 1001 + (int)tu + 10 * (int)tv;
            }
            groups.push_back(std::move(g));
        }
    }

    // world-space (metres) position of a mesh vertex
    auto worldPos = [&](size_t mesh, uint32_t v, float o[3]) {
        const float* p = reinterpret_cast<const float*>(scene.meshes[mesh].points.data()) + v * 3;
        double x = p[0], y = p[1], z = p[2];
        auto it = meshXf.find((int)mesh);
        if (it != meshXf.end()) {
            const auto& m = it->second.m;
            double X = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
            double Y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
            double Z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
            x = X; y = Y; z = Z;
        }
        o[0] = (float)(x * mpu); o[1] = (float)(y * mpu); o[2] = (float)(z * mpu);
    };

    // ── pass 2: materials, one per (RenderScene material, UDIM tile, sidedness) ──
    // USD puts doubleSided on the GPRIM, the renderer on the material, so a
    // material shared by single- and double-sided meshes becomes two instances.
    // The per-instance bbox gives the volume-thickness stand-in, as in the
    // converter: the thinnest extent of the material's meshes.
    using Key = std::tuple<int, int, bool>;   // (material, tile, doubleSided)
    std::map<Key, std::pair<std::array<float, 3>, std::array<float, 3>>> extents;
    for (const Group& g : groups) {
        auto& e = extents.try_emplace(Key{g.material, g.tile, g.doubleSided},
            std::array<float, 3>{1e30f, 1e30f, 1e30f}, std::array<float, 3>{-1e30f, -1e30f, -1e30f}).first->second;
        const auto& fvi = scene.meshes[g.mesh].faceVertexIndices();
        for (uint32_t t : g.tris) for (int k = 0; k < 3; ++k) {
            float w[3]; worldPos(g.mesh, fvi[t * 3 + k], w);
            for (int a = 0; a < 3; ++a) { e.first[a] = std::min(e.first[a], w[a]); e.second[a] = std::max(e.second[a], w[a]); }
        }
    }

    MtlxBakeParams bp;
    bp.metersPerUnit = mpu;
    if (const char* e = std::getenv("DXR_MODELVIEWER_MTLX_MAXTEX")) bp.maxTexture = std::max(16, std::atoi(e));
    std::map<Key, int> matIndex;
    size_t nMtlx = 0;
    std::vector<std::string> mtlxWarn;
    auto previewMaterial = [&](const tt::RenderMaterial& rm) {
        ModelMaterial mm{};
        mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
        mm.baseColorFactor[3] = 1.0f;
        mm.metallic = 0.0f;
        mm.roughness = 0.8f;
        if (rm.surfaceShader.has_value()) {
            const tt::PreviewSurfaceShader& sh = *rm.surfaceShader;
            const auto dc = sh.diffuseColor.value;
            mm.baseColorFactor[0] = dc[0]; mm.baseColorFactor[1] = dc[1]; mm.baseColorFactor[2] = dc[2];
            mm.baseColorFactor[3] = sh.opacity.value;
            mm.metallic  = sh.metallic.value;
            mm.roughness = sh.roughness.value;
            const auto ec = sh.emissiveColor.value;
            mm.emissive[0] = ec[0]; mm.emissive[1] = ec[1]; mm.emissive[2] = ec[2];
            if (sh.diffuseColor.is_texture())
                mm.baseColorTex = loadUsdTexture(scene, sh.diffuseColor.texture_id, modelDir, out);
            if (sh.emissiveColor.is_texture())
                mm.emissiveTex = loadUsdTexture(scene, sh.emissiveColor.texture_id, modelDir, out);
        }
        return mm;
    };
    // MaterialX bakes decode and resample every map (the Playground: ~110 TIFFs
    // at 4096^2), so they run in parallel. Each job fills its OWN texture list;
    // the merge below appends them and rebases the material's texture indices.
    struct Job {
        Key key;
        const tt::RenderMaterial* rm = nullptr;
        const MtlxRef* ref = nullptr;
        MtlxBakeParams params;
        ModelMaterial mat{};
        ModelData tex;             // only .textures is used
        std::vector<std::string> warn;
        bool baked = false;
    };
    std::vector<Job> jobs;
    for (const auto& kv : extents) {
        Job j;
        j.key = kv.first;
        const int rmId = std::get<0>(kv.first);
        if (rmId >= 0 && rmId < (int)scene.materials.size()) {
            j.rm = &scene.materials[(size_t)rmId];
            if (auto ref = mtlxRefs.find(j.rm->abs_path); ref != mtlxRefs.end()) j.ref = &ref->second;
        }
        j.params = bp;
        j.params.udimTile = std::get<1>(kv.first);
        const auto& e = kv.second;
        j.params.thicknessMeters = std::max(0.0f, std::min({e.second[0] - e.first[0],
                                                            e.second[1] - e.first[1],
                                                            e.second[2] - e.first[2]}));
        jobs.push_back(std::move(j));
    }
    {
        std::atomic<size_t> next{0};
        auto worker = [&]() {
            for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
                Job& j = jobs[i];
                if (!j.ref) continue;
                j.baked = mtlx_bake_openpbr(j.ref->file, j.ref->material,
                                            readOverrides(stage, j.rm->abs_path), j.params,
                                            j.tex, j.mat, j.warn);
            }
        };
        const unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < n; ++t) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
    }
    for (Job& j : jobs) {
        ModelMaterial mm{};
        mm.baseColorFactor[0] = mm.baseColorFactor[1] = mm.baseColorFactor[2] = 0.8f;
        mm.metallic = 0.0f; mm.roughness = 0.8f;
        if (j.baked) {
            mm = j.mat;
            const int base = (int)out.textures.size();
            for (int* t : {&mm.baseColorTex, &mm.metallicRoughnessTex, &mm.normalTex, &mm.occlusionTex,
                           &mm.emissiveTex, &mm.clearcoatTex, &mm.clearcoatRoughnessTex,
                           &mm.sheenColorTex, &mm.sheenRoughnessTex, &mm.specularTex,
                           &mm.specularColorTex, &mm.transmissionTex, &mm.thicknessTex,
                           &mm.scatterStrengthTex, &mm.multiscatterColorTex, &mm.coatColorTex,
                           &mm.coatAnisotropyTex, &mm.diffuseRoughnessTex, &mm.fuzzTex,
                           &mm.coatNormalTex})
                if (*t >= 0) *t += base;
            for (auto& t : j.tex.textures) out.textures.push_back(std::move(t));
            ++nMtlx;
        } else if (j.rm) {
            mm = previewMaterial(*j.rm);
        }
        for (auto& w : j.warn) mtlxWarn.push_back((j.rm ? j.rm->name : std::string("?")) + ": " + w);
        mm.doubleSided = std::get<2>(j.key);
        matIndex[j.key] = (int)out.materials.size();
        out.materials.push_back(mm);
    }
    // de-duplicate warnings (one image node warns once per tile instance)
    std::sort(mtlxWarn.begin(), mtlxWarn.end());
    mtlxWarn.erase(std::unique(mtlxWarn.begin(), mtlxWarn.end()), mtlxWarn.end());
    for (const auto& w : mtlxWarn) std::fprintf(stderr, "[model_loader/usd] mtlx: %s\n", w.c_str());

    // ── pass 3: geometry, world-space metres, per group ──
    for (const Group& g : groups) {
        const tt::RenderMesh& mesh = scene.meshes[g.mesh];
        const std::vector<uint32_t>& fvi = mesh.faceVertexIndices();
        const size_t vcount = mesh.points.size();
        const bool hasN  = !mesh.normals.empty() && mesh.normals.vertex_count() >= vcount;
        const float* nrm = hasN ? reinterpret_cast<const float*>(mesh.normals.get_data().data()) : nullptr;
        auto uvIt = mesh.texcoords.find(0);
        const bool hasUV = uvIt != mesh.texcoords.end() && uvIt->second.vertex_count() >= vcount;
        const float* uv  = hasUV ? reinterpret_cast<const float*>(uvIt->second.get_data().data()) : nullptr;

        // normal matrix = inverse-transpose of the upper 3x3 (row-vector form)
        double nm[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        bool flip = false;
        if (auto it = meshXf.find((int)g.mesh); it != meshXf.end()) {
            const auto& m = it->second.m;
            double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2],
                   gg = m[2][0], h = m[2][1], k = m[2][2];
            double det = a * (e * k - f * h) - b * (d * k - f * gg) + c * (d * h - e * gg);
            if (std::fabs(det) > 1e-20) {
                // cofactor matrix / det = inverse-transpose
                double co[3][3] = {{e * k - f * h, -(d * k - f * gg), d * h - e * gg},
                                   {-(b * k - c * h), a * k - c * gg, -(a * h - b * gg)},
                                   {b * f - c * e, -(a * f - c * d), a * e - b * d}};
                for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 3; ++cc) nm[r][cc] = co[r][cc] / det;
                flip = det < 0;
            }
        }

        ModelPrimitive mp{};
        mp.firstVertex = (uint32_t)out.vertices.size();
        mp.firstIndex  = (uint32_t)out.indices.size();
        const Key key{g.material, g.tile, g.doubleSided};
        mp.material = matIndex.count(key) ? matIndex[key] : -1;
        mp.node = -1; mp.skin = -1; mp.morph = -1;
        std::memset(mp.modelMatrix, 0, sizeof(mp.modelMatrix));
        mp.modelMatrix[0] = mp.modelMatrix[5] = mp.modelMatrix[10] = mp.modelMatrix[15] = 1.0f;

        // tydra expands every face corner into its own vertex (faceVarying
        // primvars force it), so weld identical corners back together: the
        // Playground drops from 8.6 M vertices to ~2 M.
        struct VKey {
            int32_t q[8];
            bool operator==(const VKey& o) const { return std::memcmp(q, o.q, sizeof(q)) == 0; }
        };
        struct VHash {
            size_t operator()(const VKey& k) const {
                size_t h = 1469598103934665603ull;
                for (int32_t v : k.q) { h ^= (uint32_t)v; h *= 1099511628211ull; }
                return h;
            }
        };
        std::unordered_map<VKey, uint32_t, VHash> weld;
        weld.reserve(g.tris.size() * 2);
        for (uint32_t t : g.tris) {
            uint32_t tri[3] = {fvi[t * 3], fvi[t * 3 + 1], fvi[t * 3 + 2]};
            if (flip) std::swap(tri[1], tri[2]);
            for (uint32_t vi : tri) {
                if (vi >= vcount) continue;
                {
                    ModelVertex mv{};
                    worldPos(g.mesh, vi, mv.pos);
                    if (nrm) {
                        const float* n = nrm + vi * 3;
                        double wx = n[0] * nm[0][0] + n[1] * nm[1][0] + n[2] * nm[2][0];
                        double wy = n[0] * nm[0][1] + n[1] * nm[1][1] + n[2] * nm[2][1];
                        double wz = n[0] * nm[0][2] + n[1] * nm[1][2] + n[2] * nm[2][2];
                        double l = std::sqrt(wx * wx + wy * wy + wz * wz);
                        if (l > 0) { mv.normal[0] = (float)(wx / l); mv.normal[1] = (float)(wy / l); mv.normal[2] = (float)(wz / l); }
                    } else {
                        mv.normal[1] = 1.0f;
                    }
                    if (uv) {  // USD V bottom-up → flip; UDIM tile moved back to [0,1)
                        mv.uv[0] = uv[vi * 2] - g.uvShift[0];
                        mv.uv[1] = 1.0f - (uv[vi * 2 + 1] - g.uvShift[1]);
                    }
                    VKey k;
                    for (int a = 0; a < 3; ++a) k.q[a] = (int32_t)std::lround(mv.pos[a] * 1e5f);
                    for (int a = 0; a < 3; ++a) k.q[3 + a] = (int32_t)std::lround(mv.normal[a] * 1e4f);
                    k.q[6] = (int32_t)std::lround(mv.uv[0] * 1e5f);
                    k.q[7] = (int32_t)std::lround(mv.uv[1] * 1e5f);
                    auto it = weld.find(k);
                    if (it == weld.end()) {
                        accumulateBBox(out, mv.pos);
                        it = weld.emplace(k, (uint32_t)(out.vertices.size() - mp.firstVertex)).first;
                        out.vertices.push_back(mv);
                    }
                    out.indices.push_back(mp.firstVertex + it->second);
                }
            }
        }
        mp.vertexCount = (uint32_t)(out.vertices.size() - mp.firstVertex);
        mp.indexCount  = (uint32_t)out.indices.size() - mp.firstIndex;
        if (mp.indexCount > 0) out.primitives.push_back(mp);
    }

    // ── lights (UsdLux) → ModelLight; units per ModelLight in model_loader.h ──
    size_t skippedLights = 0;
    for (const tt::RenderLight& L : scene.lights) {
        if (composed && !isVisible(stage, L.abs_path)) continue;
        using LT = tt::RenderLight::Type;
        if (L.type == LT::Dome || L.type == LT::Geometry || L.type == LT::Portal) { ++skippedLights; continue; }
        const auto& m = L.transform.m;   // row-vector convention, as the node matrices
        auto rowLen = [&](int r) { return std::sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]); };
        ModelLight ml;
        ml.name = L.name;
        for (int a = 0; a < 3; ++a) ml.position[a] = (float)m[3][a] * mpu;
        const float zl = rowLen(2);
        for (int a = 0; a < 3; ++a) ml.axis[a] = zl > 0 ? -(float)m[2][a] / zl : (a == 2 ? -1.0f : 0.0f);  // local -Z
        for (int a = 0; a < 3; ++a) ml.color[a] = L.color[a];
        if (L.enableColorTemperature)
            std::fprintf(stderr, "[model_loader/usd] light %s: colorTemperature not applied\n", L.name.c_str());
        const float E = L.intensity * std::exp2(L.exposure);
        const float sx = rowLen(0), sy = rowLen(1), mpu2 = mpu * mpu;
        switch (L.type) {
            case LT::Distant:
                ml.type = ModelLight::Type::Directional;
                ml.intensity = E;
                break;
            case LT::Rect:
                ml.type = ModelLight::Type::Rect;
                ml.intensity = L.normalize ? E * mpu2 : E * (L.width * sx * mpu) * (L.height * sy * mpu);
                break;
            case LT::Disk:
                ml.type = ModelLight::Type::Rect;   // one-sided cosine emitter, like rect
                ml.intensity = L.normalize ? E * mpu2 : E * 3.14159265f * std::pow(L.radius * sx * mpu, 2.0f);
                break;
            default: {   // Point, Sphere, Cylinder (as a point)
                ml.type = ModelLight::Type::Point;
                const float r = L.radius * sx * mpu;
                ml.radius = r;
                ml.intensity = L.normalize ? E * mpu2 / 4.0f : E * 3.14159265f * r * r;
                break;
            }
        }
        std::fprintf(stderr, "[model_loader/usd] light %s: type %d pos (%.3f, %.3f, %.3f) m axis (%.2f, %.2f, %.2f) "
                     "J %.4g (I %.4g, EV %.2f, normalize %d, scale %.3f x %.3f)\n",
                     ml.name.c_str(), (int)ml.type, ml.position[0], ml.position[1], ml.position[2],
                     ml.axis[0], ml.axis[1], ml.axis[2], ml.intensity, L.intensity, L.exposure,
                     (int)L.normalize, sx, sy);
        out.lights.push_back(ml);
    }
    if (!scene.lights.empty())
        std::fprintf(stderr, "[model_loader/usd] %zu light(s) imported, %zu skipped (dome/geometry/portal)\n",
                     out.lights.size(), skippedLights);

    // ── cameras: world transform from tydra's node tree, focusDistance from the stage ──
    std::function<void(const tt::Node&)> camWalk = [&](const tt::Node& n) {
        if (n.category == tt::NodeCategory::Camera && n.id >= 0 && (size_t)n.id < scene.cameras.size()) {
            const tt::RenderCamera& rc = scene.cameras[(size_t)n.id];
            const auto& m = n.global_matrix.m;
            ModelCamera mc;
            mc.name = rc.name;
            auto unit = [&](int r, float* o, float sign) {
                double l = std::sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]);
                for (int a = 0; a < 3; ++a) o[a] = l > 0 ? (float)(sign * m[r][a] / l) : 0.0f;
            };
            for (int a = 0; a < 3; ++a) mc.position[a] = (float)m[3][a] * mpu;
            unit(2, mc.forward, -1.0f);   // cameras look down local -Z
            unit(1, mc.up, 1.0f);
            mc.vfov = 2.0f * std::atan(0.5f * rc.verticalAperture / std::max(rc.focalLength, 1e-3f));
            if (const tinyusdz::Prim* p = findPrim(stage, rc.abs_path))
                if (const auto* gc = p->as<tinyusdz::GeomCamera>()) {
                    float fd = 0.0f;
                    if (gc->focusDistance.get_value().get_scalar(&fd) && fd > 0) mc.focusDistance = fd * mpu;
                }
            out.cameras.push_back(mc);
        }
        for (const auto& c : n.children) camWalk(c);
    };
    for (const auto& n : scene.nodes) camWalk(n);

    out.primitiveCount = (uint32_t)out.primitives.size();
    if (out.primitiveCount == 0 || out.vertices.empty()) {
        std::fprintf(stderr, "[model_loader/usd] '%s': no drawable triangle geometry\n", path);
        return false;
    }
    std::fprintf(stderr,
        "[model_loader/usd] '%s': %s, %zu meshes (%zu hidden), %u prims, %zu verts, "
        "%zu materials (%zu MaterialX OpenPBR), %zu textures, %.1f s\n",
        path, composed ? "composed" : "root layer", scene.meshes.size(), hidden,
        out.primitiveCount, out.vertices.size(), out.materials.size(), nMtlx, out.textures.size(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

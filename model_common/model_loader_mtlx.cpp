// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  MaterialX OpenPBR → ModelMaterial (see model_loader_mtlx.h).
 *
 * Three stages, each kept small on purpose:
 *   1. a MaterialX-subset XML reader. Not a general XML parser: MaterialX files
 *      put `<UDIM>` inside attribute values, which a conforming parser rejects,
 *      and we need only elements, attributes and nesting.
 *   2. a per-texel graph evaluator over float images/constants.
 *   3. the OpenPBR → ModelMaterial bake, mirroring openpbr_mtlx_to_gltf.py.
 * Images decode through tinyusdz's loader (PNG/JPEG via stb, TIFF via TinyDNG,
 * EXR via tinyexr) because OpenPBR content ships its maps as LZW TIFF, which
 * stb cannot read.
 */

#include "model_loader_mtlx.h"

#include "image-loader.hh"   // tinyusdz::image::LoadImageFromFile

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// ───────────────────────────── 1. XML subset ─────────────────────────────────

struct XElem {
    std::string tag;
    std::map<std::string, std::string> attr;
    std::vector<XElem> kids;
    const std::string* get(const char* k) const {
        auto it = attr.find(k);
        return it == attr.end() ? nullptr : &it->second;
    }
};

std::string xmlDecode(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        static const std::pair<const char*, char> ents[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool hit = false;
        for (auto& e : ents) {
            size_t n = std::strlen(e.first);
            if (s.compare(i, n, e.first) == 0) { o += e.second; i += n - 1; hit = true; break; }
        }
        if (!hit) o += s[i];
    }
    return o;
}

class XmlReader {
public:
    explicit XmlReader(const std::string& s) : s_(s) {}
    // Parses the first top-level element into `root`.
    bool parse(XElem& root) {
        while (skipMisc()) {
            if (peek('<') && !peek("</")) return element(root);
            if (i_ < s_.size()) ++i_;
        }
        return false;
    }
private:
    bool peek(char c) const { return i_ < s_.size() && s_[i_] == c; }
    bool peek(const char* p) const { return s_.compare(i_, std::strlen(p), p) == 0; }
    void ws() { while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) ++i_; }
    // Skips whitespace, text, comments, <?...?> and <!...>; true while input remains.
    bool skipMisc() {
        for (;;) {
            while (i_ < s_.size() && s_[i_] != '<') ++i_;
            if (i_ >= s_.size()) return false;
            if (peek("<!--")) { size_t e = s_.find("-->", i_); i_ = e == std::string::npos ? s_.size() : e + 3; continue; }
            if (peek("<?"))   { size_t e = s_.find("?>", i_);  i_ = e == std::string::npos ? s_.size() : e + 2; continue; }
            if (peek("<!"))   { size_t e = s_.find('>', i_);   i_ = e == std::string::npos ? s_.size() : e + 1; continue; }
            return true;
        }
    }
    std::string name() {
        size_t b = i_;
        while (i_ < s_.size() && !std::isspace((unsigned char)s_[i_]) && s_[i_] != '>' &&
               s_[i_] != '/' && s_[i_] != '=')
            ++i_;
        return s_.substr(b, i_ - b);
    }
    bool element(XElem& e) {
        ++i_;  // '<'
        e.tag = name();
        for (;;) {
            ws();
            if (i_ >= s_.size()) return false;
            if (peek("/>")) { i_ += 2; return true; }
            if (peek('>')) { ++i_; break; }
            std::string k = name();
            ws();
            if (!peek('=')) { e.attr[k] = ""; continue; }
            ++i_; ws();
            if (i_ >= s_.size()) return false;
            char q = s_[i_];
            if (q != '"' && q != '\'') return false;
            size_t b = ++i_;
            // Value runs to the matching quote -- deliberately allowing '<' '>'
            // inside, which is how MaterialX writes `<UDIM>` tokens.
            size_t end = s_.find(q, b);
            if (end == std::string::npos) return false;
            e.attr[k] = xmlDecode(s_.substr(b, end - b));
            i_ = end + 1;
        }
        // children until matching close tag
        for (;;) {
            if (!skipMisc()) return false;
            if (peek("</")) {
                size_t e2 = s_.find('>', i_);
                i_ = e2 == std::string::npos ? s_.size() : e2 + 1;
                return true;
            }
            XElem kid;
            if (!element(kid)) return false;
            e.kids.push_back(std::move(kid));
        }
    }
    const std::string& s_;
    size_t i_ = 0;
};

// ─────────────────────────── 2. graph evaluation ─────────────────────────────

// A value flowing through the graph: a constant (1-4 components) or a float
// image (w*h*c, row-major, row 0 = top as decoded).
struct Val {
    bool isImg = false;
    int w = 0, h = 0, c = 0;
    std::vector<float> px;     // image texels
    std::vector<float> k;      // constant components
    bool valid() const { return isImg ? !px.empty() : !k.empty(); }
    int chans() const { return isImg ? c : (int)k.size(); }
    static Val constant(std::vector<float> v) { Val r; r.k = std::move(v); return r; }
};

float srgbToLin(float x) { return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f); }
float linToSrgb(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}
float halfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
    if (e == 0) f = m ? 0 : (s << 31);  // flush subnormals
    else if (e == 31) f = (s << 31) | 0x7f800000 | (m << 13);
    else f = (s << 31) | ((e + 112) << 23) | (m << 13);
    if (e == 0 && m) { float v = std::ldexp((float)m, -24); return s ? -v : v; }
    float out; std::memcpy(&out, &f, 4); return out;
}

// ── fallback TIFF reader ──────────────────────────────────────────────────────
// TinyDNG (tinyusdz's TIFF path) rejects two variants real OpenPBR content
// ships: Deflate (compression 8 / 32946) and LZW with the horizontal-difference
// predictor. This covers exactly baseline strip TIFFs -- chunky, 8/16-bit,
// uncompressed / LZW / Deflate, predictor 1 or 2 -- and nothing else; anything
// fancier still falls back to the neutral-value warning.
}  // namespace
extern "C" char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);  // stb_image
namespace {

bool lzwDecode(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    std::vector<std::vector<uint8_t>> dict;
    auto reset = [&] { dict.assign(258, {}); for (int i = 0; i < 256; ++i) dict[i] = {(uint8_t)i}; };
    reset();
    size_t bitpos = 0;
    int width = 9;
    int prev = -1;
    out.reserve(out.size() + expect);
    const size_t start = out.size();
    while (bitpos + width <= n * 8 && out.size() - start < expect) {
        uint32_t code = 0;
        for (int b = 0; b < width; ++b, ++bitpos)   // MSB-first, as TIFF writes LZW
            code = (code << 1) | ((in[bitpos >> 3] >> (7 - (bitpos & 7))) & 1);
        if (code == 257) break;                      // EOI
        if (code == 256) { reset(); width = 9; prev = -1; continue; }  // Clear
        std::vector<uint8_t> entry;
        if (code < dict.size()) entry = dict[code];
        else if (prev >= 0 && code == dict.size()) { entry = dict[(size_t)prev]; entry.push_back(entry[0]); }
        else return false;
        out.insert(out.end(), entry.begin(), entry.end());
        if (prev >= 0) { auto e = dict[(size_t)prev]; e.push_back(entry[0]); dict.push_back(std::move(e)); }
        prev = (int)code;
        // "early change": TIFF widens one code before the table fills
        if (dict.size() + 1 >= (1u << width) && width < 12) ++width;
    }
    return true;
}

// Decodes to raw interleaved samples (bits 8 or 16, little-endian host order).
bool readTiffFallback(const std::string& path, int& W, int& H, int& C, int& bits,
                      std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 8) return false;
    const bool le = b[0] == 'I';
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 > b.size() ? 0 : le ? (b[o] | b[o + 1] << 8) : (b[o] << 8 | b[o + 1]); };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > b.size()) return 0;
        return le ? (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24)
                  : ((uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
    };
    size_t ifd = u32(4);
    uint32_t nent = u16(ifd);
    uint32_t comp = 1, pred = 1, spp = 1, bps = 8, rps = 0, planar = 1;
    std::vector<uint32_t> offs, cnts;
    W = H = 0;
    auto values = [&](size_t e, std::vector<uint32_t>& v) {
        uint32_t type = u16(e + 2), cnt = u32(e + 4);
        size_t sz = type == 3 ? 2 : 4, p = cnt * sz <= 4 ? e + 8 : u32(e + 8);
        for (uint32_t i = 0; i < cnt; ++i) v.push_back(type == 3 ? u16(p + i * 2) : u32(p + i * 4));
    };
    for (uint32_t i = 0; i < nent; ++i) {
        size_t e = ifd + 2 + i * 12;
        uint32_t tag = u16(e);
        std::vector<uint32_t> v;
        values(e, v);
        if (v.empty()) continue;
        switch (tag) {
            case 256: W = (int)v[0]; break;
            case 257: H = (int)v[0]; break;
            case 258: bps = v[0]; break;
            case 259: comp = v[0]; break;
            case 273: offs = v; break;
            case 277: spp = v[0]; break;
            case 278: rps = v[0]; break;
            case 279: cnts = v; break;
            case 284: planar = v[0]; break;
            case 317: pred = v[0]; break;
        }
    }
    if (W <= 0 || H <= 0 || offs.empty() || offs.size() != cnts.size() || planar != 1 ||
        (bps != 8 && bps != 16) || (comp != 1 && comp != 5 && comp != 8 && comp != 32946))
        return false;
    if (rps == 0) rps = (uint32_t)H;
    C = (int)spp; bits = (int)bps;
    const size_t rowBytes = (size_t)W * spp * (bps / 8);
    data.clear();
    data.reserve(rowBytes * H);
    for (size_t s = 0; s < offs.size(); ++s) {
        const size_t rows = std::min<size_t>(rps, (size_t)H - s * rps);
        const size_t want = rows * rowBytes;
        if ((size_t)offs[s] + cnts[s] > b.size()) return false;
        const uint8_t* src = b.data() + offs[s];
        const size_t before = data.size();
        if (comp == 1) {
            data.insert(data.end(), src, src + std::min<size_t>(cnts[s], want));
        } else if (comp == 5) {
            if (!lzwDecode(src, cnts[s], data, want)) return false;
        } else {
            int n = 0;
            char* z = stbi_zlib_decode_malloc((const char*)src, (int)cnts[s], &n);
            if (!z) return false;
            data.insert(data.end(), (uint8_t*)z, (uint8_t*)z + std::min<size_t>((size_t)n, want));
            std::free(z);
        }
        data.resize(before + want, 0);
        if (pred == 2)   // horizontal differencing, per row, per sample
            for (size_t r = 0; r < rows; ++r) {
                uint8_t* row = data.data() + before + r * rowBytes;
                if (bps == 8) {
                    for (size_t x = spp; x < (size_t)W * spp; ++x) row[x] = (uint8_t)(row[x] + row[x - spp]);
                } else {
                    for (size_t x = spp; x < (size_t)W * spp; ++x) {
                        uint16_t a, p;
                        std::memcpy(&a, row + x * 2, 2); std::memcpy(&p, row + (x - spp) * 2, 2);
                        if (!le) { a = (uint16_t)(a >> 8 | a << 8); p = (uint16_t)(p >> 8 | p << 8); }
                        uint16_t v = (uint16_t)(a + p);
                        if (!le) v = (uint16_t)(v >> 8 | v << 8);
                        std::memcpy(row + x * 2, &v, 2);
                    }
                }
            }
    }
    if (bps == 16 && !le)   // to host (little-endian) order
        for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
    return true;
}

// Bilinear resample of an image to (W,H).
Val resample(const Val& a, int W, int H) {
    if (!a.isImg || (a.w == W && a.h == H)) return a;
    Val r; r.isImg = true; r.w = W; r.h = H; r.c = a.c;
    r.px.resize((size_t)W * H * a.c);
    for (int y = 0; y < H; ++y) {
        float fy = ((y + 0.5f) * a.h / H) - 0.5f;
        int y0 = std::clamp((int)std::floor(fy), 0, a.h - 1), y1 = std::min(y0 + 1, a.h - 1);
        float ty = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int x = 0; x < W; ++x) {
            float fx = ((x + 0.5f) * a.w / W) - 0.5f;
            int x0 = std::clamp((int)std::floor(fx), 0, a.w - 1), x1 = std::min(x0 + 1, a.w - 1);
            float tx = std::clamp(fx - x0, 0.0f, 1.0f);
            for (int ch = 0; ch < a.c; ++ch) {
                auto at = [&](int xx, int yy) { return a.px[((size_t)yy * a.w + xx) * a.c + ch]; };
                float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
                          (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                r.px[((size_t)y * W + x) * a.c + ch] = v;
            }
        }
    }
    return r;
}

// Component `ch` of `v` at texel i (constants broadcast, channels clamp).
inline float comp(const Val& v, size_t i, int ch) {
    if (v.isImg) return v.px[i * v.c + std::min(ch, v.c - 1)];
    return v.k.empty() ? 0.0f : v.k[std::min<size_t>(ch, v.k.size() - 1)];
}

// Element-wise combination of N inputs; result takes the largest image grid.
template <class F>
Val zipWith(const std::vector<const Val*>& in, int outC, F f) {
    int W = 0, H = 0;
    for (auto* v : in) if (v->isImg && (size_t)v->w * v->h > (size_t)W * H) { W = v->w; H = v->h; }
    std::vector<Val> rs;
    rs.reserve(in.size());
    for (auto* v : in) rs.push_back(v->isImg ? resample(*v, W, H) : *v);
    Val r;
    if (W == 0) {  // all constants
        r.k.resize(outC);
        std::vector<float> args(in.size());
        for (int ch = 0; ch < outC; ++ch) {
            for (size_t j = 0; j < rs.size(); ++j) args[j] = comp(rs[j], 0, ch);
            r.k[ch] = f(args, ch);
        }
        return r;
    }
    r.isImg = true; r.w = W; r.h = H; r.c = outC;
    r.px.resize((size_t)W * H * outC);
    std::vector<float> args(in.size());
    for (size_t i = 0; i < (size_t)W * H; ++i)
        for (int ch = 0; ch < outC; ++ch) {
            for (size_t j = 0; j < rs.size(); ++j) args[j] = comp(rs[j], i, ch);
            r.px[i * outC + ch] = f(args, ch);
        }
    return r;
}

class Graph {
public:
    Graph(const XElem& doc, fs::path dir, const MtlxOverrides& ov, const MtlxBakeParams& p,
          std::vector<std::string>& warn)
        : dir_(std::move(dir)), ov_(ov), p_(p), warn_(warn) {
        for (const XElem& e : doc.kids)
            if (const std::string* n = e.get("name")) nodes_[*n] = &e;
    }
    const XElem* node(const std::string& n) const {
        auto it = nodes_.find(n);
        return it == nodes_.end() ? nullptr : it->second;
    }

    // Input `name` of node `n`, USD-composed: a CONNECTION authored in the .mtlx
    // wins over a stronger layer's value (UsdShadeInput resolves connections
    // first); an unconnected input takes the override; else the authored value.
    // Last declaration wins, as in MaterialX (files do declare inputs twice).
    Val input(const XElem& n, const std::string& name) {
        const XElem* hit = nullptr;
        for (const XElem& k : n.kids)
            if (k.tag == "input") if (const std::string* nm = k.get("name")) if (*nm == name) hit = &k;
        if (hit) if (const std::string* src = hit->get("nodename")) return eval(*src);
        if (const std::string* nn = n.get("name")) {
            auto it = ov_.find(*nn);
            if (it != ov_.end()) {
                auto jt = it->second.find(name);
                if (jt != it->second.end() && !jt->second.isString) return Val::constant(jt->second.num);
            }
        }
        if (!hit) return Val{};
        const std::string* v = hit->get("value");
        if (!v) return Val{};
        const std::string* ty = hit->get("type");
        if (ty && *ty == "boolean") return Val::constant({(*v == "true") ? 1.0f : 0.0f});
        std::vector<float> nums;
        std::stringstream ss(*v);
        std::string tok;
        while (std::getline(ss, tok, ',')) { try { nums.push_back(std::stof(tok)); } catch (...) {} }
        return Val::constant(nums);
    }

    Val eval(const std::string& name) {
        auto c = cache_.find(name);
        if (c != cache_.end()) return c->second;
        const XElem* n = node(name);
        Val v;
        if (!n) { warn_.push_back("missing node '" + name + "'"); return v; }
        const std::string ty = n->get("type") ? *n->get("type") : "";
        const std::string& t = n->tag;
        if (t == "image") {
            v = image(*n, name, ty);
        } else if (t == "extract") {
            Val in = input(*n, "in");
            Val idx = input(*n, "index");
            int ch = idx.valid() ? (int)idx.k[0] : 0;
            if (in.isImg) {
                v.isImg = true; v.w = in.w; v.h = in.h; v.c = 1;
                v.px.resize((size_t)in.w * in.h);
                for (size_t i = 0; i < v.px.size(); ++i) v.px[i] = comp(in, i, ch);
            } else if (in.valid()) {
                v = Val::constant({comp(in, 0, ch)});
            }
        } else if (t == "invert") {
            Val in = input(*n, "in"), amt = input(*n, "amount");
            if (!amt.valid()) amt = Val::constant({1.0f});
            v = zipWith({&amt, &in}, std::max(1, in.chans()),
                        [](const std::vector<float>& a, int) { return a[0] - a[1]; });
        } else if (t == "combine3") {
            Val a = input(*n, "in1"), b = input(*n, "in2"), c3 = input(*n, "in3");
            v = zipWith({&a, &b, &c3}, 3, [](const std::vector<float>& x, int ch) { return x[ch]; });
        } else if (t == "normalmap") {
            // Tangent-space, [0,1]-encoded, +Y up: already glTF's convention.
            v = input(*n, "in");
            Val s = input(*n, "scale");
            if (s.valid() && std::fabs(s.k[0] - 1.0f) > 1e-6f)
                warn_.push_back(name + ": normalmap scale ignored");
        } else if (t == "remap") {
            Val in = input(*n, "in"), il = input(*n, "inlow"), ih = input(*n, "inhigh"),
                ol = input(*n, "outlow"), oh = input(*n, "outhigh");
            if (!il.valid()) il = Val::constant({0}); if (!ih.valid()) ih = Val::constant({1});
            if (!ol.valid()) ol = Val::constant({0}); if (!oh.valid()) oh = Val::constant({1});
            v = zipWith({&in, &il, &ih, &ol, &oh}, std::max(1, in.chans()),
                        [](const std::vector<float>& x, int) {
                            float d = x[2] - x[1];
                            return x[3] + (d != 0 ? (x[0] - x[1]) / d : 0.0f) * (x[4] - x[3]);
                        });
        } else if (t == "multiply") {
            Val a = input(*n, "in1"), b = input(*n, "in2");
            v = zipWith({&a, &b}, std::max(a.chans(), b.chans()),
                        [](const std::vector<float>& x, int) { return x[0] * x[1]; });
        } else {
            v = input(*n, "in");
            warn_.push_back(name + ": <" + t + "> not evaluated - " +
                            (v.valid() ? "passing its 'in' through" : "input treated as unset"));
        }
        cache_[name] = v;
        return v;
    }

private:
    // File for an image node: a USD override wins (it is an asset path, not a
    // connection); `<UDIM>` resolves to this instance's tile, else the lowest.
    fs::path imagePath(const XElem& n, const std::string& nodeName) {
        std::string rel;
        auto it = ov_.find(nodeName);
        if (it != ov_.end()) {
            auto jt = it->second.find("file");
            if (jt != it->second.end() && jt->second.isString) rel = jt->second.str;
        }
        if (rel.empty())
            for (const XElem& k : n.kids)
                if (k.tag == "input" && k.get("name") && *k.get("name") == "file" && k.get("value"))
                    rel = *k.get("value");
        size_t u = rel.find("<UDIM>");
        if (u == std::string::npos) return dir_ / rel;
        std::string want = rel;
        want.replace(u, 6, std::to_string(p_.udimTile));
        if (fs::exists(dir_ / want)) return dir_ / want;
        // fall back to the lowest tile present
        fs::path pat = dir_ / rel;
        std::string pre = pat.filename().string().substr(0, pat.filename().string().find("<UDIM>"));
        std::string post = pat.filename().string().substr(pat.filename().string().find("<UDIM>") + 6);
        std::string best;
        std::error_code ec;
        for (auto& de : fs::directory_iterator(pat.parent_path(), ec)) {
            std::string f = de.path().filename().string();
            if (f.size() == pre.size() + 4 + post.size() && f.compare(0, pre.size(), pre) == 0 &&
                f.compare(pre.size() + 4, post.size(), post) == 0 && (best.empty() || f < best))
                best = f;
        }
        if (!best.empty()) {
            warn_.push_back(nodeName + ": no tile " + std::to_string(p_.udimTile) + " - using " + best);
            return pat.parent_path() / best;
        }
        return dir_ / want;
    }

    Val image(const XElem& n, const std::string& name, const std::string& type) {
        fs::path path = imagePath(n, name);
        std::string cs;
        for (const XElem& k : n.kids)
            if (k.tag == "input" && k.get("name") && *k.get("name") == "file" && k.get("colorspace"))
                cs = *k.get("colorspace");
        const int wantC = (type == "float") ? 1 : 3;
        auto r = tinyusdz::image::LoadImageFromFile(path.string());
        tinyusdz::Image im;
        if (r) {
            im = std::move(r.value().image);
        } else {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            int fw = 0, fh = 0, fc = 0, fb = 0;
            if ((ext == ".tif" || ext == ".tiff") &&
                readTiffFallback(path.string(), fw, fh, fc, fb, im.data)) {
                im.width = fw; im.height = fh; im.channels = fc; im.bpp = fb;
                im.format = tinyusdz::Image::PixelFormat::UInt;
            } else {
                warn_.push_back(name + ": cannot decode " + path.filename().string() + " (" +
                                r.error().substr(0, 120) + ") - using 0.5");
                return Val::constant(std::vector<float>(wantC, 0.5f));
            }
        }
        const int W = im.width, H = im.height, C = im.channels, bits = im.bpp;
        const bool isFloat = im.format == tinyusdz::Image::PixelFormat::Float;
        // Box-downsample by an integer factor to the texture budget as we convert.
        int f = 1;
        while (std::max(W, H) / f > p_.maxTexture) f *= 2;
        Val v; v.isImg = true; v.w = std::max(1, W / f); v.h = std::max(1, H / f); v.c = wantC;
        v.px.assign((size_t)v.w * v.h * wantC, 0.0f);
        auto texel = [&](size_t idx, int ch) -> float {
            const uint8_t* b = im.data.data();
            size_t o = idx * C + std::min(ch, C - 1);
            if (isFloat) {
                if (bits == 16) { uint16_t h; std::memcpy(&h, b + o * 2, 2); return halfToFloat(h); }
                float x; std::memcpy(&x, b + o * 4, 4); return x;
            }
            if (bits == 16) { uint16_t x; std::memcpy(&x, b + o * 2, 2); return x / 65535.0f; }
            if (bits == 32) { uint32_t x; std::memcpy(&x, b + o * 4, 4); return (float)(x / 4294967295.0); }
            return b[o] / 255.0f;
        };
        const size_t need = (size_t)W * H * C * (size_t)std::max(1, bits / 8);
        if (im.data.size() < need || W <= 0 || H <= 0 || C <= 0) {
            warn_.push_back(name + ": short image buffer - using 0.5");
            return Val::constant(std::vector<float>(wantC, 0.5f));
        }
        const bool srgb = cs == "srgb_tx" || cs == "srgb_texture";
        for (int y = 0; y < v.h; ++y)
            for (int x = 0; x < v.w; ++x)
                for (int ch = 0; ch < wantC; ++ch) {
                    float acc = 0;
                    for (int dy = 0; dy < f; ++dy)
                        for (int dx = 0; dx < f; ++dx) {
                            int sx = std::min(W - 1, x * f + dx), sy = std::min(H - 1, y * f + dy);
                            float t = texel((size_t)sy * W + sx, ch);
                            acc += srgb ? srgbToLin(t) : t;   // average in linear light
                        }
                    v.px[((size_t)y * v.w + x) * wantC + ch] = acc / (f * f);
                }
        return v;
    }

    fs::path dir_;
    const MtlxOverrides& ov_;
    const MtlxBakeParams& p_;
    std::vector<std::string>& warn_;
    std::map<std::string, const XElem*> nodes_;
    std::map<std::string, Val> cache_;
};

// ──────────────────────────── 3. OpenPBR bake ────────────────────────────────

// OpenPBR 1.1 defaults for every input this bake reads.
const std::map<std::string, std::vector<float>>& defaults() {
    static const std::map<std::string, std::vector<float>> d = {
        {"base_weight", {1}}, {"base_color", {0.8f, 0.8f, 0.8f}}, {"base_metalness", {0}},
        {"base_diffuse_roughness", {0}}, {"specular_weight", {1}}, {"specular_color", {1, 1, 1}},
        {"specular_roughness", {0.3f}}, {"specular_ior", {1.5f}},
        {"specular_roughness_anisotropy", {0}}, {"transmission_weight", {0}},
        {"transmission_color", {1, 1, 1}}, {"transmission_depth", {0}},
        {"subsurface_weight", {0}}, {"subsurface_color", {0.8f, 0.8f, 0.8f}},
        {"subsurface_radius", {1}}, {"subsurface_radius_scale", {1, 0.5f, 0.25f}},
        {"subsurface_scatter_anisotropy", {0}}, {"coat_weight", {0}}, {"coat_color", {1, 1, 1}},
        {"coat_roughness", {0}}, {"coat_ior", {1.6f}}, {"coat_darkening", {1}},
        {"fuzz_weight", {0}}, {"fuzz_color", {1, 1, 1}}, {"fuzz_roughness", {0.5f}},
        {"thin_film_weight", {0}}, {"thin_film_thickness", {0.5f}}, {"thin_film_ior", {1.4f}},
        {"emission_luminance", {0}}, {"emission_color", {1, 1, 1}}, {"geometry_opacity", {1}},
        {"geometry_thin_walled", {0}}};
    return d;
}

float mean(const Val& v, int ch = 0) {
    if (!v.isImg) return comp(v, 0, ch);
    double s = 0;
    size_t n = (size_t)v.w * v.h;
    for (size_t i = 0; i < n; ++i) s += comp(v, i, ch);
    return n ? (float)(s / n) : 0.0f;
}
void vec3(const Val& v, float o[3]) { for (int c = 0; c < 3; ++c) o[c] = mean(v, c); }

// Pack a Val into an RGBA8 texture. `rgb` channels come from `v` (sRGB-encoded
// when `srgb`), `alpha` from `a` if given. Returns its index in out.textures.
int addTexture(ModelData& out, const Val& v, bool srgb, const Val* a = nullptr,
               const int chanMap[4] = nullptr) {
    int W = v.isImg ? v.w : 1, H = v.isImg ? v.h : 1;
    Val va;
    if (a && a->isImg && (!v.isImg || (size_t)a->w * a->h > (size_t)W * H)) { W = a->w; H = a->h; }
    Val vv = v.isImg ? resample(v, W, H) : v;
    if (a) va = a->isImg ? resample(*a, W, H) : *a;
    ModelTexture t; t.width = W; t.height = H;
    t.rgba.resize((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; ++i)
        for (int ch = 0; ch < 4; ++ch) {
            float x;
            if (chanMap) {
                x = chanMap[ch] < 0 ? (chanMap[ch] == -1 ? 0.0f : 1.0f) : comp(vv, i, chanMap[ch]);
            } else if (ch < 3) {
                x = comp(vv, i, ch);
                if (srgb) x = linToSrgb(x);
            } else {
                x = a ? comp(va, i, 0) : 1.0f;
            }
            t.rgba[i * 4 + ch] = (uint8_t)std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f);
        }
    out.textures.push_back(std::move(t));
    return (int)out.textures.size() - 1;
}

}  // namespace

bool mtlx_bake_openpbr(const std::string& mtlxPath, const std::string& materialName,
                       const MtlxOverrides& ov, const MtlxBakeParams& p, ModelData& out,
                       ModelMaterial& mat, std::vector<std::string>& warn) {
    std::ifstream f(mtlxPath, std::ios::binary);
    if (!f) { warn.push_back("cannot open " + mtlxPath); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    XElem doc;
    if (!XmlReader(text).parse(doc) || doc.tag != "materialx") {
        warn.push_back("not a MaterialX document: " + mtlxPath);
        return false;
    }
    Graph g(doc, fs::path(mtlxPath).parent_path(), ov, p, warn);

    // surfacematerial `materialName` (or the first) → its surfaceshader node.
    const XElem* surf = nullptr;
    for (const XElem& e : doc.kids) {
        if (e.tag != "surfacematerial") continue;
        if (!materialName.empty() && e.get("name") && *e.get("name") != materialName && surf) continue;
        for (const XElem& k : e.kids)
            if (k.tag == "input" && k.get("name") && *k.get("name") == "surfaceshader" && k.get("nodename"))
                if (const XElem* s = g.node(*k.get("nodename"))) surf = s;
        if (surf && e.get("name") && *e.get("name") == materialName) break;
    }
    if (!surf || surf->tag != "open_pbr_surface") {
        warn.push_back(mtlxPath + ": no open_pbr_surface reached from '" + materialName + "'");
        return false;
    }
    const std::string surfName = surf->get("name") ? *surf->get("name") : "";

    std::set<std::string> authored;
    for (const XElem& k : surf->kids)
        if (k.tag == "input" && k.get("name")) authored.insert(*k.get("name"));
    if (auto it = ov.find(surfName); it != ov.end())
        for (auto& kv : it->second) authored.insert(kv.first);
    for (const std::string& n : authored)
        if (!defaults().count(n) && n != "geometry_normal")
            warn.push_back(materialName + ": OpenPBR input '" + n + "' has no mapping here - dropped");

    auto get = [&](const std::string& n) -> Val {
        Val v = authored.count(n) ? g.input(*surf, n) : Val{};
        return v.valid() ? v : Val::constant(defaults().at(n));
    };
    auto isImg = [](const Val& v) { return v.isImg; };

    // base colour (premultiplied by base_weight); on a transmissive surface glTF
    // tints transmitted light by baseColor where OpenPBR uses transmission_color,
    // so bake mix(base_color, transmission_color, transmission_weight) there.
    Val bw = get("base_weight"), bcol = get("base_color");
    Val bc = zipWith({&bcol, &bw}, 3, [](const std::vector<float>& x, int) { return x[0] * x[1]; });
    Val tw = get("transmission_weight"), sw = get("subsurface_weight");
    const bool noSss = !isImg(sw) && mean(sw) == 0.0f;
    if ((isImg(tw) || mean(tw) > 0) && noSss) {
        Val tc = get("transmission_color");
        bc = zipWith({&bc, &tc, &tw}, 3,
                     [](const std::vector<float>& x, int) { return x[0] * (1 - x[2]) + x[1] * x[2]; });
    }
    Val op = get("geometry_opacity");
    if (isImg(bc) || isImg(op)) {
        mat.baseColorTex = addTexture(out, bc, /*srgb*/ true, &op);
        mat.baseColorFactor[0] = mat.baseColorFactor[1] = mat.baseColorFactor[2] = 1.0f;
        mat.baseColorFactor[3] = isImg(op) ? 1.0f : mean(op);
    } else {
        vec3(bc, mat.baseColorFactor);
        mat.baseColorFactor[3] = mean(op);
    }

    // metallic / roughness → one MR texture (G = roughness, B = metalness)
    Val r = get("specular_roughness"), m = get("base_metalness");
    if (isImg(r) || isImg(m)) {
        Val rm = zipWith({&r, &m}, 2, [](const std::vector<float>& x, int ch) { return x[ch]; });
        static const int map[4] = {-1, 0, 1, -2};  // R=0, G=rough, B=metal, A=1
        mat.metallicRoughnessTex = addTexture(out, rm, false, nullptr, map);
        mat.roughness = 1.0f; mat.metallic = 1.0f;
    } else {
        mat.roughness = mean(r); mat.metallic = mean(m);
    }

    // normal
    if (authored.count("geometry_normal")) {
        Val n = g.input(*surf, "geometry_normal");
        if (n.isImg) mat.normalTex = addTexture(out, n, false);
    }

    // specular / ior / anisotropy / diffuse roughness
    Val spw = get("specular_weight"), spc = get("specular_color");
    if (isImg(spw) || isImg(spc)) warn.push_back(materialName + ": textured specular weight/colour - using mean");
    mat.specularFactor = mean(spw);
    vec3(spc, mat.specularColorFactor);
    mat.ior = mean(get("specular_ior"));
    mat.anisotropyStrength = mean(get("specular_roughness_anisotropy"));
    mat.diffuseRoughness = mean(get("base_diffuse_roughness"));

    // coat
    Val cw = get("coat_weight");
    if (isImg(cw) || mean(cw) > 0) {
        mat.hasCoat = true;
        mat.clearcoatRoughness = mean(get("coat_roughness"));
        mat.coatIor = mean(get("coat_ior"));
        vec3(get("coat_color"), mat.coatColor);
        mat.coatDarkening = mean(get("coat_darkening"));
        if (isImg(cw)) { mat.clearcoatFactor = 1.0f; mat.clearcoatTex = addTexture(out, cw, false); }
        else mat.clearcoatFactor = mean(cw);
    }

    // fuzz (the renderer carries fuzz colour/roughness in the sheen lanes)
    Val fw = get("fuzz_weight");
    if (isImg(fw) || mean(fw) > 0) {
        mat.hasFuzz = true;
        vec3(get("fuzz_color"), mat.sheenColorFactor);
        mat.sheenRoughness = mean(get("fuzz_roughness"));
        if (isImg(fw)) { mat.fuzzFactor = 1.0f; mat.fuzzTex = addTexture(out, fw, false); }
        else mat.fuzzFactor = mean(fw);
    }

    // transmission + subsurface: both replace the diffuse lobe,
    //   mix(mix(diffuse, sss, sw), transmission, tw)
    // so the transmission weight is their union and scatter carries the sss share.
    int twTex = -1;
    float twv;
    if (isImg(tw) && noSss) { twTex = addTexture(out, tw, false); twv = 1.0f; }
    else {
        if (isImg(tw) || isImg(sw)) warn.push_back(materialName + ": textured transmission/subsurface weight - using mean");
        twv = mean(tw);
    }
    const float swv = mean(sw);
    const float total = twv + (1 - twv) * swv;
    const bool thin = mean(get("geometry_thin_walled")) > 0.5f;
    if (total > 0) {
        mat.transmissionFactor = total;
        mat.transmissionTex = twTex;
        mat.volumeThickness = thin ? 0.0f : p.thicknessMeters;
        const float depth = mean(get("transmission_depth"));
        if (swv > 0) {
            Val rs = get("subsurface_radius_scale");
            const float rad = mean(get("subsurface_radius")) *
                              (mean(rs, 0) + mean(rs, 1) + mean(rs, 2)) / 3.0f;
            mat.scatterStrength = (1 - twv) * swv / total;
            vec3(get("subsurface_color"), mat.multiscatterColor);
            mat.scatterAnisotropy = mean(get("subsurface_scatter_anisotropy"));
            vec3(get("subsurface_color"), mat.attenuationColor);
            mat.attenuationDistance = std::max(rad * p.metersPerUnit, 1e-5f);
        } else if (depth > 0) {
            vec3(get("transmission_color"), mat.attenuationColor);
            mat.attenuationDistance = depth * p.metersPerUnit;
        }
    }

    // thin film: OpenPBR micrometres → nanometres
    const float tfw = mean(get("thin_film_weight"));
    if (tfw > 0) {
        mat.iridescenceFactor = tfw;
        mat.iridescenceIor = mean(get("thin_film_ior"));
        mat.iridescenceThicknessMin = mat.iridescenceThicknessMax = mean(get("thin_film_thickness")) * 1000.0f;
    } else if (authored.count("thin_film_thickness") || authored.count("thin_film_ior")) {
        warn.push_back(materialName + ": thin_film_* authored but thin_film_weight is 0 - film is OFF in OpenPBR too");
    }

    // emission (luminance mapped 1:1 by default; see header)
    Val L = get("emission_luminance"), C = get("emission_color");
    if (isImg(L) || mean(L) > 0) {
        if (isImg(L) || isImg(C)) {
            Val LC = zipWith({&C, &L}, 3, [](const std::vector<float>& x, int) { return std::max(0.0f, x[0] * x[1]); });
            float peak = 0;
            for (float x : LC.px) peak = std::max(peak, x);
            if (peak <= 0) peak = 1;
            Val norm = zipWith({&LC}, 3, [peak](const std::vector<float>& x, int) { return x[0] / peak; });
            mat.emissiveTex = addTexture(out, norm, true);
            mat.emissive[0] = mat.emissive[1] = mat.emissive[2] = 1.0f;
            mat.emissiveStrength = peak / p.nitsPerUnit;
        } else {
            vec3(C, mat.emissive);
            mat.emissiveStrength = mean(L) / p.nitsPerUnit;
        }
    }
    return true;
}

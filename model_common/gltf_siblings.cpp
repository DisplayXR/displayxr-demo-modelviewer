// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  gltf_siblings.h implementation — pure, platform-neutral.
 *
 * The JSON parser is nlohmann/json, which tinygltf already bundles (json.hpp
 * sits next to tiny_gltf.h in the FetchContent'd source dir). We deliberately
 * do NOT hand the document to tinygltf here: tinygltf resolves and LOADS
 * external buffers/images while parsing, which is exactly the step that must
 * wait until the siblings are on disk.
 */

#include "gltf_siblings.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace gltf_siblings {

namespace {

constexpr size_t kMaxUriLength = 1024;
constexpr size_t kMaxSegments = 32;

bool
IsControl(unsigned char c)
{
    return c < 0x20 || c == 0x7f;
}

char
LowerAscii(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string
Lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = LowerAscii(c);
    return out;
}

//! Does `s` start with a URI scheme ("http:", "data:", and also "C:")? RFC 3986
//! scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":".
bool
HasScheme(const std::string& s)
{
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ':') return true;
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return false;
}

//! Case-insensitive "does `hay` contain `needle`" (needle already lower-case).
bool
ContainsCi(const std::string& hay, const char* needle)
{
    const std::string low = Lower(hay);
    return low.find(needle) != std::string::npos;
}

void
SplitSegments(const std::string& path, std::vector<std::string>* out)
{
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            out->push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out->push_back(cur);
}

//! Everything a validated relative reference must still satisfy after the
//! percent-decoding pass (the encoded form can hide `..`, `\` and `/`).
bool
DecodedShapeOk(const std::string& s, std::string* reason)
{
    auto fail = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (s.empty()) return fail("empty");
    if (s.front() == '/') return fail("absolute path");
    if (s.back() == '/') return fail("directory, not a file");
    if (HasScheme(s)) return fail("has a scheme (absolute URI or drive letter)");
    for (char c : s) {
        if (IsControl(static_cast<unsigned char>(c))) return fail("control character");
        if (c == '\\') return fail("backslash");
        if (c == '?' || c == '#') return fail("query or fragment");
        if (c == ':') return fail("colon");
    }
    std::vector<std::string> segs;
    SplitSegments(s, &segs);
    if (segs.size() > kMaxSegments) return fail("too many path segments");
    size_t kept = 0;
    for (const std::string& seg : segs) {
        if (seg == "..") return fail("parent-directory segment");
        if (seg.empty()) return fail("empty path segment");
        if (seg == ".") continue;
        ++kept;
    }
    if (kept == 0) return fail("resolves to nothing");
    return true;
}

} // namespace

bool
percent_decode(const std::string& in, std::string* out)
{
    std::string result;
    result.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            result.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        const char hi = in[i + 1], lo = in[i + 2];
        if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
            !std::isxdigit(static_cast<unsigned char>(lo)))
            return false;
        auto val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            return LowerAscii(c) - 'a' + 10;
        };
        const int byte = val(hi) * 16 + val(lo);
        if (byte == 0) return false; // an escaped NUL truncates every path API
        result.push_back(static_cast<char>(byte));
        i += 2;
    }
    if (out) *out = result;
    return true;
}

bool
uri_is_safe_relative(const std::string& uri, std::string* reason)
{
    auto fail = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (reason) reason->clear();

    if (uri.empty()) return fail("empty");
    if (uri.size() > kMaxUriLength) return fail("too long");
    // Reject the ENCODED form first: `%2e%2e/x` decodes to `../x`, and
    // `%2f` to a separator. Both are refused outright rather than normalised.
    if (ContainsCi(uri, "%2e")) return fail("percent-encoded dot");
    if (ContainsCi(uri, "%2f")) return fail("percent-encoded slash");
    if (ContainsCi(uri, "%5c")) return fail("percent-encoded backslash");
    for (char c : uri) {
        if (IsControl(static_cast<unsigned char>(c))) return fail("control character");
        if (c == '\\') return fail("backslash");
        if (c == '?' || c == '#') return fail("query or fragment");
    }
    if (uri.compare(0, 2, "//") == 0) return fail("protocol-relative URL");
    if (uri.front() == '/') return fail("absolute path");
    if (HasScheme(uri)) return fail("has a scheme (absolute URI or drive letter)");

    std::string decoded;
    if (!percent_decode(uri, &decoded)) return fail("malformed percent-escape");
    return DecodedShapeOk(decoded, reason);
}

bool
relative_path_for(const std::string& uri, std::string* out)
{
    std::string reason;
    if (!uri_is_safe_relative(uri, &reason)) return false;
    std::string decoded;
    if (!percent_decode(uri, &decoded)) return false;
    std::vector<std::string> segs;
    SplitSegments(decoded, &segs);
    std::string path;
    for (const std::string& seg : segs) {
        if (seg == ".") continue;
        if (!path.empty()) path.push_back('/');
        path += seg;
    }
    if (path.empty()) return false;
    if (out) *out = path;
    return true;
}

bool
resolve_relative_url(const std::string& base, const std::string& rel, std::string* out)
{
    if (base.empty() || rel.empty()) return false;
    std::string reason;
    if (!uri_is_safe_relative(rel, &reason)) return false;

    // Drop the base's query/fragment, then split scheme://authority | path.
    std::string b = base.substr(0, base.find_first_of("?#"));
    const size_t schemeEnd = b.find("://");
    if (schemeEnd == std::string::npos) return false;
    const size_t authStart = schemeEnd + 3;
    const size_t pathStart = b.find('/', authStart);
    const std::string prefix = (pathStart == std::string::npos) ? b : b.substr(0, pathStart);
    const std::string path = (pathStart == std::string::npos) ? "/" : b.substr(pathStart);
    const size_t lastSlash = path.find_last_of('/');
    const std::string dir = path.substr(0, lastSlash + 1); // keeps the trailing '/'

    // `rel` is a plain relative path (validated) — merge, then drop '.' segments.
    std::vector<std::string> segs;
    SplitSegments(rel, &segs);
    std::string merged = dir;
    for (const std::string& seg : segs) {
        if (seg == ".") continue;
        if (!merged.empty() && merged.back() != '/') merged.push_back('/');
        merged += seg;
    }
    if (out) *out = prefix + merged;
    return true;
}

bool
same_origin(const std::string& a, const std::string& b)
{
    auto origin = [](const std::string& u, std::string* scheme, std::string* host,
                     int* port) -> bool {
        const size_t schemeEnd = u.find("://");
        if (schemeEnd == std::string::npos) return false;
        *scheme = Lower(u.substr(0, schemeEnd));
        const size_t authStart = schemeEnd + 3;
        size_t authEnd = u.find_first_of("/?#", authStart);
        if (authEnd == std::string::npos) authEnd = u.size();
        std::string auth = u.substr(authStart, authEnd - authStart);
        // userinfo is not part of an origin; refuse rather than strip it (a
        // `user@evil.example` prefix is exactly the shape used to fool eyes).
        if (auth.find('@') != std::string::npos) return false;
        *port = (*scheme == "https") ? 443 : (*scheme == "http" ? 80 : -1);
        const size_t colon = auth.find_last_of(':');
        if (colon != std::string::npos && auth.find(']', colon) == std::string::npos) {
            const std::string p = auth.substr(colon + 1);
            if (p.empty()) return false;
            for (char c : p)
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            *port = std::atoi(p.c_str());
            auth.resize(colon);
        }
        *host = Lower(auth);
        return !host->empty();
    };
    std::string sa, ha, sb, hb;
    int pa = 0, pb = 0;
    if (!origin(a, &sa, &ha, &pa) || !origin(b, &sb, &hb, &pb)) return false;
    return sa == sb && ha == hb && pa == pb;
}

std::string
path_extension(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    const std::string last = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = last.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= last.size()) return {};
    return Lower(last.substr(dot));
}

bool
collect_external_refs(const std::string& json, const std::string& baseUrl,
                      std::vector<SiblingRef>* out, std::string* err)
{
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (err) err->clear();

    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return fail("not valid JSON");
    if (!doc.is_object()) return fail("glTF root is not an object");

    std::vector<SiblingRef> refs;
    auto scan = [&](const char* arrayName) -> bool {
        const auto it = doc.find(arrayName);
        if (it == doc.end() || !it->is_array()) return true;
        for (const auto& entry : *it) {
            if (!entry.is_object()) continue;
            const auto u = entry.find("uri");
            if (u == entry.end() || !u->is_string()) continue; // GLB chunk / bufferView image
            const std::string uri = u->get<std::string>();
            if (uri.empty()) continue;
            // Embedded payload: nothing to fetch.
            if (Lower(uri).compare(0, 5, "data:") == 0) continue;
            std::string reason;
            if (!uri_is_safe_relative(uri, &reason))
                return fail(std::string(arrayName) + " uri '" + uri + "' refused: " + reason);
            SiblingRef ref;
            ref.uri = uri;
            if (!relative_path_for(uri, &ref.relativePath))
                return fail(std::string(arrayName) + " uri '" + uri + "' has no usable path");
            if (!resolve_relative_url(baseUrl, uri, &ref.url))
                return fail(std::string(arrayName) + " uri '" + uri +
                            "' could not be resolved against the asset URL");
            if (!same_origin(ref.url, baseUrl))
                return fail(std::string(arrayName) + " uri '" + uri + "' is not same-origin");
            const bool dup =
                std::any_of(refs.begin(), refs.end(),
                            [&](const SiblingRef& r) { return r.relativePath == ref.relativePath; });
            if (!dup) refs.push_back(ref);
        }
        return true;
    };
    if (!scan("buffers")) return false;
    if (!scan("images")) return false;
    if (out) *out = refs;
    return true;
}

} // namespace gltf_siblings

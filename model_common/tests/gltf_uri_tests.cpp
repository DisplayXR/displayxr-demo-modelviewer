// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit tests for gltf_siblings.h — the .gltf sibling-URI validator.
 *
 * The validator is the security boundary of the multi-file glTF fetch (#114):
 * a hostile `.gltf` served from an allowed origin must not be able to make the
 * viewer write outside the asset directory or fetch from anywhere else. That
 * is exactly the property a live download cannot demonstrate, so it is tested
 * here directly. No framework: one console exe, exit 0 = every case passed.
 */

#include "gltf_siblings.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void
Check(bool cond, const std::string& what)
{
    ++g_checks;
    if (cond) {
        std::printf("  ok    %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void
ExpectRejected(const char* uri)
{
    std::string reason;
    const bool safe = gltf_siblings::uri_is_safe_relative(uri, &reason);
    Check(!safe, std::string("rejected: \"") + uri + "\"" +
                     (safe ? "" : "   [" + reason + "]"));
}

void
ExpectAccepted(const char* uri, const char* expectedPath)
{
    std::string reason;
    const bool safe = gltf_siblings::uri_is_safe_relative(uri, &reason);
    Check(safe, std::string("accepted: \"") + uri + "\"" +
                    (safe ? "" : "   [rejected: " + reason + "]"));
    if (!safe) return;
    std::string path;
    const bool ok = gltf_siblings::relative_path_for(uri, &path);
    Check(ok && path == expectedPath,
          std::string("    -> path \"") + path + "\" (want \"" + expectedPath + "\")");
}

} // namespace

int
main()
{
    std::printf("gltf sibling URI validator\n");

    // ── Must be refused ──────────────────────────────────────────────────
    ExpectRejected("../x.bin");             // parent escape
    ExpectRejected("/etc/x");               // absolute path
    ExpectRejected("https://evil/x.bin");   // off-origin absolute URL
    ExpectRejected("C:\\x.bin");            // Windows drive letter
    ExpectRejected("a\\b.bin");             // backslash separator
    ExpectRejected("%2e%2e/x");             // percent-encoded parent escape
    ExpectRejected("%2E%2E/x");             // ... case-insensitively
    ExpectRejected("//evil.example/x.bin"); // protocol-relative
    ExpectRejected("textures/../../x.bin"); // parent escape mid-path
    ExpectRejected("textures%2f..%2fx.bin");// percent-encoded separator
    ExpectRejected("tex\ntures/a.png");     // control character
    ExpectRejected("a.png?token=1");        // query
    ExpectRejected("a.png#frag");           // fragment
    ExpectRejected("textures/");            // directory
    ExpectRejected("");                     // empty
    ExpectRejected("a//b.bin");             // empty segment
    ExpectRejected("a%00.bin");             // escaped NUL
    ExpectRejected("a%zz.bin");             // malformed escape
    ExpectRejected("file:///c:/x.bin");     // scheme

    // ── Must be accepted ─────────────────────────────────────────────────
    ExpectAccepted("textures/albedo.png", "textures/albedo.png");
    ExpectAccepted("./mesh.bin", "mesh.bin");
    ExpectAccepted("rubber_boots.bin", "rubber_boots.bin");
    ExpectAccepted("a/b/c/d.jpg", "a/b/c/d.jpg");
    ExpectAccepted("my%20texture.png", "my texture.png"); // percent-decoded

    // ── Relative resolution against the .gltf's URL ───────────────────────
    std::printf("relative resolution\n");
    const std::string base =
        "http://localhost:8123/assets/models/rubber_boots/rubber_boots_1k.gltf";
    // NB: build the label from a LOCAL, never from the out-param in the same
    // expression — argument evaluation order is unspecified, and a label that
    // prints the previous call's value is worse than no label.
    auto resolved = [&](const std::string& b, const char* rel) {
        std::string url;
        return gltf_siblings::resolve_relative_url(b, rel, &url) ? url : std::string("<refused>");
    };
    std::string url = resolved(base, "rubber_boots.bin");
    Check(url == "http://localhost:8123/assets/models/rubber_boots/rubber_boots.bin",
          "  bin -> " + url);
    url = resolved(base, "textures/a.jpg");
    Check(url == "http://localhost:8123/assets/models/rubber_boots/textures/a.jpg",
          "  texture -> " + url);
    url = resolved(base + "?v=2", "./a.bin");
    Check(url == "http://localhost:8123/assets/models/rubber_boots/a.bin",
          "  base query dropped -> " + url);
    url = resolved(base, "../a.bin");
    Check(url == "<refused>", "  refuses to resolve an unsafe reference");

    // ── Same-origin ──────────────────────────────────────────────────────
    std::printf("same origin\n");
    Check(gltf_siblings::same_origin("http://localhost:8123/a", "http://localhost:8123/b/c"),
          "  same host+port");
    Check(!gltf_siblings::same_origin("http://localhost:8123/a", "http://localhost:9000/a"),
          "  different port");
    Check(!gltf_siblings::same_origin("http://localhost:8123/a", "https://localhost:8123/a"),
          "  different scheme");
    Check(!gltf_siblings::same_origin("https://a.example/x", "https://b.example/x"),
          "  different host");
    Check(gltf_siblings::same_origin("https://A.Example/x", "https://a.example:443/y"),
          "  default port + case implied");
    Check(!gltf_siblings::same_origin("https://a.example@evil.example/x", "https://a.example/x"),
          "  userinfo refused");

    // ── Whole-document collection ────────────────────────────────────────
    std::printf("collection\n");
    const std::string json = R"({
      "buffers": [{"uri": "rubber_boots.bin", "byteLength": 4},
                  {"uri": "data:application/octet-stream;base64,AAAA"}],
      "images":  [{"uri": "textures/a.jpg"}, {"uri": "textures/a.jpg"},
                  {"bufferView": 0, "mimeType": "image/png"}]
    })";
    std::vector<gltf_siblings::SiblingRef> refs;
    std::string err;
    const bool parsed = gltf_siblings::collect_external_refs(json, base, &refs, &err);
    Check(parsed, "  parses" + (parsed ? std::string() : " (" + err + ")"));
    Check(refs.size() == 2, "  2 refs (data: skipped, duplicate deduped, bufferView image "
                            "skipped) — got " + std::to_string(refs.size()));
    if (refs.size() == 2) {
        Check(refs[0].relativePath == "rubber_boots.bin" &&
                  refs[0].url ==
                      "http://localhost:8123/assets/models/rubber_boots/rubber_boots.bin",
              "  [0] " + refs[0].url);
        Check(refs[1].relativePath == "textures/a.jpg", "  [1] " + refs[1].relativePath);
    }

    const std::string hostile = R"({"images": [{"uri": "../../../../etc/passwd"}]})";
    refs.clear();
    const bool hostileOk = gltf_siblings::collect_external_refs(hostile, base, &refs, &err);
    Check(!hostileOk, "  hostile document refused: " + err);

    const std::string offOrigin = R"({"buffers": [{"uri": "http://evil.example/x.bin"}]})";
    refs.clear();
    const bool offOriginOk = gltf_siblings::collect_external_refs(offOrigin, base, &refs, &err);
    Check(!offOriginOk, "  off-origin document refused: " + err);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

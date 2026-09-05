// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pure helpers for fetching a `.gltf`'s external siblings.
 *
 * A `.gltf` is JSON that POINTS at its payload: `buffers[].uri` and
 * `images[].uri` are (usually) paths relative to the `.gltf` itself. When the
 * viewer is handed a URL (`--src=<url>` / `displayxr-view://open?src=…`) only
 * that one file is downloaded, so the load fails with nothing to show for it
 * (issue #114). These helpers turn the JSON into a list of *validated*
 * absolute sibling URLs plus the relative paths to store them under; the
 * platform half (WinHTTP download + placement) lives in windows/main.cpp.
 *
 * Everything here is platform-neutral and unit tested by
 * `model_common/tests/gltf_uri_tests.cpp` — the validator is the security
 * boundary (a hostile glTF must not be able to write outside the asset
 * directory or fetch off-origin), so it is tested directly rather than only
 * through a live download.
 */

#pragma once

#include <string>
#include <vector>

namespace gltf_siblings {

//! One external reference from the glTF JSON.
struct SiblingRef {
    std::string uri;           //!< raw URI exactly as the JSON spells it
    std::string relativePath;  //!< percent-decoded, '.'-segments dropped, '/' separated
    std::string url;           //!< absolute URL, resolved against the .gltf's URL
};

/*!
 * Is `uri` a relative reference this loader is willing to fetch and to write
 * under the asset directory?
 *
 * Accepts a plain relative path (`textures/albedo.png`, `./mesh.bin`).
 * Rejects, with a reason in `reason` when non-null: anything with a scheme
 * (`https:`, `data:`, and `C:` — a Windows drive letter parses as one), a
 * leading `/` or `//`, any `..` segment (raw OR percent-encoded, e.g.
 * `%2e%2e`), backslashes, control characters, a query/fragment, a trailing
 * slash, and absurd lengths.
 *
 * This is the ONLY gate between a downloaded JSON document and a filesystem
 * write, so it is deliberately allowlist-shaped: unknown shapes are refused.
 */
bool uri_is_safe_relative(const std::string& uri, std::string* reason);

//! Percent-decode. False on a malformed escape or an escaped NUL.
bool percent_decode(const std::string& in, std::string* out);

/*!
 * Turn a validated relative URI into the relative path to store it under:
 * percent-decoded, `.` segments dropped, `/` separated (the caller maps `/`
 * to the native separator). False if the result is empty or re-introduces
 * something `uri_is_safe_relative` refuses.
 */
bool relative_path_for(const std::string& uri, std::string* out);

/*!
 * RFC 3986 relative-reference resolution of a *relative path* `rel` against
 * the absolute `base` (whose query and fragment are dropped first). Only the
 * shapes `uri_is_safe_relative` accepts are supported — there is no
 * `..`-popping because `..` never gets this far.
 */
bool resolve_relative_url(const std::string& base, const std::string& rel, std::string* out);

//! scheme + host + port equality, with the default port for http/https implied.
bool same_origin(const std::string& a, const std::string& b);

/*!
 * Collect `buffers[*].uri` and `images[*].uri` from a `.gltf` JSON document,
 * in document order, deduplicated. `data:` URIs are skipped (self-contained).
 * A URI that fails validation makes this FAIL rather than silently drop it: a
 * half-fetched asset would load blank, and "this asset references something
 * unsafe" is the more useful diagnostic.
 *
 * `baseUrl` is the .gltf's final URL; each result carries the resolved
 * absolute URL and the relative path to place it at.
 */
bool collect_external_refs(const std::string& json, const std::string& baseUrl,
                           std::vector<SiblingRef>* out, std::string* err);

//! Extension (lower-case, with dot) of a relative path; empty if none.
std::string path_extension(const std::string& path);

} // namespace gltf_siblings

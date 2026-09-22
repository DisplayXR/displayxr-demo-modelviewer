// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*
 * mv_log.h — one-off diagnostics from model_common, routed per platform.
 *
 * On desktop these are the same std::printf / std::fprintf(stderr, ...) calls
 * the renderer and loaders always made (stdout vs stderr preserved, so the
 * capture scripts' logs are unchanged). On Android a NativeActivity has no
 * console: stdout and stderr go nowhere, so every renderer warning ("device
 * allows N sampled images per stage", "pipeline creation failed") and every
 * loader error was invisible on the one platform most likely to trip them.
 * There they go to logcat under the app's own tag instead.
 *
 * Init / load / one-off only. Nothing on the per-frame path may log through
 * these unconditionally; a diagnostic that runs per frame must be gated by its
 * own env var or throttled.
 */
#pragma once

#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#define MV_LOG_TAG "model_viewer_vk_android"
#define MV_LOG(...) __android_log_print(ANDROID_LOG_INFO, MV_LOG_TAG, __VA_ARGS__)
#define MV_ERR(...) __android_log_print(ANDROID_LOG_WARN, MV_LOG_TAG, __VA_ARGS__)
#else
#define MV_LOG(...) std::printf(__VA_ARGS__)
#define MV_ERR(...) std::fprintf(stderr, __VA_ARGS__)
#endif

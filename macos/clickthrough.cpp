// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Click-through from the frame's own rendered alpha (macOS).
 *
 * The Vulkan half (the fence-pipelined downscale readback of the first + last
 * view tiles) is linux/clickthrough.cpp's, unchanged in behaviour. Only the
 * output differs: Cocoa has no input-region API, so instead of building rects
 * this publishes the coverage raster and answers point queries; main.mm turns
 * the answer for the live cursor position into NSWindow.ignoresMouseEvents.
 * See clickthrough.h.
 */

#include "clickthrough.h"

#include "model_vulkan_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ── Tunables. Deliberately the SAME env names the Windows leg uses ──────────
// (displayxr-common/common/vk_clickthrough_region.h), not the avatar's
// DXR_AVATAR_SIL_*: one knob per behaviour across both platforms.
//   DXR_CLICKTHROUGH_TEXEL_PX  window px per coverage texel (default 4)
//   DXR_CLICKTHROUGH_DILATE    dilation radius in texels    (default 1)
//   DXR_CLICKTHROUGH_ALPHA     region alpha threshold 1..255 (default 8)
//   DXR_VK_NO_HOST_CACHED=1    force the write-combined readback (repro lever)
static constexpr uint32_t kCovMaxW = 1024, kCovMaxH = 576;
static constexpr uint32_t kCovMinW = 64, kCovMinH = 64;
static constexpr uint32_t kTexelPxDefault = 4;

static uint32_t
CtEnvUInt(const char *name, uint32_t def, uint32_t lo, uint32_t hi)
{
	const char *e = getenv(name);
	if (e == nullptr || e[0] == '\0') {
		return def;
	}
	const long v = strtol(e, nullptr, 10);
	if (v < (long)lo || v > (long)hi) {
		return def;
	}
	return (uint32_t)v;
}

// ── Device resources ────────────────────────────────────────────────────────
// The coverage images are allocated ONCE at the ceiling and used as a
// sub-rect, so a window resize never recreates them under an in-flight copy
// (the Windows leg's rule). They are pure transfer targets: no VkImageView,
// therefore no VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT needed — which is the whole
// class of bug avatar#99 is about, avoided by construction rather than fixed.
struct CovImage {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

static CovImage g_cov[2];
static VkFormat g_covFormat = VK_FORMAT_UNDEFINED;
static ModelBuffer g_readback = {};
static void *g_mapped = nullptr;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkFence g_fence = VK_NULL_HANDLE;
static VkCommandBuffer g_prevCmd = VK_NULL_HANDLE;
static bool g_initFailed = false;

// Pipelined readback: each call submits its blits + copies behind a fence and
// does NOT wait; the NEXT call consumes the bytes. Staleness is exactly one
// frame, against the ~16 ms synchronous stall a vkQueueWaitIdle would cost.
static bool g_pending = false;
static uint32_t g_pendingW = 0, g_pendingH = 0;
static bool g_pendingTwo = false;

// Published coverage: one byte per texel, 1 = content present.
static std::vector<uint8_t> g_covBits;
static uint32_t g_covW = 0, g_covH = 0;
static bool g_covReady = false;

static uint32_t g_coveredTexels = 0;

// Raw (pre-threshold) alpha statistics of the last consumed readback, over the
// union of the sampled tiles. Diagnostic only (MacClickthroughAlphaStats).
static MacAlphaStats g_stats = {};

static bool
CreateCovImage(VkDevice dev, VkPhysicalDevice phys, VkFormat fmt, CovImage &out)
{
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {kCovMaxW, kCovMaxH, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(dev, &ici, nullptr, &out.image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, out.image, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = modelFindMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(dev, &ai, nullptr, &out.memory) != VK_SUCCESS) {
		vkDestroyImage(dev, out.image, nullptr);
		out.image = VK_NULL_HANDLE;
		return false;
	}
	vkBindImageMemory(dev, out.image, out.memory, 0);
	return true;
}

//! One-time device setup. Returns false (permanently, via g_initFailed) when
//! the driver cannot blit this format or an allocation fails — click-through
//! then degrades to "not shaped", never to a crash.
static bool
EnsureDevice(const MacClickthroughParams &p)
{
	if (g_initFailed) {
		return false;
	}
	if (g_cov[0].image != VK_NULL_HANDLE && g_covFormat == p.viewFormat) {
		return true;
	}
	if (g_cov[0].image != VK_NULL_HANDLE) {
		// Format changed (a swapchain re-create with a different format).
		// Rare enough to just tear down and rebuild.
		MacClickthroughDestroy(p.dev);
		g_initFailed = false;
	}

	// The blit is the whole mechanism; a format that cannot be blitted must
	// disable the pass rather than generate validation errors every frame.
	VkFormatProperties fp = {};
	vkGetPhysicalDeviceFormatProperties(p.phys, p.viewFormat, &fp);
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if ((fp.optimalTilingFeatures & need) != need) {
		fprintf(stderr,
		        "[WARN]  clickthrough: swapchain format %d cannot be blitted "
		        "(optimalTilingFeatures=0x%x) — click-through disabled.\n",
		        (int)p.viewFormat, (unsigned)fp.optimalTilingFeatures);
		g_initFailed = true;
		return false;
	}

	if (g_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = p.queueFamily;
		if (vkCreateCommandPool(p.dev, &pci, nullptr, &g_pool) != VK_SUCCESS) {
			g_initFailed = true;
			return false;
		}
	}
	if (g_fence == VK_NULL_HANDLE) {
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(p.dev, &fci, nullptr, &g_fence) != VK_SUCCESS) {
			g_initFailed = true;
			return false;
		}
	}
	if (!CreateCovImage(p.dev, p.phys, p.viewFormat, g_cov[0]) ||
	    !CreateCovImage(p.dev, p.phys, p.viewFormat, g_cov[1])) {
		g_initFailed = true;
		return false;
	}
	g_covFormat = p.viewFormat;

	// HOST_CACHED: this buffer is READ every frame, and HOST_VISIBLE|COHERENT
	// alone is typically WRITE-COMBINED on a discrete GPU — fast to write,
	// pathologically slow to read (measured on the Windows leg: ~127 ms/frame
	// write-combined against ~0.86 ms cached). DXR_VK_NO_HOST_CACHED=1 forces
	// the legacy allocation so the pathology stays reproducible.
	const VkDeviceSize bytes = (VkDeviceSize)kCovMaxW * kCovMaxH * 4 * 2;
	static const bool s_noCached = getenv("DXR_VK_NO_HOST_CACHED") != nullptr;
	g_readback = modelCreateBuffer(
	    p.dev, p.phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    s_noCached ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	               : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
	if (g_readback.buffer == VK_NULL_HANDLE) {
		fprintf(stderr, "[WARN]  clickthrough: no HOST_CACHED memory type — using write-combined; "
		                "the per-frame coverage read will be slower on this driver.\n");
		g_readback = modelCreateBuffer(p.dev, p.phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
	if (g_readback.buffer == VK_NULL_HANDLE) {
		g_initFailed = true;
		return false;
	}
	if (vkMapMemory(p.dev, g_readback.memory, 0, bytes, 0, &g_mapped) != VK_SUCCESS) {
		g_mapped = nullptr;
		g_initFailed = true;
		return false;
	}
	return true;
}

//! Fold the previous call's readback into the published coverage: union both
//! planes' alpha, threshold, dilate.
static void
ConsumePendingReadback(VkDevice dev, uint32_t dilate, uint8_t alphaMin)
{
	if (!g_pending || g_fence == VK_NULL_HANDLE) {
		return;
	}
	vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
	g_pending = false;

	const uint8_t *px = (const uint8_t *)g_mapped;
	const uint32_t cw = g_pendingW, ch = g_pendingH;
	const size_t n = (size_t)cw * ch;
	if (px == nullptr || n == 0) {
		return;
	}
	const size_t plane1 = (size_t)kCovMaxW * kCovMaxH * 4;

	// Row-wise memcpy BEFORE touching the bytes: memcpy uses wide streaming
	// loads, which survive write-combined memory; a per-texel loop does not.
	// Insurance for drivers with no HOST_CACHED type. Alpha is byte 3 in both
	// R8G8B8A8 and B8G8R8A8, and sRGB never encodes the alpha channel, so the
	// swapchain's exact format does not matter here.
	std::vector<uint8_t> unionAlpha(n, 0);
	std::vector<uint8_t> rowBuf((size_t)cw * 4);
	for (uint32_t y = 0; y < ch; ++y) {
		uint8_t *out = unionAlpha.data() + (size_t)y * cw;
		memcpy(rowBuf.data(), px + (size_t)y * cw * 4, (size_t)cw * 4);
		for (uint32_t x = 0; x < cw; ++x) {
			out[x] = rowBuf[(size_t)x * 4 + 3];
		}
		if (g_pendingTwo) {
			memcpy(rowBuf.data(), px + plane1 + (size_t)y * cw * 4, (size_t)cw * 4);
			for (uint32_t x = 0; x < cw; ++x) {
				const uint8_t a = rowBuf[(size_t)x * 4 + 3];
				if (a > out[x]) {
					out[x] = a;
				}
			}
		}
	}

	MacAlphaStats st = {};
	st.valid = true;
	st.texels = (uint32_t)n;
	st.minAlpha = 255;
	st.maxAlpha = 0;
	std::vector<uint8_t> bits(n, 0);
	for (size_t i = 0; i < n; ++i) {
		const uint8_t a = unionAlpha[i];
		if (a < st.minAlpha) st.minAlpha = a;
		if (a > st.maxAlpha) st.maxAlpha = a;
		if (a == 0) st.zero++;
		if (a == 255) st.full++;
		bits[i] = (a > alphaMin) ? 1 : 0;
	}
	g_stats = st;
	// Separable dilation, both passes biased outward: the region is what makes
	// the window reachable at all, so an under-large mask loses content the
	// user can see, while an over-large one only leaves a few transparent
	// pixels clickable.
	if (dilate > 0) {
		const uint32_t R = dilate;
		std::vector<uint8_t> tmp(n, 0);
		for (uint32_t y = 0; y < ch; ++y) {
			const uint8_t *src = bits.data() + (size_t)y * cw;
			uint8_t *dst = tmp.data() + (size_t)y * cw;
			for (uint32_t x = 0; x < cw; ++x) {
				if (!src[x]) {
					continue;
				}
				const uint32_t x0 = (x > R) ? x - R : 0;
				const uint32_t x1 = (x + R + 1 < cw) ? x + R + 1 : cw;
				memset(dst + x0, 1, x1 - x0);
			}
		}
		std::vector<uint8_t> out(n, 0);
		for (uint32_t y = 0; y < ch; ++y) {
			const uint8_t *src = tmp.data() + (size_t)y * cw;
			const uint32_t y0 = (y > R) ? y - R : 0;
			const uint32_t y1 = (y + R + 1 < ch) ? y + R + 1 : ch;
			for (uint32_t yy = y0; yy < y1; ++yy) {
				uint8_t *d = out.data() + (size_t)yy * cw;
				for (uint32_t x = 0; x < cw; ++x) {
					d[x] |= src[x];
				}
			}
		}
		bits.swap(out);
	}

	uint32_t covered = 0;
	for (size_t i = 0; i < n; ++i) {
		covered += bits[i];
	}
	g_covBits.swap(bits);
	g_covW = cw;
	g_covH = ch;
	g_coveredTexels = covered;
	g_covReady = true;
}

void
MacClickthroughUpdate(const MacClickthroughParams &p)
{
	if (p.winW == 0 || p.winH == 0 || p.viewImage == VK_NULL_HANDLE || p.tileW == 0 || p.tileH == 0) {
		return;
	}

	// Opaque (Ctrl+T off) and no probe asked for: nothing to read. The stale
	// coverage is dropped so a later Ctrl+T never hit-tests against an old
	// silhouette; the first transparent frame then reports "not ready" and
	// main.mm keeps the window clickable until the readback lands.
	if (!p.active) {
		if (g_pending) {
			ConsumePendingReadback(p.dev, 0, 8);
		}
		g_covReady = false;
		return;
	}

	static const uint32_t s_texelPx = CtEnvUInt("DXR_CLICKTHROUGH_TEXEL_PX", kTexelPxDefault, 1, 64);
	static const uint32_t s_dilate = CtEnvUInt("DXR_CLICKTHROUGH_DILATE", 1, 0, 16);
	static const uint8_t s_alphaMin = (uint8_t)CtEnvUInt("DXR_CLICKTHROUGH_ALPHA", 8, 1, 255);

	uint32_t w = (p.winW + s_texelPx - 1) / s_texelPx;
	uint32_t h = (p.winH + s_texelPx - 1) / s_texelPx;
	if (w < kCovMinW) {
		w = kCovMinW;
	}
	if (w > kCovMaxW) {
		w = kCovMaxW;
	}
	if (h < kCovMinH) {
		h = kCovMinH;
	}
	if (h > kCovMaxH) {
		h = kCovMaxH;
	}

	// 1. Consume the previous call's readback first — it is what the cursor
	//    is hit-tested against until the next call.
	ConsumePendingReadback(p.dev, s_dilate, s_alphaMin);

	if (!EnsureDevice(p)) {
		return;
	}

	// 2. Downscale-blit this frame's outermost view tiles into the coverage
	//    images and copy them out behind the fence — no wait. The atlas is
	//    left in COLOR_ATTACHMENT_OPTIMAL, which is what the runtime expects
	//    at xrReleaseSwapchainImage.
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = g_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(p.dev, &ai, &cmd) != VK_SUCCESS) {
		return;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.subresourceRange = colorRange;

	// Atlas → TRANSFER_SRC.
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.image = p.viewImage;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);

	// Coverage images → TRANSFER_DST. UNDEFINED is correct: every texel that
	// will be read is overwritten by the blit below.
	const uint32_t planes = p.twoViews ? 2u : 1u;
	VkImageMemoryBarrier toDst[2];
	for (uint32_t i = 0; i < planes; ++i) {
		toDst[i] = b;
		toDst[i].srcAccessMask = 0;
		toDst[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst[i].image = g_cov[i].image;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                     nullptr, planes, toDst);

	// BOTH halves of the union come from the SAME frame, in one command
	// buffer. Alternating views instead makes the region trail a moving
	// subject and clip its leading edge: the weave shows every view at once,
	// with horizontal disparity that grows with the window.
	auto blitTile = [&](uint32_t srcX, uint32_t srcY, VkImage dst) {
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {(int32_t)srcX, (int32_t)srcY, 0};
		blit.srcOffsets[1] = {(int32_t)(srcX + p.tileW), (int32_t)(srcY + p.tileH), 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstOffsets[0] = {0, 0, 0};
		blit.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
		vkCmdBlitImage(cmd, p.viewImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	};
	blitTile(p.firstTileX, p.firstTileY, g_cov[0].image);
	if (p.twoViews) {
		blitTile(p.lastTileX, p.lastTileY, g_cov[1].image);
	}

	// Atlas back to COLOR_ATTACHMENT_OPTIMAL for the compositor.
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.image = p.viewImage;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);

	// Coverage → TRANSFER_SRC, then out to the host buffer.
	VkImageMemoryBarrier toSrc[2];
	for (uint32_t i = 0; i < planes; ++i) {
		toSrc[i] = b;
		toSrc[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toSrc[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toSrc[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toSrc[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toSrc[i].image = g_cov[i].image;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                     nullptr, planes, toSrc);

	for (uint32_t i = 0; i < planes; ++i) {
		VkBufferImageCopy region = {};
		region.bufferOffset = (VkDeviceSize)i * kCovMaxW * kCovMaxH * 4;
		region.bufferRowLength = w;
		region.bufferImageHeight = h;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		vkCmdCopyImageToBuffer(cmd, g_cov[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer,
		                       1, &region);
	}
	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkResetFences(p.dev, 1, &g_fence);
	if (vkQueueSubmit(p.queue, 1, &si, g_fence) == VK_SUCCESS) {
		g_pending = true;
		g_pendingTwo = p.twoViews;
		g_pendingW = w;
		g_pendingH = h;
	}
	// `cmd` is consumed on the NEXT call's fence wait; freeing it here while
	// in flight would be invalid. Free the previous one instead.
	if (g_prevCmd != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(p.dev, g_pool, 1, &g_prevCmd);
	}
	g_prevCmd = cmd;
}

void
MacClickthroughDestroy(VkDevice dev)
{
	if (g_fence != VK_NULL_HANDLE) {
		if (g_pending) {
			vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
			g_pending = false;
		}
		vkDestroyFence(dev, g_fence, nullptr);
		g_fence = VK_NULL_HANDLE;
	}
	if (g_prevCmd != VK_NULL_HANDLE && g_pool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(dev, g_pool, 1, &g_prevCmd);
		g_prevCmd = VK_NULL_HANDLE;
	}
	if (g_mapped != nullptr) {
		vkUnmapMemory(dev, g_readback.memory);
		g_mapped = nullptr;
	}
	if (g_readback.buffer != VK_NULL_HANDLE) {
		modelDestroyBuffer(dev, g_readback);
	}
	for (auto &c : g_cov) {
		if (c.image != VK_NULL_HANDLE) {
			vkDestroyImage(dev, c.image, nullptr);
			c.image = VK_NULL_HANDLE;
		}
		if (c.memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev, c.memory, nullptr);
			c.memory = VK_NULL_HANDLE;
		}
	}
	g_covFormat = VK_FORMAT_UNDEFINED;
	if (g_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(dev, g_pool, nullptr);
		g_pool = VK_NULL_HANDLE;
	}
	g_covReady = false;
	g_covBits.clear();
	g_covW = g_covH = 0;
	g_coveredTexels = 0;
	g_stats = {};
}

bool
MacClickthroughHit(float u, float v, bool *outHit)
{
	if (!g_covReady || g_covW == 0 || g_covH == 0) {
		return false;
	}
	if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) {
		*outHit = false;
		return true;
	}
	const uint32_t x = (uint32_t)(u * (float)g_covW);
	const uint32_t y = (uint32_t)(v * (float)g_covH);
	*outHit = g_covBits[(size_t)y * g_covW + (x < g_covW ? x : g_covW - 1)] != 0;
	return true;
}

uint64_t
MacClickthroughCoveredPx(uint32_t winW, uint32_t winH)
{
	if (!g_covReady || g_covW == 0 || g_covH == 0) {
		return 0;
	}
	return (uint64_t)g_coveredTexels * winW * winH / ((uint64_t)g_covW * g_covH);
}

bool
MacClickthroughAlphaStats(MacAlphaStats *out)
{
	if (!g_stats.valid) {
		return false;
	}
	*out = g_stats;
	return true;
}

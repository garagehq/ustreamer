/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    This file adds Rockchip MPP hardware encoder support for RK3588.        #
#                                                                            #
#    Copyright (C) 2018-2024  Maxim Devaev <mdevaev@gmail.com>               #
#    Copyright (C) 2024  GarageHQ                                            #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/

#ifdef WITH_MPP

#include "encoder.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

#include <linux/videodev2.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_venc_cmd.h>

#include "../../../libs/types.h"
#include "../../../libs/tools.h"
#include "../../../libs/logging.h"
#include "../../../libs/frame.h"
#include "../../../libs/overlay.h"
#include "../../../libs/blocking.h"

#include "../../encoder.h"  // For us_g_encode_scale

#ifdef WITH_RGA
#include <rga/im2d.h>
#include <rga/rga.h>
#endif


#define _LOG_ERROR(x_msg, ...)   US_LOG_ERROR("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_PERROR(x_msg, ...)  US_LOG_PERROR("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_INFO(x_msg, ...)    US_LOG_INFO("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_VERBOSE(x_msg, ...) US_LOG_VERBOSE("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_DEBUG(x_msg, ...)   US_LOG_DEBUG("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)

// Align macro for MPP buffer alignment
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// Raw frame storage rate limiting: only store every N frames to reduce memcpy overhead
// OCR/VLM capture at 2s intervals during blocking (see capture.py _MIN_CAPTURE_INTERVAL_BLOCKING)
// so we only need ~1fps updates - every 60 frames at 60fps is sufficient
#define RAW_FRAME_UPDATE_INTERVAL 60
static _Atomic uint _raw_frame_counter = 0;


static void _mpp_encoder_cleanup(us_mpp_encoder_s *enc);
static int _mpp_encoder_prepare(us_mpp_encoder_s *enc, uint width, uint height, uint format,
                                uint hor_stride, uint ver_stride);
static int _compress_copy(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest);
static int _compress_zero_copy(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest);
static MppBuffer _get_imported_buffer(us_mpp_encoder_s *enc, int fd, size_t size);
static void _release_imported_buffers(us_mpp_encoder_s *enc);
static int _encode_buffer(us_mpp_encoder_s *enc, MppBuffer mbuf, us_frame_s *dest);
#ifdef WITH_RGA
static int _compress_rga(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest);
static int _rga_convert_to_frame_buf(us_mpp_encoder_s *enc, const us_frame_s *src);
static int _rga_get_handle(us_mpp_encoder_s *enc, int fd, uint w, uint h, uint fmt);
static void _release_rga_handles(us_mpp_encoder_s *enc);
#endif
static MppFrameFormat _v4l2_to_mpp_format(uint v4l2_format);
static void _get_target_resolution(const us_frame_s *src, uint *target_width, uint *target_height);
static void _copy_nv12_aligned(const u8 *src_data, uint src_width, uint src_height,
                               u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride);
static void _downscale_nv12(const u8 *src_data, uint src_width, uint src_height,
                            u8 *dst_data, uint dst_width, uint dst_height);
static void _convert_nv24_to_nv12(const u8 *src_data, uint width, uint height,
                                   u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride);
static void _convert_bgr24_to_nv12(const u8 *src_data, uint width, uint height,
                                    u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride);


us_mpp_encoder_s *us_mpp_jpeg_encoder_init(const char *name, uint quality) {
	us_mpp_encoder_s *enc;
	US_CALLOC(enc, 1);
	enc->name = us_strdup(name);
	enc->quality = quality;
	enc->ready = false;

	US_LOG_INFO("MPP %s: Initializing hardware JPEG encoder (quality=%u) ...", name, quality);
	return enc;
}

void us_mpp_encoder_destroy(us_mpp_encoder_s *enc) {
	if (enc == NULL) {
		return;
	}
	_LOG_INFO("Destroying encoder ...");
	_mpp_encoder_cleanup(enc);
	free(enc->name);
	free(enc);
}

int us_mpp_encoder_compress(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest) {
	// ZERO-COPY FAST PATH: when no pixel modification is needed (no blocking
	// composite, no notification overlay, no scaling/format conversion) and
	// the source carries an exported DMABUF fd, hand the V4L2 buffer straight
	// to the VPU. The copy path below reads every pixel from UNCACHED V4L2
	// mmap memory on the CPU — measured ~90ms per 1080p frame (15x the actual
	// ~5ms VPU encode) — which was the root cause of the pipeline capping at
	// half the capture rate (60 -> 27-30 fps).
	if (!enc->zero_copy_broken
		&& src->format == V4L2_PIX_FMT_NV12
		&& src->dma_fd >= 0
		&& !us_blocking_is_enabled_fast()
		&& !us_overlay_is_enabled()) {

		uint target_width, target_height;
		_get_target_resolution(src, &target_width, &target_height);
		if (target_width == src->width && target_height == src->height) {
			if (_compress_zero_copy(enc, src, dest) == 0) {
				return 0;
			}
			_LOG_INFO("Zero-copy encode failed; falling back to copy path permanently");
			enc->zero_copy_broken = true;
		}
	}

#ifdef WITH_RGA
	// RGA HARDWARE CSC FAST PATH: NV24 (1080p YCbCr 4:4:4 sources like Roku)
	// and BGR24 (Google TV) need conversion to NV12 for the VEPU. The CPU
	// converters below read every pixel from UNCACHED V4L2 memory (~90ms per
	// 1080p NV24 frame). The RGA 2D block does the same conversion in ~2.4ms
	// DMA-to-DMA. Requires the exported DMABUF fd; pixel-modifying features
	// (blocking composite, notification overlay) still use the CPU path.
	if (!enc->rga_broken
		&& (src->format == V4L2_PIX_FMT_NV24 || src->format == V4L2_PIX_FMT_BGR24)
		&& src->dma_fd >= 0
		&& !us_blocking_is_enabled_fast()
		&& !us_overlay_is_enabled()) {

		uint target_width, target_height;
		_get_target_resolution(src, &target_width, &target_height);
		if (target_width == src->width && target_height == src->height) {
			if (_compress_rga(enc, src, dest) == 0) {
				return 0;
			}
			_LOG_INFO("RGA-assisted encode failed; falling back to CPU path permanently");
			enc->rga_broken = true;
		}
	}
#endif

	return _compress_copy(enc, src, dest);
}

static int _compress_zero_copy(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest) {
	MPP_RET ret;

	// The V4L2 single-planar NV12 layout puts the UV plane at
	// stride * height, so ver_stride MUST be the exact height (2160 and
	// 1080 both satisfy the VEPU's 8-alignment).
	const uint hs = (src->stride > 0 ? src->stride : src->width);
	const uint vs = src->height;

	if (_mpp_encoder_prepare(enc, src->width, src->height, V4L2_PIX_FMT_NV12, hs, vs) < 0) {
		return -1;
	}
	if (!enc->ready) {
		return -1;
	}

	MppBuffer mbuf = _get_imported_buffer(enc, src->dma_fd, (size_t)hs * vs * 3 / 2);
	if (mbuf == NULL) {
		return -1;
	}

	us_frame_encoding_begin(src, dest, V4L2_PIX_FMT_JPEG);
	if (_encode_buffer(enc, mbuf, dest) < 0) {
		return -1;
	}
	us_frame_encoding_end(dest);
	_LOG_VERBOSE("ZC: Compressed frame: %zu bytes, time=%0.3Lf",
		dest->used, dest->encode_end_ts - dest->encode_begin_ts);
	return 0;
}

// Encode one already-populated NV12 MppBuffer into dest using the encoder's
// current configuration. Shared by the zero-copy and RGA fast paths.
static int _encode_buffer(us_mpp_encoder_s *enc, MppBuffer mbuf, us_frame_s *dest) {
	MPP_RET ret;

	MppFrame mpp_frame = NULL;
	ret = mpp_frame_init(&mpp_frame);
	if (ret != MPP_OK) {
		_LOG_ERROR("EB: Failed to init MPP frame: %d", ret);
		return -1;
	}
	mpp_frame_set_width(mpp_frame, enc->width);
	mpp_frame_set_height(mpp_frame, enc->height);
	mpp_frame_set_hor_stride(mpp_frame, enc->hor_stride);
	mpp_frame_set_ver_stride(mpp_frame, enc->ver_stride);
	mpp_frame_set_fmt(mpp_frame, enc->mpp_format);
	mpp_frame_set_eos(mpp_frame, 0);
	mpp_frame_set_buffer(mpp_frame, mbuf);

	MppPacket mpp_packet = NULL;
	ret = enc->mpi->encode_put_frame(enc->mpp_ctx, mpp_frame);
	if (ret != MPP_OK) {
		_LOG_ERROR("EB: Failed to put frame: %d", ret);
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}
	ret = enc->mpi->encode_get_packet(enc->mpp_ctx, &mpp_packet);
	if (ret != MPP_OK || mpp_packet == NULL) {
		_LOG_ERROR("EB: Failed to get packet: %d", ret);
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}

	void *pkt_ptr = mpp_packet_get_pos(mpp_packet);
	size_t pkt_len = mpp_packet_get_length(mpp_packet);
	if (pkt_ptr == NULL || pkt_len == 0) {
		_LOG_ERROR("EB: Empty packet received");
		mpp_packet_deinit(&mpp_packet);
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}
	us_frame_set_data(dest, pkt_ptr, pkt_len);
	dest->key = true;
	dest->gop = 0;

	mpp_packet_deinit(&mpp_packet);
	mpp_frame_deinit(&mpp_frame);
	return 0;
}

#ifdef WITH_RGA

static int _compress_rga(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest) {
	// The VEPU consumes NV12 from enc->frame_buf with 16-aligned strides
	// (identical geometry to the CPU copy path, so blocking-mode transitions
	// reuse the same encoder configuration).
	if (_mpp_encoder_prepare(enc, src->width, src->height, V4L2_PIX_FMT_NV12,
			MPP_ALIGN(src->width, 16), MPP_ALIGN(src->height, 16)) < 0) {
		return -1;
	}
	if (!enc->ready) {
		return -1;
	}
	if (_rga_convert_to_frame_buf(enc, src) < 0) {
		return -1;
	}

	us_frame_encoding_begin(src, dest, V4L2_PIX_FMT_JPEG);
	if (_encode_buffer(enc, enc->frame_buf, dest) < 0) {
		return -1;
	}
	us_frame_encoding_end(dest);
	_LOG_VERBOSE("RGA: Compressed frame: %zu bytes, time=%0.3Lf",
		dest->used, dest->encode_end_ts - dest->encode_begin_ts);
	return 0;
}

static int _rga_convert_to_frame_buf(us_mpp_encoder_s *enc, const us_frame_s *src) {
	const uint width = src->width;
	const uint height = src->height;
	const uint hs = enc->hor_stride;
	const uint vs = enc->ver_stride;
	const uint sstride = (src->stride > 0 ? src->stride : width);

	const int dst_fd = mpp_buffer_get_fd(enc->frame_buf);
	if (dst_fd < 0) {
		_LOG_ERROR("RGA: Failed to get frame_buf fd");
		return -1;
	}

	if (src->format == V4L2_PIX_FMT_BGR24) {
		// Direct hardware colorspace conversion (measured ~2ms @1080p)
		const int sh = _rga_get_handle(enc, src->dma_fd, width, height, RK_FORMAT_BGR_888);
		const int dh = _rga_get_handle(enc, dst_fd, hs, vs, RK_FORMAT_YCbCr_420_SP);
		if (sh == 0 || dh == 0) {
			return -1;
		}
		rga_buffer_t s = wrapbuffer_handle_t(sh, width, height, sstride / 3 > 0 ? sstride / 3 : width, height, RK_FORMAT_BGR_888);
		rga_buffer_t d = wrapbuffer_handle_t(dh, width, height, hs, vs, RK_FORMAT_YCbCr_420_SP);
		const int ret = imcvtcolor(s, d, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_SP);
		if (ret != IM_STATUS_SUCCESS) {
			_LOG_ERROR("RGA: BGR24 imcvtcolor failed: %s", imStrError(ret));
			return -1;
		}
		return 0;
	}

	// NV24 -> NV12 in two hardware passes. The RGA has no YUV444SP support,
	// so both passes reinterpret the planes as RGBA byte layouts:
	//  Pass 1 (Y):  W x H bytes == RGBA image W/4 x H -> plain DMA copy.
	//  Pass 2 (UV): the NV24 UV plane (2W bytes/row, H rows) viewed as RGBA
	//    W/2 x H, each pixel = [U0 V0 U1 V1]. A 2x2 bilinear downscale to
	//    W/4 x H/2 averages each byte channel independently, producing
	//    exactly the NV12 UV plane layout (W bytes/row, H/2 rows). Chroma
	//    pairing differs subtly from the CPU reference (averages columns
	//    {x, x+2} instead of {x, x+1}) — visually indistinguishable after
	//    4:2:0 subsampling (validated: mean |diff| 0.18 on gradients).
	//
	// Both planes address the same DMABUFs through "canvas" views (full
	// buffer as one tall RGBA image) with im_rects selecting the plane:
	//  src canvas A (Y):  sstride/4 px wide, 3H rows;  Y = rows [0, H)
	//  src canvas B (UV): sstride/2 px wide, 3H/2 rows; UV = rows [H/2, 3H/2)
	//  dst canvas    :    hs/4 px wide, vs*3/2 rows;   Y = rows [0, H), UV = rows [vs, vs + H/2)

	const int sha = _rga_get_handle(enc, src->dma_fd, sstride / 4, height * 3, RK_FORMAT_RGBA_8888);
	const int shb = _rga_get_handle(enc, src->dma_fd, sstride / 2, (height * 3) / 2, RK_FORMAT_RGBA_8888);
	const int dhc = _rga_get_handle(enc, dst_fd, hs / 4, (vs * 3) / 2, RK_FORMAT_RGBA_8888);
	if (sha == 0 || shb == 0 || dhc == 0) {
		return -1;
	}

	rga_buffer_t src_y = wrapbuffer_handle_t(sha, sstride / 4, height * 3, sstride / 4, height * 3, RK_FORMAT_RGBA_8888);
	rga_buffer_t src_uv = wrapbuffer_handle_t(shb, sstride / 2, (height * 3) / 2, sstride / 2, (height * 3) / 2, RK_FORMAT_RGBA_8888);
	rga_buffer_t dst_c = wrapbuffer_handle_t(dhc, hs / 4, (vs * 3) / 2, hs / 4, (vs * 3) / 2, RK_FORMAT_RGBA_8888);
	rga_buffer_t pat; memset(&pat, 0, sizeof(pat));
	im_rect prect; memset(&prect, 0, sizeof(prect));

	// Pass 1: Y plane copy
	im_rect sy_rect = { 0, 0, (int)(width / 4), (int)height };
	im_rect dy_rect = { 0, 0, (int)(width / 4), (int)height };
	int ret = improcess(src_y, dst_c, pat, sy_rect, dy_rect, prect, IM_SYNC);
	if (ret != IM_STATUS_SUCCESS) {
		_LOG_ERROR("RGA: NV24 Y copy failed: %s", imStrError(ret));
		return -1;
	}

	// Pass 2: UV plane 2x2 downscale
	im_rect suv_rect = { 0, (int)(height / 2), (int)(width / 2), (int)height };
	im_rect duv_rect = { 0, (int)vs, (int)(width / 4), (int)(height / 2) };
	ret = improcess(src_uv, dst_c, pat, suv_rect, duv_rect, prect, IM_SYNC);
	if (ret != IM_STATUS_SUCCESS) {
		_LOG_ERROR("RGA: NV24 UV downscale failed: %s", imStrError(ret));
		return -1;
	}
	return 0;
}

static int _rga_get_handle(us_mpp_encoder_s *enc, int fd, uint w, uint h, uint fmt) {
	for (uint i = 0; i < enc->n_rga_handles; ++i) {
		if (enc->rga_handles[i].fd == fd
			&& enc->rga_handles[i].w == w
			&& enc->rga_handles[i].h == h
			&& enc->rga_handles[i].fmt == fmt) {
			return enc->rga_handles[i].handle;
		}
	}
	if (enc->n_rga_handles >= US_MPP_MAX_RGA_HANDLES) {
		_LOG_ERROR("RGA: Handle cache full");
		return 0;
	}
	im_handle_param_t param = { w, h, fmt };
	const rga_buffer_handle_t handle = importbuffer_fd(fd, &param);
	if (handle == 0) {
		_LOG_ERROR("RGA: Failed to import fd=%d (%ux%u fmt=0x%x)", fd, w, h, fmt);
		return 0;
	}
	enc->rga_handles[enc->n_rga_handles].fd = fd;
	enc->rga_handles[enc->n_rga_handles].w = w;
	enc->rga_handles[enc->n_rga_handles].h = h;
	enc->rga_handles[enc->n_rga_handles].fmt = fmt;
	enc->rga_handles[enc->n_rga_handles].handle = handle;
	enc->n_rga_handles += 1;
	_LOG_INFO("RGA: Imported fd=%d as %ux%u fmt=0x%x (%u cached)", fd, w, h, fmt, enc->n_rga_handles);
	return handle;
}

static void _release_rga_handles(us_mpp_encoder_s *enc) {
	for (uint i = 0; i < enc->n_rga_handles; ++i) {
		if (enc->rga_handles[i].handle != 0) {
			releasebuffer_handle(enc->rga_handles[i].handle);
			enc->rga_handles[i].handle = 0;
		}
		enc->rga_handles[i].fd = -1;
	}
	enc->n_rga_handles = 0;
}

#endif // WITH_RGA

static MppBuffer _get_imported_buffer(us_mpp_encoder_s *enc, int fd, size_t size) {
	for (uint i = 0; i < enc->n_imports; ++i) {
		if (enc->imports[i].fd == fd) {
			return enc->imports[i].buf;
		}
	}
	if (enc->n_imports >= US_MPP_MAX_IMPORTS) {
		_LOG_ERROR("ZC: Import cache full");
		return NULL;
	}

	MppBufferInfo info;
	memset(&info, 0, sizeof(info));
	info.type = MPP_BUFFER_TYPE_EXT_DMA;
	info.fd = fd;
	info.size = size;

	MppBuffer buf = NULL;
	MPP_RET ret = mpp_buffer_import(&buf, &info);
	if (ret != MPP_OK || buf == NULL) {
		_LOG_ERROR("ZC: Failed to import DMABUF fd=%d: %d", fd, ret);
		return NULL;
	}
	enc->imports[enc->n_imports].fd = fd;
	enc->imports[enc->n_imports].buf = buf;
	enc->n_imports += 1;
	_LOG_INFO("ZC: Imported V4L2 DMABUF fd=%d size=%zu (%u cached)", fd, size, enc->n_imports);
	return buf;
}

static void _release_imported_buffers(us_mpp_encoder_s *enc) {
	for (uint i = 0; i < enc->n_imports; ++i) {
		if (enc->imports[i].buf != NULL) {
			mpp_buffer_put(enc->imports[i].buf);
			enc->imports[i].buf = NULL;
		}
		enc->imports[i].fd = -1;
	}
	enc->n_imports = 0;
}

static int _compress_copy(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest) {
	MPP_RET ret;

	us_frame_encoding_begin(src, dest, V4L2_PIX_FMT_JPEG);

	// Determine target resolution based on encode scale setting
	uint target_width, target_height;
	_get_target_resolution(src, &target_width, &target_height);
	bool needs_downscale = (target_width != src->width || target_height != src->height);

	// Check if we need format conversion to NV12
	// The MPP VEPU hardware encoder only reliably supports NV12 input.
	// When sources output NV24 (e.g., Roku at 720p) or BGR24 (e.g., Google TV),
	// we convert to NV12 for encoding.
	bool needs_nv24_conversion = (src->format == V4L2_PIX_FMT_NV24);
	bool needs_bgr24_conversion = (src->format == V4L2_PIX_FMT_BGR24);
	uint encoder_format = (needs_nv24_conversion || needs_bgr24_conversion) ? V4L2_PIX_FMT_NV12 : src->format;

	// Ensure encoder is configured for target dimensions
	// Always use NV12 format for MPP since that's what the hardware supports reliably
	if (_mpp_encoder_prepare(enc, target_width, target_height, encoder_format,
			MPP_ALIGN(target_width, 16), MPP_ALIGN(target_height, 16)) < 0) {
		_LOG_ERROR("Failed to prepare encoder");
		return -1;
	}

	if (!enc->ready) {
		_LOG_ERROR("Encoder not ready");
		return -1;
	}

	_LOG_DEBUG("Compressing frame %ux%u -> %ux%u ...", src->width, src->height, target_width, target_height);

	// Create input frame
	MppFrame mpp_frame = NULL;
	ret = mpp_frame_init(&mpp_frame);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to init MPP frame: %d", ret);
		return -1;
	}

	// Set frame parameters
	mpp_frame_set_width(mpp_frame, enc->width);
	mpp_frame_set_height(mpp_frame, enc->height);
	mpp_frame_set_hor_stride(mpp_frame, enc->hor_stride);
	mpp_frame_set_ver_stride(mpp_frame, enc->ver_stride);
	mpp_frame_set_fmt(mpp_frame, enc->mpp_format);
	mpp_frame_set_eos(mpp_frame, 0);

	// Copy frame data to MPP buffer (with optional downscaling)
	void *buf_ptr = mpp_buffer_get_ptr(enc->frame_buf);
	if (buf_ptr == NULL) {
		_LOG_ERROR("Failed to get buffer pointer");
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}

	size_t buf_size = mpp_buffer_get_size(enc->frame_buf);

	if (needs_nv24_conversion) {
		// Convert NV24 to NV12 for MPP encoder
		// This enables encoding from sources like Roku that output NV24
		_convert_nv24_to_nv12(src->data, src->width, src->height,
		                       buf_ptr, enc->hor_stride, enc->ver_stride);
	} else if (needs_bgr24_conversion) {
		// Convert BGR24 to NV12 for MPP encoder
		// This enables encoding from sources like Google TV that output BGR24
		_convert_bgr24_to_nv12(src->data, src->width, src->height,
		                        buf_ptr, enc->hor_stride, enc->ver_stride);
	} else if (needs_downscale && src->format == V4L2_PIX_FMT_NV12) {
		// Downscale NV12 frame to target resolution
		_downscale_nv12(src->data, src->width, src->height,
		                buf_ptr, target_width, target_height);
	} else if (src->format == V4L2_PIX_FMT_NV12) {
		// Copy NV12 with proper stride alignment for MPP
		// Source has packed strides, MPP needs aligned strides
		_copy_nv12_aligned(src->data, src->width, src->height,
		                   buf_ptr, enc->hor_stride, enc->ver_stride);
	} else {
		// Direct copy for other formats
		size_t copy_size = src->used;
		if (copy_size > buf_size) {
			_LOG_ERROR("Frame size %zu exceeds buffer size %zu", copy_size, buf_size);
			mpp_frame_deinit(&mpp_frame);
			return -1;
		}
		memcpy(buf_ptr, src->data, copy_size);
	}

	// Check if blocking mode is enabled (atomic check - no mutex overhead)
	bool blocking_enabled = us_blocking_is_enabled_fast();

	// Blocking mode works with NV12 data (after NV24/BGR24 conversion if needed)
	if (blocking_enabled && (src->format == V4L2_PIX_FMT_NV12 || needs_nv24_conversion || needs_bgr24_conversion)) {
		// Blocking mode: composite background + preview + text overlays
		// Use pre-allocated buffer to avoid malloc/free per frame

		// Store raw frame BEFORE compositing for /snapshot/raw endpoint
		// Only update every N frames to reduce ~12MB memcpy overhead
		// OCR/VLM don't need 60fps - 2fps (every 30 frames) is plenty
		uint frame_count = atomic_fetch_add(&_raw_frame_counter, 1);
		if (frame_count % RAW_FRAME_UPDATE_INTERVAL == 0) {
			us_blocking_store_raw_frame((u8*)buf_ptr, enc->width, enc->height, enc->hor_stride);
		}

		// Copy source to blocking buffer first (composite overwrites destination)
		memcpy(enc->blocking_buf, buf_ptr, enc->blocking_buf_size);

		// Destination planes (MPP buffer - will be overwritten with composite)
		u8 *dst_y = (u8*)buf_ptr;
		u8 *dst_uv = dst_y + (enc->hor_stride * enc->ver_stride);

		// Source planes from our copy (live video for preview window)
		u8 *copy_y = enc->blocking_buf;
		u8 *copy_uv = enc->blocking_buf + (enc->hor_stride * enc->ver_stride);

		// Composite blocking overlay onto the frame
		us_blocking_composite_nv12(
			copy_y, copy_uv,                           // Source (live video for preview)
			enc->width, enc->height,
			enc->hor_stride, enc->hor_stride,
			dst_y, dst_uv,                             // Destination (MPP buffer)
			enc->width, enc->height,
			enc->hor_stride, enc->hor_stride
		);
	} else if ((src->format == V4L2_PIX_FMT_NV12 || needs_nv24_conversion || needs_bgr24_conversion) && us_g_overlay != NULL) {
		// Normal mode: just apply text overlay if enabled
		u8 *y_plane = (u8*)buf_ptr;
		u8 *uv_plane = y_plane + (enc->hor_stride * enc->ver_stride);
		us_overlay_draw_nv12(
			y_plane, uv_plane,
			enc->width, enc->height,
			enc->hor_stride, enc->hor_stride
		);
	}

	// Sync buffer to device (flush CPU cache for DMA access)
	// This is critical - without it, MPP may read stale data causing artifacts
	mpp_buffer_sync_end(enc->frame_buf);

	mpp_frame_set_buffer(mpp_frame, enc->frame_buf);

	// Encode
	MppPacket mpp_packet = NULL;
	ret = enc->mpi->encode_put_frame(enc->mpp_ctx, mpp_frame);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to put frame: %d", ret);
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}

	ret = enc->mpi->encode_get_packet(enc->mpp_ctx, &mpp_packet);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to get packet: %d", ret);
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}

	if (mpp_packet != NULL) {
		void *pkt_ptr = mpp_packet_get_pos(mpp_packet);
		size_t pkt_len = mpp_packet_get_length(mpp_packet);

		if (pkt_ptr != NULL && pkt_len > 0) {
			us_frame_set_data(dest, pkt_ptr, pkt_len);
			dest->key = true;  // JPEG frames are always keyframes
			dest->gop = 0;
			_LOG_DEBUG("Encoded JPEG: %zu bytes", pkt_len);
		} else {
			_LOG_ERROR("Empty packet received");
			mpp_packet_deinit(&mpp_packet);
			mpp_frame_deinit(&mpp_frame);
			return -1;
		}

		mpp_packet_deinit(&mpp_packet);
	} else {
		_LOG_ERROR("No packet received");
		mpp_frame_deinit(&mpp_frame);
		return -1;
	}

	mpp_frame_deinit(&mpp_frame);

	us_frame_encoding_end(dest);

	_LOG_VERBOSE("Compressed frame: %zu bytes, time=%0.3Lf",
		dest->used, dest->encode_end_ts - dest->encode_begin_ts);

	return 0;
}

static int _mpp_encoder_prepare(us_mpp_encoder_s *enc, uint width, uint height, uint format,
                                uint hor_stride, uint ver_stride) {
	MppFrameFormat mpp_format = _v4l2_to_mpp_format(format);

	if (mpp_format == MPP_FMT_BUTT) {
		_LOG_ERROR("Unsupported input format: 0x%08x", format);
		return -1;
	}

	// Check if reconfiguration is needed
	if (enc->ready &&
		enc->width == width &&
		enc->height == height &&
		enc->hor_stride == hor_stride &&
		enc->ver_stride == ver_stride &&
		enc->mpp_format == mpp_format) {
		return 0;  // Already configured
	}

	_LOG_INFO("Configuring encoder for %ux%u stride=%ux%u format=0x%08x ...",
		width, height, hor_stride, ver_stride, format);

	// Cleanup existing configuration
	_mpp_encoder_cleanup(enc);

	MPP_RET ret;

	// Store configuration
	enc->width = width;
	enc->height = height;
	enc->mpp_format = mpp_format;

	// Strides are caller-provided: the copy path uses 16-aligned strides for
	// its own buffer; the zero-copy path must match the V4L2 buffer layout
	// exactly (bytesperline x height).
	enc->hor_stride = hor_stride;
	enc->ver_stride = ver_stride;

	// Create MPP context
	ret = mpp_create(&enc->mpp_ctx, &enc->mpi);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to create MPP context: %d", ret);
		goto error;
	}

	// Initialize for MJPEG encoding
	ret = mpp_init(enc->mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to init MPP encoder: %d", ret);
		goto error;
	}

	// Configure encoder
	MppEncCfg cfg;
	ret = mpp_enc_cfg_init(&cfg);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to init encoder config: %d", ret);
		goto error;
	}

	// Get default config
	ret = enc->mpi->control(enc->mpp_ctx, MPP_ENC_GET_CFG, cfg);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to get encoder config: %d", ret);
		mpp_enc_cfg_deinit(cfg);
		goto error;
	}

	// Set prep config (input format)
	mpp_enc_cfg_set_s32(cfg, "prep:width", enc->width);
	mpp_enc_cfg_set_s32(cfg, "prep:height", enc->height);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", enc->hor_stride);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", enc->ver_stride);
	mpp_enc_cfg_set_s32(cfg, "prep:format", enc->mpp_format);

	// Set RC config (for JPEG, mainly quality)
	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);

	// Set codec config (JPEG quality)
	// JPEG quality in MPP is 1-99, where 99 is best quality
	int mpp_quality = enc->quality;
	if (mpp_quality < 1) mpp_quality = 1;
	if (mpp_quality > 99) mpp_quality = 99;
	mpp_enc_cfg_set_s32(cfg, "jpeg:quant", mpp_quality);

	// Apply config
	ret = enc->mpi->control(enc->mpp_ctx, MPP_ENC_SET_CFG, cfg);
	mpp_enc_cfg_deinit(cfg);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to set encoder config: %d", ret);
		goto error;
	}

	// Create buffer group
	ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_DRM);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to create buffer group: %d", ret);
		goto error;
	}

	// Calculate buffer sizes
	size_t frame_size;
	switch (enc->mpp_format) {
		case MPP_FMT_YUV420SP:  // NV12
			frame_size = enc->hor_stride * enc->ver_stride * 3 / 2;
			break;
		case MPP_FMT_YUV422SP:  // NV16
			frame_size = enc->hor_stride * enc->ver_stride * 2;
			break;
		case MPP_FMT_YUV444SP:  // NV24
			frame_size = enc->hor_stride * enc->ver_stride * 3;
			break;
		case MPP_FMT_BGR888:
		case MPP_FMT_RGB888:
			frame_size = enc->hor_stride * enc->ver_stride * 3;
			break;
		case MPP_FMT_YUV422_YUYV:
		case MPP_FMT_YUV422_UYVY:
			frame_size = enc->hor_stride * enc->ver_stride * 2;
			break;
		default:
			frame_size = enc->hor_stride * enc->ver_stride * 3;
			break;
	}

	ret = mpp_buffer_get(enc->buf_grp, &enc->frame_buf, frame_size);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to allocate frame buffer: %d", ret);
		goto error;
	}

	// Clear the buffer ONCE here so stride-padding areas encode as black.
	// The visible region is fully overwritten every frame by the copy/convert
	// helpers and padding never changes, so the previous per-frame memset in
	// the compress path (3MB @1080p / 12MB @4K of pure CPU waste per frame)
	// was unnecessary.
	void *init_ptr = mpp_buffer_get_ptr(enc->frame_buf);
	if (init_ptr != NULL) {
		memset(init_ptr, 0, frame_size);
		mpp_buffer_sync_end(enc->frame_buf);
	}

	// Allocate packet buffer (output JPEG, estimate max size)
	size_t pkt_size = enc->width * enc->height;  // Conservative estimate
	ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf, pkt_size);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to allocate packet buffer: %d", ret);
		goto error;
	}

	// Pre-allocate blocking mode buffer (same size as frame buffer)
	enc->blocking_buf_size = frame_size;
	enc->blocking_buf = (u8*)malloc(enc->blocking_buf_size);
	if (enc->blocking_buf == NULL) {
		_LOG_ERROR("Failed to allocate blocking buffer");
		goto error;
	}

	enc->ready = true;
	_LOG_INFO("Encoder ready: %ux%u, stride=%ux%u, format=%d",
		enc->width, enc->height, enc->hor_stride, enc->ver_stride, enc->mpp_format);

	return 0;

error:
	_mpp_encoder_cleanup(enc);
	return -1;
}

static void _mpp_encoder_cleanup(us_mpp_encoder_s *enc) {
	enc->ready = false;

	_release_imported_buffers(enc);
#ifdef WITH_RGA
	_release_rga_handles(enc);
#endif

	if (enc->pkt_buf != NULL) {
		mpp_buffer_put(enc->pkt_buf);
		enc->pkt_buf = NULL;
	}

	if (enc->frame_buf != NULL) {
		mpp_buffer_put(enc->frame_buf);
		enc->frame_buf = NULL;
	}

	if (enc->buf_grp != NULL) {
		mpp_buffer_group_put(enc->buf_grp);
		enc->buf_grp = NULL;
	}

	if (enc->mpp_ctx != NULL) {
		mpp_destroy(enc->mpp_ctx);
		enc->mpp_ctx = NULL;
		enc->mpi = NULL;
	}

	// Free blocking mode buffer
	if (enc->blocking_buf != NULL) {
		free(enc->blocking_buf);
		enc->blocking_buf = NULL;
		enc->blocking_buf_size = 0;
	}

	enc->width = 0;
	enc->height = 0;
}

static MppFrameFormat _v4l2_to_mpp_format(uint v4l2_format) {
	switch (v4l2_format) {
		case V4L2_PIX_FMT_NV12:
			return MPP_FMT_YUV420SP;
		case V4L2_PIX_FMT_NV16:
			return MPP_FMT_YUV422SP;
		case V4L2_PIX_FMT_NV24:
			return MPP_FMT_YUV444SP;
		case V4L2_PIX_FMT_YUYV:
			return MPP_FMT_YUV422_YUYV;
		case V4L2_PIX_FMT_UYVY:
			return MPP_FMT_YUV422_UYVY;
		case V4L2_PIX_FMT_RGB24:
			return MPP_FMT_RGB888;
		case V4L2_PIX_FMT_BGR24:
			return MPP_FMT_BGR888;
		default:
			return MPP_FMT_BUTT;  // Unsupported
	}
}

static void _copy_nv12_aligned(const u8 *src_data, uint src_width, uint src_height,
                               u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride) {
	// Copy NV12 from packed source to stride-aligned destination
	// NV12: Y plane followed by interleaved UV plane (half height)

	const u8 *src_y = src_data;
	const u8 *src_uv = src_data + (src_width * src_height);

	u8 *dst_y = dst_data;
	u8 *dst_uv = dst_data + (dst_hor_stride * dst_ver_stride);

	// Copy Y plane row by row (handles stride difference)
	if (src_width == dst_hor_stride) {
		// Fast path: strides match, copy entire Y plane
		memcpy(dst_y, src_y, src_width * src_height);
	} else {
		// Copy row by row with stride adjustment
		for (uint y = 0; y < src_height; y++) {
			memcpy(dst_y + y * dst_hor_stride, src_y + y * src_width, src_width);
		}
	}

	// Copy UV plane (half height)
	uint uv_height = src_height / 2;
	if (src_width == dst_hor_stride) {
		// Fast path: strides match
		memcpy(dst_uv, src_uv, src_width * uv_height);
	} else {
		// Copy row by row with stride adjustment
		for (uint y = 0; y < uv_height; y++) {
			memcpy(dst_uv + y * dst_hor_stride, src_uv + y * src_width, src_width);
		}
	}
}

static void _get_target_resolution(const us_frame_s *src, uint *target_width, uint *target_height) {
	// Check the global encode scale setting
	switch (us_g_encode_scale) {
		case US_ENCODE_SCALE_1080P:
			*target_width = 1920;
			*target_height = 1080;
			break;
		case US_ENCODE_SCALE_2K:
			*target_width = 2560;
			*target_height = 1440;
			break;
		case US_ENCODE_SCALE_PASSTHROUGH:
			// Passthrough: no scaling, use source resolution directly
			*target_width = src->width;
			*target_height = src->height;
			break;
		case US_ENCODE_SCALE_NATIVE:
		default:
			// Native mode: auto-downscale 4K NV12 to 1080p
			if (src->width >= 3840 && src->height >= 2160 && src->format == V4L2_PIX_FMT_NV12) {
				*target_width = 1920;
				*target_height = 1080;
			} else {
				*target_width = src->width;
				*target_height = src->height;
			}
			break;
	}

	// Ensure dimensions don't exceed source
	if (*target_width > src->width) {
		*target_width = src->width;
	}
	if (*target_height > src->height) {
		*target_height = src->height;
	}
}

static void _downscale_nv12(const u8 *src_data, uint src_width, uint src_height,
                            u8 *dst_data, uint dst_width, uint dst_height) {
	// Ultra-fast NV12 downscaler using nearest-neighbor sampling
	// NV12: Y plane followed by interleaved UV plane (half height)

	const uint dst_y_stride = MPP_ALIGN(dst_width, 16);
	const uint dst_uv_stride = dst_y_stride;

	// Fixed-point scale factors (16.16 format)
	const uint scale_x_fp = (src_width << 16) / dst_width;
	const uint scale_y_fp = (src_height << 16) / dst_height;

	// Source plane pointers
	const u8 *src_y = src_data;
	const u8 *src_uv = src_data + (src_width * src_height);

	// Destination plane pointers (MPP buffer with aligned strides)
	u8 *dst_y = dst_data;
	u8 *dst_uv = dst_data + (dst_y_stride * MPP_ALIGN(dst_height, 16));

	// Downscale Y plane using nearest-neighbor
	for (uint dy = 0; dy < dst_height; dy++) {
		const uint sy = (dy * scale_y_fp) >> 16;
		const u8 *src_row = src_y + sy * src_width;
		u8 *dst_row = dst_y + dy * dst_y_stride;

		for (uint dx = 0; dx < dst_width; dx++) {
			const uint sx = (dx * scale_x_fp) >> 16;
			dst_row[dx] = src_row[sx];
		}
	}

	// Downscale UV plane (interleaved U/V pairs, half height in source)
	const uint src_uv_height = src_height / 2;
	const uint dst_uv_height = dst_height / 2;
	const uint scale_uv_y_fp = (src_uv_height << 16) / dst_uv_height;

	for (uint dy = 0; dy < dst_uv_height; dy++) {
		const uint sy = (dy * scale_uv_y_fp) >> 16;
		const u8 *src_row = src_uv + sy * src_width;
		u8 *dst_row = dst_uv + dy * dst_uv_stride;

		// Process UV pairs
		for (uint dx = 0; dx < dst_width; dx += 2) {
			const uint sx = ((dx * scale_x_fp) >> 16) & ~1;  // Align to UV pair
			dst_row[dx] = src_row[sx];
			dst_row[dx + 1] = src_row[sx + 1];
		}
	}
}

static void _convert_nv24_to_nv12(const u8 *src_data, uint width, uint height,
                                   u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride) {
	// Convert NV24 (YUV444SP) to NV12 (YUV420SP) for MPP encoder compatibility
	// NV24: Y plane (full res) + UV plane (full res, interleaved)
	// NV12: Y plane (full res) + UV plane (half res in both dimensions, interleaved)
	//
	// This allows HDMI sources that output NV24 (like Roku) to be encoded by MPP.

	// Source planes
	const u8 *src_y = src_data;
	const u8 *src_uv = src_data + (width * height);  // Full resolution UV in NV24

	// Destination planes (with stride alignment)
	u8 *dst_y = dst_data;
	u8 *dst_uv = dst_data + (dst_hor_stride * dst_ver_stride);

	// Copy Y plane row by row (handles stride difference)
	if (width == dst_hor_stride) {
		// Fast path: strides match
		memcpy(dst_y, src_y, width * height);
	} else {
		// Copy row by row with stride adjustment
		for (uint y = 0; y < height; y++) {
			memcpy(dst_y + y * dst_hor_stride, src_y + y * width, width);
		}
	}

	// Subsample UV plane from 4:4:4 to 4:2:0
	// For each 2x2 block of pixels, average the UV values
	const uint dst_uv_height = height / 2;
	const uint dst_uv_width = width;  // Width stays same (UV pairs)

	for (uint dy = 0; dy < dst_uv_height; dy++) {
		// Source UV rows (two rows that we'll average)
		const u8 *src_uv_row0 = src_uv + (dy * 2) * (width * 2);      // Row y*2
		const u8 *src_uv_row1 = src_uv + (dy * 2 + 1) * (width * 2);  // Row y*2+1

		// Destination UV row
		u8 *dst_uv_row = dst_uv + dy * dst_hor_stride;

		// Process 2 pixels at a time (UV pair)
		for (uint dx = 0; dx < dst_uv_width; dx += 2) {
			// In NV24, UV is interleaved at full resolution: U0 V0 U1 V1 ...
			// Average 2x2 block of UV values
			uint u00 = src_uv_row0[dx * 2];       // U at (x, y*2)
			uint v00 = src_uv_row0[dx * 2 + 1];   // V at (x, y*2)
			uint u01 = src_uv_row0[dx * 2 + 2];   // U at (x+1, y*2)
			uint v01 = src_uv_row0[dx * 2 + 3];   // V at (x+1, y*2)
			uint u10 = src_uv_row1[dx * 2];       // U at (x, y*2+1)
			uint v10 = src_uv_row1[dx * 2 + 1];   // V at (x, y*2+1)
			uint u11 = src_uv_row1[dx * 2 + 2];   // U at (x+1, y*2+1)
			uint v11 = src_uv_row1[dx * 2 + 3];   // V at (x+1, y*2+1)

			// Average U and V for the 2x2 block
			dst_uv_row[dx] = (u00 + u01 + u10 + u11 + 2) / 4;
			dst_uv_row[dx + 1] = (v00 + v01 + v10 + v11 + 2) / 4;
		}
	}
}

static void _convert_bgr24_to_nv12(const u8 *src_data, uint width, uint height,
                                    u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride) {
	// Convert BGR24 to NV12 (YUV420SP) for MPP encoder compatibility
	// BGR24: packed BGR bytes (B0 G0 R0 B1 G1 R1 ...)
	// NV12: Y plane (full res) + UV plane (half res in both dimensions, interleaved)
	//
	// This allows HDMI sources that output BGR24 (like Google TV) to be encoded by MPP.
	//
	// Using BT.601 coefficients with fixed-point arithmetic:
	// Y  = ( 66*R + 129*G +  25*B + 128) >> 8 + 16
	// Cb = (-38*R -  74*G + 112*B + 128) >> 8 + 128
	// Cr = (112*R -  94*G -  18*B + 128) >> 8 + 128

	// Destination planes (with stride alignment)
	u8 *dst_y = dst_data;
	u8 *dst_uv = dst_data + (dst_hor_stride * dst_ver_stride);

	// Convert Y plane - process every pixel
	for (uint y = 0; y < height; y++) {
		const u8 *src_row = src_data + y * width * 3;
		u8 *dst_row = dst_y + y * dst_hor_stride;

		for (uint x = 0; x < width; x++) {
			uint b = src_row[x * 3];
			uint g = src_row[x * 3 + 1];
			uint r = src_row[x * 3 + 2];

			// Y = (66*R + 129*G + 25*B + 128) >> 8 + 16
			int y_val = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			dst_row[x] = (u8)(y_val < 0 ? 0 : (y_val > 255 ? 255 : y_val));
		}
	}

	// Convert UV plane - subsample 2x2 blocks
	const uint dst_uv_height = height / 2;
	const uint dst_uv_width = width / 2;

	for (uint dy = 0; dy < dst_uv_height; dy++) {
		// Source rows (two rows that we'll average)
		const u8 *src_row0 = src_data + (dy * 2) * width * 3;
		const u8 *src_row1 = src_data + (dy * 2 + 1) * width * 3;

		// Destination UV row
		u8 *dst_uv_row = dst_uv + dy * dst_hor_stride;

		for (uint dx = 0; dx < dst_uv_width; dx++) {
			// Get 2x2 block of BGR pixels
			uint sx = dx * 2;

			// Pixel (0,0)
			uint b00 = src_row0[sx * 3];
			uint g00 = src_row0[sx * 3 + 1];
			uint r00 = src_row0[sx * 3 + 2];

			// Pixel (1,0)
			uint b01 = src_row0[(sx + 1) * 3];
			uint g01 = src_row0[(sx + 1) * 3 + 1];
			uint r01 = src_row0[(sx + 1) * 3 + 2];

			// Pixel (0,1)
			uint b10 = src_row1[sx * 3];
			uint g10 = src_row1[sx * 3 + 1];
			uint r10 = src_row1[sx * 3 + 2];

			// Pixel (1,1)
			uint b11 = src_row1[(sx + 1) * 3];
			uint g11 = src_row1[(sx + 1) * 3 + 1];
			uint r11 = src_row1[(sx + 1) * 3 + 2];

			// Average the RGB values
			uint r_avg = (r00 + r01 + r10 + r11 + 2) / 4;
			uint g_avg = (g00 + g01 + g10 + g11 + 2) / 4;
			uint b_avg = (b00 + b01 + b10 + b11 + 2) / 4;

			// Cb = (-38*R - 74*G + 112*B + 128) >> 8 + 128
			int cb = ((-38 * (int)r_avg - 74 * (int)g_avg + 112 * (int)b_avg + 128) >> 8) + 128;
			// Cr = (112*R - 94*G - 18*B + 128) >> 8 + 128
			int cr = ((112 * (int)r_avg - 94 * (int)g_avg - 18 * (int)b_avg + 128) >> 8) + 128;

			// Clamp and store (NV12 is U then V interleaved)
			dst_uv_row[dx * 2] = (u8)(cb < 0 ? 0 : (cb > 255 ? 255 : cb));
			dst_uv_row[dx * 2 + 1] = (u8)(cr < 0 ? 0 : (cr > 255 ? 255 : cr));
		}
	}
}

#endif // WITH_MPP

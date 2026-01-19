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

#include "../../encoder.h"  // For us_g_encode_scale


#define _LOG_ERROR(x_msg, ...)   US_LOG_ERROR("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_PERROR(x_msg, ...)  US_LOG_PERROR("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_INFO(x_msg, ...)    US_LOG_INFO("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_VERBOSE(x_msg, ...) US_LOG_VERBOSE("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)
#define _LOG_DEBUG(x_msg, ...)   US_LOG_DEBUG("MPP %s: " x_msg, enc->name, ##__VA_ARGS__)

// Align macro for MPP buffer alignment
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))


static void _mpp_encoder_cleanup(us_mpp_encoder_s *enc);
static int _mpp_encoder_prepare(us_mpp_encoder_s *enc, uint width, uint height, uint format);
static MppFrameFormat _v4l2_to_mpp_format(uint v4l2_format);
static void _get_target_resolution(const us_frame_s *src, uint *target_width, uint *target_height);
static void _copy_nv12_aligned(const u8 *src_data, uint src_width, uint src_height,
                               u8 *dst_data, uint dst_hor_stride, uint dst_ver_stride);
static void _downscale_nv12(const u8 *src_data, uint src_width, uint src_height,
                            u8 *dst_data, uint dst_width, uint dst_height);


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
	MPP_RET ret;

	us_frame_encoding_begin(src, dest, V4L2_PIX_FMT_JPEG);

	// Determine target resolution based on encode scale setting
	uint target_width, target_height;
	_get_target_resolution(src, &target_width, &target_height);
	bool needs_downscale = (target_width != src->width || target_height != src->height);

	// Ensure encoder is configured for target dimensions
	if (_mpp_encoder_prepare(enc, target_width, target_height, src->format) < 0) {
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

	// Clear buffer to avoid artifacts from padding areas
	size_t buf_size = mpp_buffer_get_size(enc->frame_buf);
	memset(buf_ptr, 0, buf_size);

	if (needs_downscale && src->format == V4L2_PIX_FMT_NV12) {
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

	// Apply text overlay if enabled (for NV12 format only)
	if (src->format == V4L2_PIX_FMT_NV12 && us_g_overlay != NULL) {
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

static int _mpp_encoder_prepare(us_mpp_encoder_s *enc, uint width, uint height, uint format) {
	MppFrameFormat mpp_format = _v4l2_to_mpp_format(format);

	if (mpp_format == MPP_FMT_BUTT) {
		_LOG_ERROR("Unsupported input format: 0x%08x", format);
		return -1;
	}

	// Check if reconfiguration is needed
	if (enc->ready &&
		enc->width == width &&
		enc->height == height &&
		enc->mpp_format == mpp_format) {
		return 0;  // Already configured
	}

	_LOG_INFO("Configuring encoder for %ux%u format=0x%08x ...",
		width, height, format);

	// Cleanup existing configuration
	_mpp_encoder_cleanup(enc);

	MPP_RET ret;

	// Store configuration
	enc->width = width;
	enc->height = height;
	enc->mpp_format = mpp_format;

	// Calculate strides (16-byte aligned for MPP)
	enc->hor_stride = MPP_ALIGN(width, 16);
	enc->ver_stride = MPP_ALIGN(height, 16);

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

	// Allocate packet buffer (output JPEG, estimate max size)
	size_t pkt_size = enc->width * enc->height;  // Conservative estimate
	ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf, pkt_size);
	if (ret != MPP_OK) {
		_LOG_ERROR("Failed to allocate packet buffer: %d", ret);
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
		case US_ENCODE_SCALE_4K:
			// Force 4K output (no downscaling)
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

#endif // WITH_MPP

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

#pragma once

#include "../../../libs/types.h"
#include "../../../libs/frame.h"

#ifdef WITH_MPP

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

typedef struct {
	char			*name;
	uint			quality;

	// MPP context
	MppCtx			mpp_ctx;
	MppApi			*mpi;

	// Buffer management
	MppBufferGroup	buf_grp;
	MppBuffer		frame_buf;
	MppBuffer		pkt_buf;

	// Current configuration
	uint			width;
	uint			height;
	uint			hor_stride;
	uint			ver_stride;
	MppFrameFormat	mpp_format;

	// State
	bool			ready;
} us_mpp_encoder_s;


us_mpp_encoder_s *us_mpp_jpeg_encoder_init(const char *name, uint quality);
void us_mpp_encoder_destroy(us_mpp_encoder_s *enc);

int us_mpp_encoder_compress(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest);

#else

// Stubs when MPP is not available
typedef struct {
	char *name;
} us_mpp_encoder_s;

static inline us_mpp_encoder_s *us_mpp_jpeg_encoder_init(const char *name, uint quality) {
	(void)name;
	(void)quality;
	return NULL;
}

static inline void us_mpp_encoder_destroy(us_mpp_encoder_s *enc) {
	(void)enc;
}

static inline int us_mpp_encoder_compress(us_mpp_encoder_s *enc, const us_frame_s *src, us_frame_s *dest) {
	(void)enc;
	(void)src;
	(void)dest;
	return -1;
}

#endif

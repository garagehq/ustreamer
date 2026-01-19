/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    Text overlay support for NV12 frames with MPP hardware encoding.        #
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

#include <stdbool.h>
#include <pthread.h>

#include "types.h"

// Overlay position presets
typedef enum {
	US_OVERLAY_POS_TOP_LEFT = 0,
	US_OVERLAY_POS_TOP_RIGHT,
	US_OVERLAY_POS_BOTTOM_LEFT,
	US_OVERLAY_POS_BOTTOM_RIGHT,
	US_OVERLAY_POS_CENTER,
	US_OVERLAY_POS_CUSTOM,  // Use x, y coordinates
} us_overlay_pos_e;

// Overlay configuration
typedef struct {
	bool		enabled;
	char		text[256];		// Text to display
	us_overlay_pos_e position;	// Position preset
	int			x;				// Custom X position (if position == CUSTOM)
	int			y;				// Custom Y position (if position == CUSTOM)
	uint		scale;			// Text scale factor (1-10)
	u8			y_color;		// Y component (brightness: 16-235 video, 0-255 full)
	u8			u_color;		// U component (128 = neutral)
	u8			v_color;		// V component (128 = neutral)
	bool		background;		// Draw background box
	u8			bg_y;			// Background Y
	u8			bg_u;			// Background U
	u8			bg_v;			// Background V
	u8			bg_alpha;		// Background alpha (0-255, 255=opaque)
	uint		padding;		// Padding around text in pixels
} us_overlay_config_s;

// Global overlay state (thread-safe)
typedef struct {
	us_overlay_config_s config;
	pthread_mutex_t		mutex;
	bool				dirty;		// Config changed, need to recalc
} us_overlay_s;

// Global overlay instance
extern us_overlay_s *us_g_overlay;

// Initialize/destroy global overlay
void us_overlay_init(void);
void us_overlay_destroy(void);

// Configuration functions (thread-safe)
void us_overlay_set_text(const char *text);
void us_overlay_set_position(us_overlay_pos_e pos, int x, int y);
void us_overlay_set_scale(uint scale);
void us_overlay_set_color(u8 y, u8 u, u8 v);
void us_overlay_set_background(bool enabled, u8 y, u8 u, u8 v, u8 alpha);
void us_overlay_set_padding(uint padding);
void us_overlay_enable(bool enabled);
void us_overlay_clear(void);

// Get current config (thread-safe copy)
void us_overlay_get_config(us_overlay_config_s *config);

// Draw overlay onto NV12 frame buffer
// This is called by the MPP encoder before encoding
void us_overlay_draw_nv12(
	u8 *y_plane,		// Pointer to Y plane
	u8 *uv_plane,		// Pointer to UV plane (interleaved)
	uint width,			// Frame width
	uint height,		// Frame height
	uint y_stride,		// Y plane stride
	uint uv_stride		// UV plane stride
);

// Utility: Convert RGB to YUV
void us_overlay_rgb_to_yuv(u8 r, u8 g, u8 b, u8 *y, u8 *u, u8 *v);

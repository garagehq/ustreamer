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

#include "overlay.h"

#include <string.h>
#include <stdlib.h>

#include "tools.h"
#include "threading.h"
#include "logging.h"
#include "frametext_font.h"

// Global overlay instance
us_overlay_s *us_g_overlay = NULL;

#define _LOG_INFO(x_msg, ...)    US_LOG_INFO("OVERLAY: " x_msg, ##__VA_ARGS__)
#define _LOG_DEBUG(x_msg, ...)   US_LOG_DEBUG("OVERLAY: " x_msg, ##__VA_ARGS__)

// Character dimensions in the font bitmap
#define FONT_CHAR_WIDTH		8
#define FONT_CHAR_HEIGHT	8


void us_overlay_init(void) {
	if (us_g_overlay != NULL) {
		return;
	}

	US_CALLOC(us_g_overlay, 1);
	US_MUTEX_INIT(us_g_overlay->mutex);

	// Default configuration
	us_g_overlay->config.enabled = false;
	us_g_overlay->config.text[0] = '\0';
	us_g_overlay->config.position = US_OVERLAY_POS_TOP_RIGHT;
	us_g_overlay->config.x = 0;
	us_g_overlay->config.y = 0;
	us_g_overlay->config.scale = 2;
	// White text (full range)
	us_g_overlay->config.y_color = 235;
	us_g_overlay->config.u_color = 128;
	us_g_overlay->config.v_color = 128;
	// Semi-transparent black background
	us_g_overlay->config.background = true;
	us_g_overlay->config.bg_y = 16;
	us_g_overlay->config.bg_u = 128;
	us_g_overlay->config.bg_v = 128;
	us_g_overlay->config.bg_alpha = 180;
	us_g_overlay->config.padding = 8;
	us_g_overlay->dirty = false;

	_LOG_INFO("Overlay system initialized");
}

void us_overlay_destroy(void) {
	if (us_g_overlay == NULL) {
		return;
	}

	US_MUTEX_DESTROY(us_g_overlay->mutex);
	free(us_g_overlay);
	us_g_overlay = NULL;

	_LOG_INFO("Overlay system destroyed");
}

void us_overlay_set_text(const char *text) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	strncpy(us_g_overlay->config.text, text, sizeof(us_g_overlay->config.text) - 1);
	us_g_overlay->config.text[sizeof(us_g_overlay->config.text) - 1] = '\0';
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);

	_LOG_DEBUG("Text set to: %s", text);
}

void us_overlay_set_position(us_overlay_pos_e pos, int x, int y) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.position = pos;
	us_g_overlay->config.x = x;
	us_g_overlay->config.y = y;
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);
}

void us_overlay_set_scale(uint scale) {
	if (us_g_overlay == NULL) return;
	if (scale < 1) scale = 1;
	if (scale > 10) scale = 10;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.scale = scale;
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);
}

void us_overlay_set_color(u8 y, u8 u, u8 v) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.y_color = y;
	us_g_overlay->config.u_color = u;
	us_g_overlay->config.v_color = v;
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);
}

void us_overlay_set_background(bool enabled, u8 y, u8 u, u8 v, u8 alpha) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.background = enabled;
	us_g_overlay->config.bg_y = y;
	us_g_overlay->config.bg_u = u;
	us_g_overlay->config.bg_v = v;
	us_g_overlay->config.bg_alpha = alpha;
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);
}

void us_overlay_set_padding(uint padding) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.padding = padding;
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);
}

void us_overlay_enable(bool enabled) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.enabled = enabled;
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);

	_LOG_INFO("Overlay %s", enabled ? "enabled" : "disabled");
}

void us_overlay_clear(void) {
	if (us_g_overlay == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	us_g_overlay->config.enabled = false;
	us_g_overlay->config.text[0] = '\0';
	us_g_overlay->dirty = true;
	US_MUTEX_UNLOCK(us_g_overlay->mutex);

	_LOG_INFO("Overlay cleared");
}

void us_overlay_get_config(us_overlay_config_s *config) {
	if (us_g_overlay == NULL || config == NULL) return;

	US_MUTEX_LOCK(us_g_overlay->mutex);
	memcpy(config, &us_g_overlay->config, sizeof(us_overlay_config_s));
	US_MUTEX_UNLOCK(us_g_overlay->mutex);
}

void us_overlay_rgb_to_yuv(u8 r, u8 g, u8 b, u8 *y, u8 *u, u8 *v) {
	// BT.601 conversion (video range 16-235)
	int y_tmp = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
	int u_tmp = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
	int v_tmp = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

	*y = (u8)(y_tmp < 16 ? 16 : (y_tmp > 235 ? 235 : y_tmp));
	*u = (u8)(u_tmp < 16 ? 16 : (u_tmp > 240 ? 240 : u_tmp));
	*v = (u8)(v_tmp < 16 ? 16 : (v_tmp > 240 ? 240 : v_tmp));
}

// Internal: Calculate text dimensions
static void _calc_text_size(const char *text, uint scale, uint *width, uint *height) {
	*width = 0;
	*height = 0;

	if (text == NULL || text[0] == '\0') {
		return;
	}

	uint max_line_width = 0;
	uint current_line_width = 0;
	uint num_lines = 1;

	for (const char *p = text; *p; p++) {
		if (*p == '\n') {
			if (current_line_width > max_line_width) {
				max_line_width = current_line_width;
			}
			current_line_width = 0;
			num_lines++;
		} else {
			current_line_width++;
		}
	}

	if (current_line_width > max_line_width) {
		max_line_width = current_line_width;
	}

	*width = max_line_width * FONT_CHAR_WIDTH * scale;
	*height = num_lines * FONT_CHAR_HEIGHT * scale;
}

// Internal: Calculate overlay position
static void _calc_position(
	const us_overlay_config_s *config,
	uint frame_width, uint frame_height,
	uint text_width, uint text_height,
	int *out_x, int *out_y) {

	uint total_width = text_width + 2 * config->padding;
	uint total_height = text_height + 2 * config->padding;

	switch (config->position) {
		case US_OVERLAY_POS_TOP_LEFT:
			*out_x = config->padding;
			*out_y = config->padding;
			break;

		case US_OVERLAY_POS_TOP_RIGHT:
			*out_x = (int)frame_width - (int)total_width;
			*out_y = config->padding;
			break;

		case US_OVERLAY_POS_BOTTOM_LEFT:
			*out_x = config->padding;
			*out_y = (int)frame_height - (int)total_height;
			break;

		case US_OVERLAY_POS_BOTTOM_RIGHT:
			*out_x = (int)frame_width - (int)total_width;
			*out_y = (int)frame_height - (int)total_height;
			break;

		case US_OVERLAY_POS_CENTER:
			*out_x = ((int)frame_width - (int)total_width) / 2;
			*out_y = ((int)frame_height - (int)total_height) / 2;
			break;

		case US_OVERLAY_POS_CUSTOM:
		default:
			*out_x = config->x;
			*out_y = config->y;
			break;
	}

	// Clamp to frame bounds
	if (*out_x < 0) *out_x = 0;
	if (*out_y < 0) *out_y = 0;
	if (*out_x + (int)total_width > (int)frame_width) {
		*out_x = (int)frame_width - (int)total_width;
	}
	if (*out_y + (int)total_height > (int)frame_height) {
		*out_y = (int)frame_height - (int)total_height;
	}
}

// Internal: Draw a single character to NV12
static void _draw_char_nv12(
	u8 *y_plane, u8 *uv_plane,
	uint y_stride, uint uv_stride,
	uint frame_width, uint frame_height,
	uint x, uint y,
	char ch, uint scale,
	u8 fg_y, u8 fg_u, u8 fg_v) {

	// Clamp character to printable range
	u8 ch_idx = (u8)ch;
	if (ch_idx >= 128) ch_idx = '?';

	const u8 *glyph = US_FRAMETEXT_FONT[ch_idx];

	for (uint cy = 0; cy < FONT_CHAR_HEIGHT; cy++) {
		for (uint cx = 0; cx < FONT_CHAR_WIDTH; cx++) {
			// Check if pixel is set in font bitmap
			bool pixel_on = (glyph[cy] >> cx) & 1;

			if (pixel_on) {
				// Draw scaled pixel
				for (uint sy = 0; sy < scale; sy++) {
					for (uint sx = 0; sx < scale; sx++) {
						uint px = x + cx * scale + sx;
						uint py = y + cy * scale + sy;

						if (px >= frame_width || py >= frame_height) {
							continue;
						}

						// Set Y plane
						y_plane[py * y_stride + px] = fg_y;

						// Set UV plane (2x2 subsampled)
						// Only update UV for even coordinates
						if ((px % 2 == 0) && (py % 2 == 0)) {
							uint uv_x = px;
							uint uv_y = py / 2;
							u8 *uv_ptr = &uv_plane[uv_y * uv_stride + uv_x];
							uv_ptr[0] = fg_u;  // U
							uv_ptr[1] = fg_v;  // V
						}
					}
				}
			}
		}
	}
}

// Internal: Draw background rectangle to NV12
static void _draw_rect_nv12(
	u8 *y_plane, u8 *uv_plane,
	uint y_stride, uint uv_stride,
	uint frame_width, uint frame_height,
	uint x, uint y, uint width, uint height,
	u8 bg_y, u8 bg_u, u8 bg_v, u8 alpha) {

	// Alpha blending factor (0-256 for fixed-point math)
	uint alpha_fg = alpha;
	uint alpha_bg = 256 - alpha;

	for (uint py = y; py < y + height && py < frame_height; py++) {
		for (uint px = x; px < x + width && px < frame_width; px++) {
			// Blend Y
			u8 *y_ptr = &y_plane[py * y_stride + px];
			*y_ptr = (u8)((alpha_fg * bg_y + alpha_bg * (*y_ptr)) >> 8);

			// Blend UV (2x2 subsampled)
			if ((px % 2 == 0) && (py % 2 == 0)) {
				uint uv_x = px;
				uint uv_y = py / 2;
				u8 *uv_ptr = &uv_plane[uv_y * uv_stride + uv_x];
				uv_ptr[0] = (u8)((alpha_fg * bg_u + alpha_bg * uv_ptr[0]) >> 8);
				uv_ptr[1] = (u8)((alpha_fg * bg_v + alpha_bg * uv_ptr[1]) >> 8);
			}
		}
	}
}

void us_overlay_draw_nv12(
	u8 *y_plane, u8 *uv_plane,
	uint width, uint height,
	uint y_stride, uint uv_stride) {

	if (us_g_overlay == NULL) return;

	// Get config snapshot (thread-safe)
	us_overlay_config_s config;
	US_MUTEX_LOCK(us_g_overlay->mutex);
	memcpy(&config, &us_g_overlay->config, sizeof(config));
	US_MUTEX_UNLOCK(us_g_overlay->mutex);

	// Early exit if not enabled or no text
	if (!config.enabled || config.text[0] == '\0') {
		return;
	}

	// Calculate text dimensions
	uint text_width, text_height;
	_calc_text_size(config.text, config.scale, &text_width, &text_height);

	if (text_width == 0 || text_height == 0) {
		return;
	}

	// Calculate position
	int pos_x, pos_y;
	_calc_position(&config, width, height, text_width, text_height, &pos_x, &pos_y);

	// Draw background if enabled
	if (config.background) {
		uint bg_x = (pos_x > (int)config.padding) ? pos_x - config.padding : 0;
		uint bg_y_pos = (pos_y > (int)config.padding) ? pos_y - config.padding : 0;
		uint bg_width = text_width + 2 * config.padding;
		uint bg_height = text_height + 2 * config.padding;

		_draw_rect_nv12(
			y_plane, uv_plane,
			y_stride, uv_stride,
			width, height,
			bg_x, bg_y_pos, bg_width, bg_height,
			config.bg_y, config.bg_u, config.bg_v, config.bg_alpha
		);
	}

	// Draw text
	uint cur_x = pos_x;
	uint cur_y = pos_y;

	for (const char *p = config.text; *p; p++) {
		if (*p == '\n') {
			cur_x = pos_x;
			cur_y += FONT_CHAR_HEIGHT * config.scale;
			continue;
		}

		_draw_char_nv12(
			y_plane, uv_plane,
			y_stride, uv_stride,
			width, height,
			cur_x, cur_y,
			*p, config.scale,
			config.y_color, config.u_color, config.v_color
		);

		cur_x += FONT_CHAR_WIDTH * config.scale;
	}
}

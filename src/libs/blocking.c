/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    Blocking mode support for ad blocking overlay.                          #
#    Composites: pixelated background + preview window + text overlays       #
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

#include "blocking.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

#include <jpeglib.h>
#include <setjmp.h>

#include "tools.h"
#include "threading.h"
#include "logging.h"
#include "frametext_font.h"

// Global blocking instance
us_blocking_s *us_g_blocking = NULL;

#define _LOG_INFO(x_msg, ...)    US_LOG_INFO("BLOCKING: " x_msg, ##__VA_ARGS__)
#define _LOG_DEBUG(x_msg, ...)   US_LOG_DEBUG("BLOCKING: " x_msg, ##__VA_ARGS__)
#define _LOG_ERROR(x_msg, ...)   US_LOG_ERROR("BLOCKING: " x_msg, ##__VA_ARGS__)

// Font dimensions
#define FONT_CHAR_WIDTH     8
#define FONT_CHAR_HEIGHT    8

// JPEG error handling structure
typedef struct {
    struct jpeg_error_mgr   mgr;
    jmp_buf                 jmp;
} _blocking_jpeg_error_s;

// Forward declarations
static void _jpeg_error_handler(j_common_ptr jpeg);
static void _rgb24_to_nv12(const u8 *rgb, uint width, uint height, u8 *nv12);
static void _draw_text_nv12(
    u8 *y_plane, u8 *uv_plane,
    uint y_stride, uint uv_stride,
    uint width, uint height,
    int x, int y,
    const char *text, uint scale,
    u8 fg_y, u8 fg_u, u8 fg_v,
    bool draw_bg, u8 bg_y, u8 bg_u, u8 bg_v, u8 bg_alpha
);
static void _calc_text_size(const char *text, uint scale, uint *w, uint *h);
static void _draw_scaled_nv12(
    const u8 *src_y, const u8 *src_uv,
    uint src_width, uint src_height,
    uint src_y_stride, uint src_uv_stride,
    u8 *dst_y, u8 *dst_uv,
    uint dst_x, uint dst_y_pos,
    uint dst_width, uint dst_height,
    uint dst_y_stride, uint dst_uv_stride,
    uint frame_width, uint frame_height
);


void us_blocking_init(void) {
    if (us_g_blocking != NULL) {
        return;
    }

    US_CALLOC(us_g_blocking, 1);
    US_MUTEX_INIT(us_g_blocking->mutex);

    // Allocate background buffer
    US_CALLOC(us_g_blocking->config.background, US_BLOCKING_MAX_BG_SIZE);

    // Default configuration
    us_g_blocking->config.enabled = false;
    us_g_blocking->config.bg_valid = false;
    us_g_blocking->config.preview_enabled = false;

    // Initialize atomic enabled flag
    atomic_store(&us_g_blocking->enabled_fast, false);

    // Default text scales (vocab larger for readability, stats smaller)
    us_g_blocking->config.text_vocab_scale = 10;  // Large for vocabulary (8x8 * 10 = 80px chars)
    us_g_blocking->config.text_stats_scale = 4;   // Smaller for debug stats (8x8 * 4 = 32px chars)

    // Default colors: white text on semi-transparent black background
    us_g_blocking->config.text_y = 235;
    us_g_blocking->config.text_u = 128;
    us_g_blocking->config.text_v = 128;
    us_g_blocking->config.bg_box_y = 16;
    us_g_blocking->config.bg_box_u = 128;
    us_g_blocking->config.bg_box_v = 128;
    us_g_blocking->config.bg_box_alpha = 180;

    us_g_blocking->dirty = false;

    _LOG_INFO("Blocking mode system initialized");
}

void us_blocking_destroy(void) {
    if (us_g_blocking == NULL) {
        return;
    }

    US_MUTEX_DESTROY(us_g_blocking->mutex);

    if (us_g_blocking->config.background != NULL) {
        free(us_g_blocking->config.background);
    }

    free(us_g_blocking);
    us_g_blocking = NULL;

    _LOG_INFO("Blocking mode system destroyed");
}

void us_blocking_enable(bool enabled) {
    if (us_g_blocking == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.enabled = enabled;
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);

    // Update atomic flag for fast hot-path check
    atomic_store_explicit(&us_g_blocking->enabled_fast, enabled, memory_order_release);

    _LOG_INFO("Blocking mode %s", enabled ? "ENABLED" : "DISABLED");
}

bool us_blocking_is_enabled(void) {
    if (us_g_blocking == NULL) return false;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    bool enabled = us_g_blocking->config.enabled;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);

    return enabled;
}

// JPEG error handler for setjmp
static void _jpeg_error_handler(j_common_ptr jpeg) {
    _blocking_jpeg_error_s *err = (_blocking_jpeg_error_s*)jpeg->err;
    char msg[JMSG_LENGTH_MAX];
    (*err->mgr.format_message)(jpeg, msg);
    _LOG_ERROR("JPEG decode error: %s", msg);
    longjmp(err->jmp, -1);
}

// Convert RGB24 to NV12
static void _rgb24_to_nv12(const u8 *rgb, uint width, uint height, u8 *nv12) {
    u8 *y_plane = nv12;
    u8 *uv_plane = nv12 + width * height;

    for (uint py = 0; py < height; py++) {
        for (uint px = 0; px < width; px++) {
            uint rgb_idx = (py * width + px) * 3;
            u8 r = rgb[rgb_idx];
            u8 g = rgb[rgb_idx + 1];
            u8 b = rgb[rgb_idx + 2];

            // BT.601 conversion (video range)
            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_plane[py * width + px] = (u8)(y < 16 ? 16 : (y > 235 ? 235 : y));

            // UV plane is 2x2 subsampled
            if ((px % 2 == 0) && (py % 2 == 0)) {
                int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                uint uv_idx = (py / 2) * width + px;
                uv_plane[uv_idx] = (u8)(u < 16 ? 16 : (u > 240 ? 240 : u));
                uv_plane[uv_idx + 1] = (u8)(v < 16 ? 16 : (v > 240 ? 240 : v));
            }
        }
    }
}

int us_blocking_set_background_jpeg(const u8 *jpeg_data, size_t jpeg_size) {
    if (us_g_blocking == NULL) return -1;
    if (jpeg_data == NULL || jpeg_size == 0) return -1;

    struct jpeg_decompress_struct jpeg;
    _blocking_jpeg_error_s jpeg_error;

    jpeg_create_decompress(&jpeg);
    jpeg.err = jpeg_std_error(&jpeg_error.mgr);
    jpeg_error.mgr.error_exit = _jpeg_error_handler;

    if (setjmp(jpeg_error.jmp) < 0) {
        jpeg_destroy_decompress(&jpeg);
        return -1;
    }

    jpeg_mem_src(&jpeg, jpeg_data, jpeg_size);
    jpeg_read_header(&jpeg, TRUE);
    jpeg.out_color_space = JCS_RGB;

    jpeg_start_decompress(&jpeg);

    uint width = jpeg.output_width;
    uint height = jpeg.output_height;

    _LOG_DEBUG("Decoding background JPEG: %ux%u", width, height);

    // Check if it fits in our buffer
    size_t nv12_size = width * height * 3 / 2;
    if (nv12_size > US_BLOCKING_MAX_BG_SIZE) {
        _LOG_ERROR("Background too large: %ux%u (max %zu bytes)", width, height, (size_t)US_BLOCKING_MAX_BG_SIZE);
        jpeg_destroy_decompress(&jpeg);
        return -1;
    }

    // Decode to RGB24 first (temporary buffer)
    size_t rgb_size = width * height * 3;
    u8 *rgb_buf = (u8*)malloc(rgb_size);
    if (rgb_buf == NULL) {
        _LOG_ERROR("Failed to allocate RGB buffer");
        jpeg_destroy_decompress(&jpeg);
        return -1;
    }

    u8 *scanline = (u8*)malloc(width * 3);
    if (scanline == NULL) {
        free(rgb_buf);
        jpeg_destroy_decompress(&jpeg);
        return -1;
    }

    u8 *rgb_ptr = rgb_buf;
    while (jpeg.output_scanline < jpeg.output_height) {
        u8 *row_ptr = scanline;
        jpeg_read_scanlines(&jpeg, &row_ptr, 1);
        memcpy(rgb_ptr, scanline, width * 3);
        rgb_ptr += width * 3;
    }

    jpeg_finish_decompress(&jpeg);
    jpeg_destroy_decompress(&jpeg);
    free(scanline);

    // Convert RGB24 to NV12 and store in background buffer
    US_MUTEX_LOCK(us_g_blocking->mutex);

    _rgb24_to_nv12(rgb_buf, width, height, us_g_blocking->config.background);
    us_g_blocking->config.bg_width = width;
    us_g_blocking->config.bg_height = height;
    us_g_blocking->config.bg_valid = true;
    us_g_blocking->dirty = true;

    US_MUTEX_UNLOCK(us_g_blocking->mutex);

    free(rgb_buf);

    _LOG_INFO("Background set: %ux%u", width, height);
    return 0;
}

void us_blocking_set_preview(int x, int y, uint w, uint h, bool enabled) {
    if (us_g_blocking == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.preview_x = x;
    us_g_blocking->config.preview_y = y;
    us_g_blocking->config.preview_w = w;
    us_g_blocking->config.preview_h = h;
    us_g_blocking->config.preview_enabled = enabled;
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);

    _LOG_DEBUG("Preview set: pos=(%d,%d) size=%ux%u enabled=%d", x, y, w, h, enabled);
}

void us_blocking_set_text_vocab(const char *text) {
    if (us_g_blocking == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    if (text != NULL) {
        strncpy(us_g_blocking->config.text_vocab, text, US_BLOCKING_TEXT_VOCAB_SIZE - 1);
        us_g_blocking->config.text_vocab[US_BLOCKING_TEXT_VOCAB_SIZE - 1] = '\0';
    } else {
        us_g_blocking->config.text_vocab[0] = '\0';
    }
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

void us_blocking_set_text_stats(const char *text) {
    if (us_g_blocking == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    if (text != NULL) {
        strncpy(us_g_blocking->config.text_stats, text, US_BLOCKING_TEXT_STATS_SIZE - 1);
        us_g_blocking->config.text_stats[US_BLOCKING_TEXT_STATS_SIZE - 1] = '\0';
    } else {
        us_g_blocking->config.text_stats[0] = '\0';
    }
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

void us_blocking_set_text_vocab_scale(uint scale) {
    if (us_g_blocking == NULL) return;
    if (scale < 1) scale = 1;
    if (scale > 15) scale = 15;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.text_vocab_scale = scale;
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

void us_blocking_set_text_stats_scale(uint scale) {
    if (us_g_blocking == NULL) return;
    if (scale < 1) scale = 1;
    if (scale > 10) scale = 10;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.text_stats_scale = scale;
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

void us_blocking_set_text_color(u8 y, u8 u, u8 v) {
    if (us_g_blocking == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.text_y = y;
    us_g_blocking->config.text_u = u;
    us_g_blocking->config.text_v = v;
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

void us_blocking_set_box_color(u8 y, u8 u, u8 v, u8 alpha) {
    if (us_g_blocking == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.bg_box_y = y;
    us_g_blocking->config.bg_box_u = u;
    us_g_blocking->config.bg_box_v = v;
    us_g_blocking->config.bg_box_alpha = alpha;
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

void us_blocking_clear(void) {
    if (us_g_blocking == NULL) return;

    // Update atomic flag first
    atomic_store_explicit(&us_g_blocking->enabled_fast, false, memory_order_release);

    US_MUTEX_LOCK(us_g_blocking->mutex);
    us_g_blocking->config.enabled = false;
    us_g_blocking->config.bg_valid = false;
    us_g_blocking->config.preview_enabled = false;
    us_g_blocking->config.text_vocab[0] = '\0';
    us_g_blocking->config.text_stats[0] = '\0';
    us_g_blocking->dirty = true;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);

    _LOG_INFO("Blocking state cleared");
}

void us_blocking_get_config(us_blocking_config_s *config) {
    if (us_g_blocking == NULL || config == NULL) return;

    US_MUTEX_LOCK(us_g_blocking->mutex);
    // Copy config but NOT the background buffer pointer
    config->enabled = us_g_blocking->config.enabled;
    config->bg_width = us_g_blocking->config.bg_width;
    config->bg_height = us_g_blocking->config.bg_height;
    config->bg_valid = us_g_blocking->config.bg_valid;
    config->preview_x = us_g_blocking->config.preview_x;
    config->preview_y = us_g_blocking->config.preview_y;
    config->preview_w = us_g_blocking->config.preview_w;
    config->preview_h = us_g_blocking->config.preview_h;
    config->preview_enabled = us_g_blocking->config.preview_enabled;
    memcpy(config->text_vocab, us_g_blocking->config.text_vocab, US_BLOCKING_TEXT_VOCAB_SIZE);
    memcpy(config->text_stats, us_g_blocking->config.text_stats, US_BLOCKING_TEXT_STATS_SIZE);
    config->text_vocab_scale = us_g_blocking->config.text_vocab_scale;
    config->text_stats_scale = us_g_blocking->config.text_stats_scale;
    config->text_y = us_g_blocking->config.text_y;
    config->text_u = us_g_blocking->config.text_u;
    config->text_v = us_g_blocking->config.text_v;
    config->bg_box_y = us_g_blocking->config.bg_box_y;
    config->bg_box_u = us_g_blocking->config.bg_box_u;
    config->bg_box_v = us_g_blocking->config.bg_box_v;
    config->bg_box_alpha = us_g_blocking->config.bg_box_alpha;
    US_MUTEX_UNLOCK(us_g_blocking->mutex);
}

// Calculate text dimensions
static void _calc_text_size(const char *text, uint scale, uint *w, uint *h) {
    *w = 0;
    *h = 0;

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

    *w = max_line_width * FONT_CHAR_WIDTH * scale;
    *h = num_lines * FONT_CHAR_HEIGHT * scale;
}

// Draw a single character
static void _draw_char_nv12(
    u8 *y_plane, u8 *uv_plane,
    uint y_stride, uint uv_stride,
    uint frame_width, uint frame_height,
    uint x, uint y,
    char ch, uint scale,
    u8 fg_y, u8 fg_u, u8 fg_v) {

    u8 ch_idx = (u8)ch;
    if (ch_idx >= 128) ch_idx = '?';

    const u8 *glyph = US_FRAMETEXT_FONT[ch_idx];

    for (uint cy = 0; cy < FONT_CHAR_HEIGHT; cy++) {
        for (uint cx = 0; cx < FONT_CHAR_WIDTH; cx++) {
            bool pixel_on = (glyph[cy] >> cx) & 1;

            if (pixel_on) {
                for (uint sy = 0; sy < scale; sy++) {
                    for (uint sx = 0; sx < scale; sx++) {
                        uint px = x + cx * scale + sx;
                        uint py = y + cy * scale + sy;

                        if (px >= frame_width || py >= frame_height) {
                            continue;
                        }

                        y_plane[py * y_stride + px] = fg_y;

                        if ((px % 2 == 0) && (py % 2 == 0)) {
                            uint uv_x = px;
                            uint uv_y = py / 2;
                            u8 *uv_ptr = &uv_plane[uv_y * uv_stride + uv_x];
                            uv_ptr[0] = fg_u;
                            uv_ptr[1] = fg_v;
                        }
                    }
                }
            }
        }
    }
}

// Draw background rectangle with alpha blending
static void _draw_rect_nv12(
    u8 *y_plane, u8 *uv_plane,
    uint y_stride, uint uv_stride,
    uint frame_width, uint frame_height,
    uint x, uint y, uint width, uint height,
    u8 bg_y, u8 bg_u, u8 bg_v, u8 alpha) {

    uint alpha_fg = alpha;
    uint alpha_bg = 256 - alpha;

    for (uint py = y; py < y + height && py < frame_height; py++) {
        for (uint px = x; px < x + width && px < frame_width; px++) {
            u8 *y_ptr = &y_plane[py * y_stride + px];
            *y_ptr = (u8)((alpha_fg * bg_y + alpha_bg * (*y_ptr)) >> 8);

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

// Draw text with optional background
static void _draw_text_nv12(
    u8 *y_plane, u8 *uv_plane,
    uint y_stride, uint uv_stride,
    uint width, uint height,
    int x, int y,
    const char *text, uint scale,
    u8 fg_y, u8 fg_u, u8 fg_v,
    bool draw_bg, u8 bg_y, u8 bg_u, u8 bg_v, u8 bg_alpha) {

    if (text == NULL || text[0] == '\0') return;

    uint text_w, text_h;
    _calc_text_size(text, scale, &text_w, &text_h);

    uint padding = 8;

    // Draw background if requested
    if (draw_bg && text_w > 0 && text_h > 0) {
        int bg_x = x > (int)padding ? x - (int)padding : 0;
        int bg_y_pos = y > (int)padding ? y - (int)padding : 0;
        uint bg_w = text_w + 2 * padding;
        uint bg_h = text_h + 2 * padding;

        _draw_rect_nv12(
            y_plane, uv_plane, y_stride, uv_stride,
            width, height,
            bg_x, bg_y_pos, bg_w, bg_h,
            bg_y, bg_u, bg_v, bg_alpha
        );
    }

    // Draw text characters
    uint cur_x = x;
    uint cur_y = y;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            cur_x = x;
            cur_y += FONT_CHAR_HEIGHT * scale;
            continue;
        }

        _draw_char_nv12(
            y_plane, uv_plane, y_stride, uv_stride,
            width, height,
            cur_x, cur_y,
            *p, scale,
            fg_y, fg_u, fg_v
        );

        cur_x += FONT_CHAR_WIDTH * scale;
    }
}

// Scale and copy NV12 frame (for preview window)
static void _draw_scaled_nv12(
    const u8 *src_y, const u8 *src_uv,
    uint src_width, uint src_height,
    uint src_y_stride, uint src_uv_stride,
    u8 *dst_y, u8 *dst_uv,
    uint dst_x, uint dst_y_pos,
    uint dst_width, uint dst_height,
    uint dst_y_stride, uint dst_uv_stride,
    uint frame_width, uint frame_height) {

    // Fixed-point scale factors
    uint scale_x = (src_width << 16) / dst_width;
    uint scale_y = (src_height << 16) / dst_height;

    // Draw Y plane (nearest neighbor scaling)
    for (uint dy = 0; dy < dst_height; dy++) {
        uint py = dst_y_pos + dy;
        if (py >= frame_height) break;

        uint sy = (dy * scale_y) >> 16;
        if (sy >= src_height) sy = src_height - 1;

        for (uint dx = 0; dx < dst_width; dx++) {
            uint px = dst_x + dx;
            if (px >= frame_width) break;

            uint sx = (dx * scale_x) >> 16;
            if (sx >= src_width) sx = src_width - 1;

            dst_y[py * dst_y_stride + px] = src_y[sy * src_y_stride + sx];
        }
    }

    // Draw UV plane (half resolution)
    uint src_uv_height = src_height / 2;
    uint dst_uv_height = dst_height / 2;

    for (uint dy = 0; dy < dst_uv_height; dy++) {
        uint py = (dst_y_pos / 2) + dy;
        if (py >= frame_height / 2) break;

        uint sy = (dy * scale_y) >> 16;
        if (sy >= src_uv_height) sy = src_uv_height - 1;

        for (uint dx = 0; dx < dst_width; dx += 2) {
            uint px = dst_x + dx;
            if (px >= frame_width) break;

            uint sx = ((dx * scale_x) >> 16) & ~1;  // Align to UV pair
            if (sx >= src_width) sx = (src_width - 2) & ~1;

            uint dst_idx = py * dst_uv_stride + px;
            uint src_idx = sy * src_uv_stride + sx;

            dst_uv[dst_idx] = src_uv[src_idx];
            dst_uv[dst_idx + 1] = src_uv[src_idx + 1];
        }
    }
}

// Copy background NV12 to destination buffer (with scaling if needed)
static void _copy_background_nv12(
    const u8 *bg_data, uint bg_width, uint bg_height,
    u8 *dst_y, u8 *dst_uv,
    uint dst_width, uint dst_height,
    uint dst_y_stride, uint dst_uv_stride) {

    const u8 *bg_y = bg_data;
    const u8 *bg_uv = bg_data + bg_width * bg_height;

    // Use scaling function to fit background to destination
    if (bg_width == dst_width && bg_height == dst_height) {
        // Same size - direct copy with stride adjustment
        for (uint y = 0; y < dst_height; y++) {
            memcpy(dst_y + y * dst_y_stride, bg_y + y * bg_width, dst_width);
        }
        for (uint y = 0; y < dst_height / 2; y++) {
            memcpy(dst_uv + y * dst_uv_stride, bg_uv + y * bg_width, dst_width);
        }
    } else {
        // Need to scale
        _draw_scaled_nv12(
            bg_y, bg_uv,
            bg_width, bg_height,
            bg_width, bg_width,  // Background has packed strides
            dst_y, dst_uv,
            0, 0,  // Start at top-left
            dst_width, dst_height,
            dst_y_stride, dst_uv_stride,
            dst_width, dst_height
        );
    }
}

void us_blocking_composite_nv12(
    const u8 *src_y, const u8 *src_uv,
    uint src_width, uint src_height,
    uint src_y_stride, uint src_uv_stride,
    u8 *dst_y, u8 *dst_uv,
    uint dst_width, uint dst_height,
    uint dst_y_stride, uint dst_uv_stride) {

    if (us_g_blocking == NULL) return;

    // Get config snapshot
    us_blocking_config_s config;
    US_MUTEX_LOCK(us_g_blocking->mutex);

    // Copy scalar values
    config.enabled = us_g_blocking->config.enabled;
    config.bg_width = us_g_blocking->config.bg_width;
    config.bg_height = us_g_blocking->config.bg_height;
    config.bg_valid = us_g_blocking->config.bg_valid;
    config.preview_x = us_g_blocking->config.preview_x;
    config.preview_y = us_g_blocking->config.preview_y;
    config.preview_w = us_g_blocking->config.preview_w;
    config.preview_h = us_g_blocking->config.preview_h;
    config.preview_enabled = us_g_blocking->config.preview_enabled;
    memcpy(config.text_vocab, us_g_blocking->config.text_vocab, US_BLOCKING_TEXT_VOCAB_SIZE);
    memcpy(config.text_stats, us_g_blocking->config.text_stats, US_BLOCKING_TEXT_STATS_SIZE);
    config.text_vocab_scale = us_g_blocking->config.text_vocab_scale;
    config.text_stats_scale = us_g_blocking->config.text_stats_scale;
    config.text_y = us_g_blocking->config.text_y;
    config.text_u = us_g_blocking->config.text_u;
    config.text_v = us_g_blocking->config.text_v;
    config.bg_box_y = us_g_blocking->config.bg_box_y;
    config.bg_box_u = us_g_blocking->config.bg_box_u;
    config.bg_box_v = us_g_blocking->config.bg_box_v;
    config.bg_box_alpha = us_g_blocking->config.bg_box_alpha;

    // Copy background data pointer (we'll use it under lock)
    const u8 *bg_data = config.bg_valid ? us_g_blocking->config.background : NULL;

    // Step 1: Copy or scale background to destination
    if (bg_data != NULL && config.bg_valid) {
        _copy_background_nv12(
            bg_data, config.bg_width, config.bg_height,
            dst_y, dst_uv,
            dst_width, dst_height,
            dst_y_stride, dst_uv_stride
        );
    } else {
        // No background - fill with dark gray
        memset(dst_y, 32, dst_y_stride * dst_height);  // Dark Y
        memset(dst_uv, 128, dst_uv_stride * (dst_height / 2));  // Neutral UV
    }

    US_MUTEX_UNLOCK(us_g_blocking->mutex);

    // Step 2: Draw preview window (scaled live video)
    if (config.preview_enabled && config.preview_w > 0 && config.preview_h > 0) {
        // Handle negative positions (from right/bottom edge)
        int preview_x = config.preview_x;
        int preview_y = config.preview_y;

        if (preview_x < 0) {
            preview_x = (int)dst_width + preview_x - (int)config.preview_w;
        }
        if (preview_y < 0) {
            preview_y = (int)dst_height + preview_y - (int)config.preview_h;
        }

        // Clamp to valid range
        if (preview_x < 0) preview_x = 0;
        if (preview_y < 0) preview_y = 0;

        // Ensure even coordinates for NV12
        preview_x &= ~1;
        preview_y &= ~1;

        _draw_scaled_nv12(
            src_y, src_uv,
            src_width, src_height,
            src_y_stride, src_uv_stride,
            dst_y, dst_uv,
            preview_x, preview_y,
            config.preview_w, config.preview_h,
            dst_y_stride, dst_uv_stride,
            dst_width, dst_height
        );

        // Draw border around preview (white)
        // Top edge
        for (uint x = preview_x; x < (uint)preview_x + config.preview_w && x < dst_width; x++) {
            dst_y[preview_y * dst_y_stride + x] = 235;
            if (preview_y + 1 < (int)dst_height) {
                dst_y[(preview_y + 1) * dst_y_stride + x] = 235;
            }
        }
        // Bottom edge
        uint bottom_y = preview_y + config.preview_h - 1;
        if (bottom_y < dst_height) {
            for (uint x = preview_x; x < (uint)preview_x + config.preview_w && x < dst_width; x++) {
                dst_y[bottom_y * dst_y_stride + x] = 235;
                if (bottom_y > 0) {
                    dst_y[(bottom_y - 1) * dst_y_stride + x] = 235;
                }
            }
        }
        // Left edge
        for (uint y = preview_y; y < (uint)preview_y + config.preview_h && y < dst_height; y++) {
            dst_y[y * dst_y_stride + preview_x] = 235;
            if (preview_x + 1 < (int)dst_width) {
                dst_y[y * dst_y_stride + preview_x + 1] = 235;
            }
        }
        // Right edge
        uint right_x = preview_x + config.preview_w - 1;
        if (right_x < dst_width) {
            for (uint y = preview_y; y < (uint)preview_y + config.preview_h && y < dst_height; y++) {
                dst_y[y * dst_y_stride + right_x] = 235;
                if (right_x > 0) {
                    dst_y[y * dst_y_stride + right_x - 1] = 235;
                }
            }
        }
    }

    // Step 3: Draw vocabulary text (centered both horizontally and vertically)
    if (config.text_vocab[0] != '\0') {
        uint text_w, text_h;
        _calc_text_size(config.text_vocab, config.text_vocab_scale, &text_w, &text_h);

        // Center horizontally
        int text_x = ((int)dst_width - (int)text_w) / 2;

        // Center vertically, but shift up a bit to leave room for preview in corner
        // Use upper 60% of screen to avoid overlap with preview window
        int text_y = ((int)dst_height * 6 / 10 - (int)text_h) / 2;

        if (text_x < 10) text_x = 10;
        if (text_y < 10) text_y = 10;

        _draw_text_nv12(
            dst_y, dst_uv, dst_y_stride, dst_uv_stride,
            dst_width, dst_height,
            text_x, text_y,
            config.text_vocab, config.text_vocab_scale,
            config.text_y, config.text_u, config.text_v,
            true, config.bg_box_y, config.bg_box_u, config.bg_box_v, config.bg_box_alpha
        );
    }

    // Step 4: Draw stats text (bottom-left, smaller font)
    if (config.text_stats[0] != '\0') {
        uint text_w, text_h;
        _calc_text_size(config.text_stats, config.text_stats_scale, &text_w, &text_h);

        int text_x = 20;
        int text_y = (int)dst_height - (int)text_h - 30;

        if (text_y < 10) text_y = 10;

        _draw_text_nv12(
            dst_y, dst_uv, dst_y_stride, dst_uv_stride,
            dst_width, dst_height,
            text_x, text_y,
            config.text_stats, config.text_stats_scale,
            config.text_y, config.text_u, config.text_v,
            true, config.bg_box_y, config.bg_box_u, config.bg_box_v, config.bg_box_alpha
        );
    }
}

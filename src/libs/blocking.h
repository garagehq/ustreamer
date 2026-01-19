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

#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include "types.h"

// Maximum text buffer sizes
#define US_BLOCKING_TEXT_VOCAB_SIZE    1024
#define US_BLOCKING_TEXT_STATS_SIZE    512
#define US_BLOCKING_MAX_BG_SIZE        (3840 * 2160 * 3 / 2)  // Max 4K NV12

// Blocking mode configuration
typedef struct {
    bool        enabled;            // Blocking mode active

    // Background image (pixelated pre-ad content)
    u8          *background;        // NV12 data
    uint        bg_width;           // Background width
    uint        bg_height;          // Background height
    bool        bg_valid;           // Background loaded

    // Preview window (live video in corner)
    int         preview_x;          // Preview X position
    int         preview_y;          // Preview Y position
    uint        preview_w;          // Preview width
    uint        preview_h;          // Preview height
    bool        preview_enabled;    // Show preview window

    // Text overlays
    char        text_vocab[US_BLOCKING_TEXT_VOCAB_SIZE];   // Spanish vocabulary
    char        text_stats[US_BLOCKING_TEXT_STATS_SIZE];   // Debug stats
    uint        text_scale;         // Text scale factor (1-10)

    // Colors (YUV)
    u8          text_y, text_u, text_v;     // Text color
    u8          bg_box_y, bg_box_u, bg_box_v, bg_box_alpha;  // Text background box
} us_blocking_config_s;

// Global blocking mode state (thread-safe)
typedef struct {
    us_blocking_config_s    config;
    pthread_mutex_t         mutex;
    bool                    dirty;              // Config changed
    atomic_bool             enabled_fast;       // Atomic flag for fast check (no mutex)
} us_blocking_s;

// Global blocking instance
extern us_blocking_s *us_g_blocking;

// Initialize/destroy global blocking
void us_blocking_init(void);
void us_blocking_destroy(void);

// Enable/disable blocking mode
void us_blocking_enable(bool enabled);
bool us_blocking_is_enabled(void);

// Fast enabled check using atomic (no mutex, safe for hot path)
static inline bool us_blocking_is_enabled_fast(void) {
    extern us_blocking_s *us_g_blocking;
    if (us_g_blocking == NULL) return false;
    return atomic_load_explicit(&us_g_blocking->enabled_fast, memory_order_relaxed);
}

// Set pixelated background image (JPEG data will be decoded to NV12)
// Returns 0 on success, -1 on error
int us_blocking_set_background_jpeg(const u8 *jpeg_data, size_t jpeg_size);

// Set preview window position and size
void us_blocking_set_preview(int x, int y, uint w, uint h, bool enabled);

// Set text overlays
void us_blocking_set_text_vocab(const char *text);
void us_blocking_set_text_stats(const char *text);
void us_blocking_set_text_scale(uint scale);

// Set colors
void us_blocking_set_text_color(u8 y, u8 u, u8 v);
void us_blocking_set_box_color(u8 y, u8 u, u8 v, u8 alpha);

// Clear all blocking state
void us_blocking_clear(void);

// Composite blocking frame onto NV12 buffer
// This replaces the captured frame content with the blocking overlay
// src_frame: original captured frame (used for preview window)
// dst_y, dst_uv: destination buffer planes (will be overwritten)
void us_blocking_composite_nv12(
    const u8 *src_y, const u8 *src_uv,       // Source captured frame
    uint src_width, uint src_height,
    uint src_y_stride, uint src_uv_stride,
    u8 *dst_y, u8 *dst_uv,                   // Destination buffer
    uint dst_width, uint dst_height,
    uint dst_y_stride, uint dst_uv_stride
);

// Get current config snapshot (thread-safe)
void us_blocking_get_config(us_blocking_config_s *config);

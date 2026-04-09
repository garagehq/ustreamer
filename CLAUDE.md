# uStreamer GarageHQ Fork - Development Notes

## Overview

This is a fork of [PiKVM's ustreamer](https://github.com/pikvm/ustreamer) with additional features for RK3588-based HDMI capture devices, specifically for the [Minus](https://github.com/garagehq/Minus) project.

## Key Additions

### NV12/NV16/NV24 Format Support
The RK3588 HDMI-RX driver outputs video in NV12 format (Y/UV 4:2:0), which the stock ustreamer doesn't support. This fork adds:
- NV12, NV16, and NV24 pixel format support
- YCbCr-direct JPEG encoding (no RGB conversion overhead)
- Optimized scanline processing for NV formats

### MPP Hardware JPEG Encoding (`--encoder=mpp-jpeg`)
The RK3588 has a dedicated VPU (Video Processing Unit) for hardware video encoding. This fork integrates the Rockchip MPP (Media Process Platform) library for hardware-accelerated JPEG encoding:
```bash
--encoder=mpp-jpeg    # Use RK3588 VPU for JPEG encoding
```
- Uses `/dev/mpp_service` hardware encoder
- Supports NV12, NV16, NV24, YUYV, UYVY, RGB24, BGR24 input formats
- Significantly faster than CPU encoding for high resolutions
- Auto-detected at build time when `librockchip-mpp-dev` is installed
- Multi-worker support: 4 parallel MPP encoder instances for maximum throughput
- Cache coherency handled via `mpp_buffer_sync_end()` to prevent DMA artifacts

### Flexible Resolution Scaling (`--encode-scale`)
For high-resolution input (4K), CPU JPEG encoding can be a bottleneck. This fork adds:
```bash
--encode-scale native   # Auto: 4K NV12 → 1080p, others unchanged (default)
--encode-scale 1080p    # Force 1080p output (1920x1080)
--encode-scale 2k       # Force 2K output (2560x1440)
--encode-scale 4k       # Force 4K output (no downscaling)
```

### Extended Timeouts & Retry Logic
The RK3588 HDMI-RX driver can be slow to respond. This fork adds:
- Extended device timeout (5s instead of 1s)
- Retry logic (3 attempts) before device restart
- Better error handling for multiplanar devices

## Performance (4K HDMI Input)

| Mode | Encoder | Workers | FPS | Notes |
|------|---------|---------|-----|-------|
| Native 4K | CPU | 1 | ~4 fps | CPU bottleneck on JPEG encoding |
| 2K (2560x1440) | CPU | 8 | ~9-15 fps | `--encode-scale 2k` |
| 1080p | CPU | 8 | ~15-30 fps | `--encode-scale 1080p` |
| **4K** | **MPP-JPEG** | 4 | **~50 fps** | `--encoder=mpp-jpeg --encode-scale=4k` |
| **2K** | **MPP-JPEG** | 4 | **~28 fps** | `--encoder=mpp-jpeg --encode-scale=2k` |
| **1080p** | **MPP-JPEG** | 4 | **~55 fps** | `--encoder=mpp-jpeg` (native auto-downscale) |
| Raw V4L2 capture | N/A | - | 52 fps | Hardware capture capability |

The bottleneck with CPU encoding is JPEG compression. The MPP hardware encoder bypasses this by using the RK3588's dedicated VPU. Using 4 workers allows parallel encoding across multiple MPP encoder instances.

## Modified Files

### Core Format Support
- `src/libs/capture.c` - NV12/NV16/NV24 format detection, extended timeouts, retry logic
- `src/libs/capture.h` - Format string updates (US_FORMATS_STR)
- `src/libs/frame.c` - NV format byte calculations

### JPEG Encoding
- `src/ustreamer/encoders/cpu/encoder.c` - NV12/NV16/NV24 YCbCr encoding, flexible downscaling

### MPP Hardware Encoding (NEW)
- `src/ustreamer/encoders/mpp/encoder.h` - MPP encoder interface
- `src/ustreamer/encoders/mpp/encoder.c` - Rockchip MPP JPEG encoder implementation
- `src/ustreamer/encoder.c` - MPP encoder type and worker integration
- `src/ustreamer/encoder.h` - `US_ENCODER_TYPE_MPP_IMAGE` enum
- `src/ustreamer/options.c` - `--encoder=mpp-jpeg` CLI option
- `src/Makefile` - Auto-detect and link `librockchip_mpp`

### Resolution Scaling
- `src/ustreamer/encoder.c` - Global encode scale setting, scale parsing functions
- `src/ustreamer/encoder.h` - Encode scale enum and extern declaration
- `src/ustreamer/options.c` - `--encode-scale` CLI option with help text

### Blocking Mode System (Ad Blocking Overlay)
- `src/libs/blocking.c` - Core blocking overlay: FreeType text rendering, NV12 compositing, preview window
- `src/libs/blocking.h` - Blocking API interface and config structures
- `src/ustreamer/http/server.c` - HTTP API endpoints (`/blocking`, `/blocking/set`, `/blocking/background`)
- `src/ustreamer/encoders/mpp/encoder.c` - Blocking composite integration in hot path

### Text Overlay System
- `src/libs/overlay.c` - Simple text overlay (non-blocking mode)
- `src/libs/overlay.h` - Overlay API interface

## Blocking Mode System

The blocking mode system renders ad-blocking overlays directly in the MPP encoder pipeline at 60fps. This is used by [Minus](https://github.com/garagehq/Minus) for seamless ad blocking without GStreamer pipeline modifications.

### Features
- **FreeType TrueType Rendering**: Uses IBM Plex Mono Bold (Spanish word), DejaVu Sans Bold (other vocab text), and IBM Plex Mono Regular (stats) - clean, readable fonts for TV viewing
- **Per-line Multi-color Text**: Vocabulary text renders with different colors and fonts per line (purple IBM Plex Mono for Spanish word, white DejaVu Sans for header/translation/pronunciation/example) matching the web UI aesthetic
- **NV12 Direct Rendering**: Text rendered directly to NV12 planes for zero-copy compositing
- **Resolution Flexible**: Automatically handles 1080p, 2K, and 4K output resolutions
- **Preview Window**: Scaled live video thumbnail with border
- **Pixelated Background**: Darkened, pixelated background from pre-ad content
- **Thread-Safe**: Mutex-protected FreeType calls for multi-worker MPP encoding

### API Endpoints

**GET `/blocking`** - Get current blocking configuration
```json
{
  "ok": true,
  "result": {
    "enabled": false,
    "bg_valid": true,
    "bg_width": 1920,
    "bg_height": 1080,
    "preview": {"enabled": true, "x": -40, "y": -40, "w": 384, "h": 216},
    "text_vocab_scale": 10,
    "text_stats_scale": 4,
    "text_color": {"y": 235, "u": 128, "v": 128},
    "word_color": {"y": 128, "u": 195, "v": 156},
    "secondary_color": {"y": 112, "u": 128, "v": 128},
    "box_color": {"y": 16, "u": 128, "v": 128, "alpha": 180}
  }
}
```

**GET `/blocking/set`** - Configure blocking mode
| Parameter | Description |
|-----------|-------------|
| `enabled` | `true`/`1` to enable blocking overlay |
| `text_vocab` | Vocabulary text (URL-encoded, supports `\n` for newlines) |
| `text_stats` | Stats text (bottom-left corner) |
| `text_vocab_scale` | Vocab font scale (e.g., 10 = 120px) |
| `text_stats_scale` | Stats font scale (e.g., 4 = 48px) |
| `preview_enabled` | Show live preview window |
| `preview_x`, `preview_y` | Preview position (negative = from right/bottom edge) |
| `preview_w`, `preview_h` | Preview dimensions (auto-scaled if exceeds frame) |
| `text_y`, `text_u`, `text_v` | Default text color (white - header/translation) |
| `word_y`, `word_u`, `word_v` | Spanish word color (purple) |
| `secondary_y`, `secondary_u`, `secondary_v` | Secondary text color (gray - pronunciation/example) |
| `box_y`, `box_u`, `box_v`, `box_alpha` | Background box color |
| `clear` | Clear all text/disable blocking |

**Multi-color Vocabulary Rendering:**
The vocabulary text is automatically rendered with different colors per line:
- Lines starting with `[` → white (header)
- Lines starting with `(` → gray (pronunciation)
- Lines starting with `=` → white (translation)
- Lines starting with `"` → gray (example)
- Other non-empty lines → purple (Spanish word)

**POST `/blocking/background`** - Upload pixelated background (NV12 raw data)
- Content-Type: application/octet-stream
- Body: Raw NV12 data (width * height * 1.5 bytes)
- Query params: `width`, `height`

### Thread Safety

FreeType is NOT thread-safe. With 4 parallel MPP encoder workers, a mutex (`_ft_mutex`) serializes all FreeType calls in the composite function to prevent crashes:

```c
static pthread_mutex_t _ft_mutex = PTHREAD_MUTEX_INITIALIZER;

// In us_blocking_composite_nv12():
if (need_ft) {
    pthread_mutex_lock(&_ft_mutex);
}
// ... FreeType rendering ...
if (need_ft) {
    pthread_mutex_unlock(&_ft_mutex);
}
```

### Resolution Handling

The blocking system handles resolution mismatches between API calls (which may specify 4K dimensions) and actual encoder output (which may be 1080p due to `--encode-scale`):

1. Preview dimensions are scaled proportionally if they exceed frame bounds
2. Positions are clamped to valid ranges
3. All coordinates are aligned to even values for NV12 compatibility
4. Background is scaled to match output resolution

## Building

```bash
git clone https://github.com/garagehq/ustreamer.git
cd ustreamer

# Install MPP development library (for hardware encoding support)
sudo apt install librockchip-mpp-dev

make -j$(nproc)

# Install
sudo cp ustreamer /usr/local/bin/ustreamer-patched
```

The build system auto-detects `librockchip-mpp-dev` and enables MPP hardware encoding if available. You'll see "MPP hardware encoder support enabled" during build.

## Usage with Minus

Minus automatically:
1. Probes the V4L2 device to detect format (NV12, BGR24, etc.)
2. Starts ustreamer-patched with the correct format
3. Uses `--encode-scale native` for automatic 4K→1080p downscaling

```bash
# Hardware encoding at 4K (~50 FPS)
ustreamer-patched --device=/dev/video0 --format=NV12 --resolution=3840x2160 \
    --encoder=mpp-jpeg --encode-scale=4k --quality=80 --workers=4

# Hardware encoding at 2K (~28 FPS)
ustreamer-patched --device=/dev/video0 --format=NV12 --resolution=3840x2160 \
    --encoder=mpp-jpeg --encode-scale=2k --quality=80 --workers=4

# Hardware encoding with auto-downscale (~55 FPS at 1080p)
ustreamer-patched --device=/dev/video0 --format=NV12 --resolution=3840x2160 \
    --encoder=mpp-jpeg --quality=80 --workers=4

# CPU encoding with downscaling (fallback if no MPP)
ustreamer-patched --device=/dev/video0 --format=NV12 --resolution=3840x2160 \
    --encode-scale 1080p --quality=75 --workers=8 --buffers=8

# Manual usage for BGR24 device
ustreamer-patched --device=/dev/video0 --format=BGR24 --resolution=3840x2160 \
    --quality=75 --workers=8 --buffers=8
```

## Supported Formats

| V4L2 Format | ustreamer Flag | Notes |
|-------------|----------------|-------|
| NV12 | `--format=NV12` | RK3588 HDMI-RX native, YCbCr encoding |
| NV16 | `--format=NV16` | 4:2:2 subsampling |
| NV24 | `--format=NV24` | 4:4:4 no subsampling |
| BGR3/BGR24 | `--format=BGR24` | Some HDMI devices |
| RGB3/RGB24 | `--format=RGB24` | Standard RGB |
| YUYV | `--format=YUYV` | Webcam-style |
| UYVY | `--format=UYVY` | Alternate YUV |
| MJPEG | `--format=MJPEG` | Pre-compressed |

## Troubleshooting

### Device select() timeout
The RK3588 HDMI-RX driver can be slow. This fork has extended timeouts and retry logic. If you still see timeouts:
```bash
# Check if device is working
v4l2-ctl -d /dev/video0 --all

# Check HDMI signal
v4l2-ctl -d /dev/video0 --query-dv-timings
```

### Low FPS at 4K
Use `--encode-scale 1080p` to downscale to 1080p for better FPS:
```bash
ustreamer-patched --encode-scale 1080p ...
```

### Format mismatch errors
Check what format the device is actually outputting:
```bash
v4l2-ctl -d /dev/video0 --get-fmt-video
```

Then use the matching `--format` flag.

### Green horizontal line artifacts (MPP encoder)
If you see intermittent horizontal green/cyan lines in encoded frames, this is caused by CPU cache not being flushed before MPP's DMA engine reads the buffer. The fix requires calling `mpp_buffer_sync_end()` after copying frame data to the MPP buffer. This is already implemented in the current codebase.

## Future Improvements

- ~~**MPP Hardware JPEG encoding**~~: **DONE** - Integrated via `--encoder=mpp-jpeg`. Uses Rockchip MPP library directly.

- **Dynamic format switching**: Currently format is set at startup. Could add runtime format detection when HDMI source changes.

- **MPP H.264/H.265 streaming**: The RK3588 VPU also supports H.264/H.265 encoding which could enable efficient video streaming for WebRTC/Janus integration.

## License

Same as upstream ustreamer - GPLv3.

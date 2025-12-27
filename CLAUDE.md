# uStreamer GarageHQ Fork - Development Notes

## Overview

This is a fork of [PiKVM's ustreamer](https://github.com/pikvm/ustreamer) with additional features for RK3588-based HDMI capture devices, specifically for the [StreamSentry](https://github.com/garagehq/StreamSentry) project.

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

## Usage with StreamSentry

StreamSentry automatically:
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

## Future Improvements

- ~~**MPP Hardware JPEG encoding**~~: **DONE** - Integrated via `--encoder=mpp-jpeg`. Uses Rockchip MPP library directly.

- **Dynamic format switching**: Currently format is set at startup. Could add runtime format detection when HDMI source changes.

- **MPP H.264/H.265 streaming**: The RK3588 VPU also supports H.264/H.265 encoding which could enable efficient video streaming for WebRTC/Janus integration.

## License

Same as upstream ustreamer - GPLv3.

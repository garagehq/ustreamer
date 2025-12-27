# uStreamer GarageHQ Fork - Development Notes

## Overview

This is a fork of [PiKVM's ustreamer](https://github.com/pikvm/ustreamer) with additional features for RK3588-based HDMI capture devices, specifically for the [StreamSentry](https://github.com/garagehq/StreamSentry) project.

## Key Additions

### NV12/NV16/NV24 Format Support
The RK3588 HDMI-RX driver outputs video in NV12 format (Y/UV 4:2:0), which the stock ustreamer doesn't support. This fork adds:
- NV12, NV16, and NV24 pixel format support
- YCbCr-direct JPEG encoding (no RGB conversion overhead)
- Optimized scanline processing for NV formats

### Flexible Resolution Scaling (`--encode-scale`)
For high-resolution input (4K), CPU JPEG encoding can be a bottleneck. This fork adds:
```bash
--encode-scale native   # Auto: 4K NV12 → 1080p, others unchanged (default)
--encode-scale 1080p    # Force 1080p output (1920x1080)
--encode-scale 2k       # Force 2K output (2560x1440)
```

### Extended Timeouts & Retry Logic
The RK3588 HDMI-RX driver can be slow to respond. This fork adds:
- Extended device timeout (5s instead of 1s)
- Retry logic (3 attempts) before device restart
- Better error handling for multiplanar devices

## Performance (4K HDMI Input)

| Mode | FPS | Notes |
|------|-----|-------|
| Native 4K | ~4 fps | CPU bottleneck on JPEG encoding |
| 2K (2560x1440) | ~9 fps | `--encode-scale 2k` |
| 1080p (1920x1080) | ~13-30 fps | `--encode-scale 1080p` (default) |
| Raw V4L2 capture | 52 fps | Hardware is capable |
| MPP Hardware JPEG | ~18 fps | Future: mppjpegenc integration |

The bottleneck is CPU JPEG compression, not V4L2 capture. The RK3588 has hardware JPEG encoding via MPP (`mppjpegenc`) that could achieve ~18 FPS at 2K, but integrating it requires significant changes.

## Modified Files

### Core Format Support
- `src/libs/capture.c` - NV12/NV16/NV24 format detection, extended timeouts, retry logic
- `src/libs/capture.h` - Format string updates (US_FORMATS_STR)
- `src/libs/frame.c` - NV format byte calculations

### JPEG Encoding
- `src/ustreamer/encoders/cpu/encoder.c` - NV12/NV16/NV24 YCbCr encoding, flexible downscaling

### Resolution Scaling
- `src/ustreamer/encoder.c` - Global encode scale setting, scale parsing functions
- `src/ustreamer/encoder.h` - Encode scale enum and extern declaration
- `src/ustreamer/options.c` - `--encode-scale` CLI option with help text

## Building

```bash
git clone https://github.com/garagehq/ustreamer.git
cd ustreamer
make -j$(nproc)

# Install
sudo cp ustreamer /usr/local/bin/ustreamer-patched
```

## Usage with StreamSentry

StreamSentry automatically:
1. Probes the V4L2 device to detect format (NV12, BGR24, etc.)
2. Starts ustreamer-patched with the correct format
3. Uses `--encode-scale native` for automatic 4K→1080p downscaling

```bash
# Manual usage for NV12 device at 4K
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

- **MPP Hardware JPEG encoding**: The RK3588 has hardware JPEG encoding via MPP that could achieve ~18 FPS at 2K. The `mppjpegenc` GStreamer element is available but integrating it into ustreamer requires significant refactoring.

- **Dynamic format switching**: Currently format is set at startup. Could add runtime format detection when HDMI source changes.

## License

Same as upstream ustreamer - GPLv3.

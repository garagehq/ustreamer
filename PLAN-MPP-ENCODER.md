# RK3588 VPU Hardware Encoding Integration Plan

## Executive Summary

This plan outlines the integration of RK3588 hardware video encoding (VPU) into ustreamer via the Rockchip MPP (Media Process Platform) library. The goal is to achieve 2K 30fps MJPEG encoding using hardware acceleration instead of CPU encoding.

## Current Situation

### Performance Bottleneck
- **CPU JPEG encoding**: ~15 fps at 2K, ~4 fps at 4K
- **V4L2 capture**: 52 fps at 4K (hardware capable)
- **Bottleneck**: CPU JPEG compression, not capture

### RK3588 Hardware Capabilities
- Dedicated VPU for video encoding (not GPU)
- Supports H.264, H.265, VP8, and **MJPEG** encoding
- Up to 8K @ 30fps encoding capability
- MJPEG hardware encoder available via MPP

## Technical Findings

### Why NOT V4L2 M2M
The existing ustreamer M2M encoder (`m2m.c`) uses V4L2 M2M interface which works on Raspberry Pi. However, RK3588:
- **Does NOT expose V4L2 M2M encoder devices** (only `/dev/video0` for HDMI-RX capture)
- Uses MPP library (`/dev/mpp_service`) for hardware encoding
- The only video devices are:
  - `/dev/video0` - HDMI-RX capture (rk_hdmirx)
  - `/dev/video-enc0` - metadata file (not a device)
  - `/dev/video-dec0` - metadata file (not a device)

### MPP Library Details
- **Library**: `librockchip_mpp.so` (version 1.5.0)
- **Device**: `/dev/mpp_service`
- **Headers**: `/usr/include/rockchip/` (rk_mpi.h, mpp_frame.h, etc.)
- **Key Types**:
  - `MppCtx` - Encoder context
  - `MppFrame` - Input frame (NV12, BGR24, etc.)
  - `MppPacket` - Output compressed data
  - `MPP_VIDEO_CodingMJPEG` - MJPEG codec type

### Supported Input Formats
MPP encoder accepts:
- `MPP_FMT_YUV420SP` (NV12) - **Direct from HDMI-RX**
- `MPP_FMT_YUV422SP` (NV16)
- `MPP_FMT_YUV444SP` (NV24)
- `MPP_FMT_BGR888` (BGR24)
- `MPP_FMT_RGB888` (RGB24)

## Implementation Architecture

### New Encoder Module Structure
```
src/ustreamer/
├── encoders/
│   ├── cpu/
│   │   └── encoder.c      # Existing libjpeg encoder
│   └── mpp/               # NEW: MPP hardware encoder
│       ├── encoder.c      # MPP JPEG encoder implementation
│       └── encoder.h      # MPP encoder structures
├── encoder.c              # Dispatcher (add MPP type)
├── encoder.h              # Add US_ENCODER_TYPE_MPP_IMAGE
└── options.c              # Add --encoder=mpp-jpeg option
```

### Encoder Type Addition
```c
// In encoder.h
typedef enum {
    US_ENCODER_TYPE_CPU,
    US_ENCODER_TYPE_HW,
    US_ENCODER_TYPE_M2M_VIDEO,
    US_ENCODER_TYPE_M2M_IMAGE,
    US_ENCODER_TYPE_MPP_IMAGE,   // NEW: RK3588 MPP JPEG
} us_encoder_type_e;
```

### MPP Encoder API Flow
```
1. Initialization:
   ┌─────────────────────────────────────────────┐
   │ mpp_create(&ctx, &mpi)                      │
   │ mpp_init(ctx, MPP_CTX_ENC, CodingMJPEG)    │
   │ mpi->control(ctx, MPP_ENC_SET_CFG, cfg)    │
   │   - Set MppEncPrepCfg (width, height, NV12)│
   │   - Set MppEncJpegCfg (quality)            │
   └─────────────────────────────────────────────┘

2. Per-Frame Encoding:
   ┌─────────────────────────────────────────────┐
   │ mpp_frame_init(&frame)                      │
   │ mpp_frame_set_width/height/fmt(frame, ...)  │
   │ mpp_frame_set_buffer(frame, input_buffer)   │
   │                                             │
   │ mpi->encode(ctx, frame, &packet)            │
   │                                             │
   │ data = mpp_packet_get_data(packet)          │
   │ length = mpp_packet_get_length(packet)      │
   │ memcpy(dest, data, length)                  │
   │                                             │
   │ mpp_packet_deinit(&packet)                  │
   │ mpp_frame_deinit(&frame)                    │
   └─────────────────────────────────────────────┘

3. Cleanup:
   ┌─────────────────────────────────────────────┐
   │ mpp_destroy(ctx)                            │
   └─────────────────────────────────────────────┘
```

## Implementation Steps

### Phase 1: Core MPP Encoder Module

#### 1.1 Create encoder header (`src/ustreamer/encoders/mpp/encoder.h`)
```c
#pragma once

#include <stdbool.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

typedef struct {
    MppCtx      ctx;
    MppApi      *mpi;
    MppBuffer   input_buf;
    MppBufferGroup buf_grp;

    // Configuration
    uint        width;
    uint        height;
    uint        quality;
    MppFrameFormat format;

    // State
    bool        ready;
    char        *name;
} us_mpp_encoder_s;

us_mpp_encoder_s *us_mpp_jpeg_encoder_init(const char *name, uint quality);
void us_mpp_encoder_destroy(us_mpp_encoder_s *enc);
int us_mpp_encoder_compress(us_mpp_encoder_s *enc,
                            const us_frame_s *src,
                            us_frame_s *dest);
```

#### 1.2 Create encoder implementation (`src/ustreamer/encoders/mpp/encoder.c`)
Key functions:
- `us_mpp_jpeg_encoder_init()` - Create MPP context, configure for MJPEG
- `_mpp_encoder_ensure()` - Reconfigure if frame dimensions change
- `us_mpp_encoder_compress()` - Encode single frame
- `us_mpp_encoder_destroy()` - Cleanup

### Phase 2: Integration

#### 2.1 Update encoder dispatcher (`src/ustreamer/encoder.c`)
- Add MPP encoder pool creation
- Handle `US_ENCODER_TYPE_MPP_IMAGE` case

#### 2.2 Add CLI option (`src/ustreamer/options.c`)
```c
// Add to encoder type parsing
} else if (!strcasecmp(optarg, "mpp-jpeg") || !strcasecmp(optarg, "mpp-image")) {
    options->encoder_type = US_ENCODER_TYPE_MPP_IMAGE;
}
```

### Phase 3: Build System

#### 3.1 Update Makefile
```makefile
# Detect MPP library
ifneq ($(shell pkg-config --exists rockchip_mpp && echo yes),)
    WITH_MPP := 1
    CFLAGS += -DWITH_MPP=1 $(shell pkg-config --cflags rockchip_mpp)
    LDFLAGS += $(shell pkg-config --libs rockchip_mpp)
endif
```

### Phase 4: Testing

#### 4.1 Basic functionality test
```bash
# Test MPP JPEG encoder with NV12 4K input
ustreamer-patched --device=/dev/video0 --format=NV12 --resolution=3840x2160 \
    --encoder=mpp-jpeg --quality=75 --workers=1 --buffers=4
```

#### 4.2 Performance benchmarks
- Measure FPS at 4K, 2K, 1080p
- Compare with CPU encoder
- Test quality settings impact

## Expected Results

### Performance Targets
| Resolution | CPU Encoder | MPP Encoder (Expected) |
|------------|-------------|------------------------|
| 4K (3840x2160) | ~4 fps | ~15-20 fps |
| 2K (2560x1440) | ~9-15 fps | **~30 fps (target)** |
| 1080p | ~15-30 fps | ~45-60 fps |

### Based on ffmpeg-rockchip benchmarks
The ffmpeg MJPEG encoder (`mjpeg_rkmpp`) achieves good performance on RK3588, suggesting ustreamer MPP integration should reach similar results.

## Risks and Mitigations

### Risk 1: MPP MJPEG encoder quirks
- **Mitigation**: Reference ffmpeg-rockchip implementation for configuration patterns

### Risk 2: Buffer management complexity
- **Mitigation**: Start with simple memcpy, optimize with DMA buffers later

### Risk 3: Format conversion overhead
- **Mitigation**: NV12 is native for both HDMI-RX and MPP, no conversion needed

## Files to Modify/Create

### New Files
1. `src/ustreamer/encoders/mpp/encoder.h` - MPP encoder header
2. `src/ustreamer/encoders/mpp/encoder.c` - MPP encoder implementation

### Modified Files
1. `src/ustreamer/encoder.h` - Add `US_ENCODER_TYPE_MPP_IMAGE`
2. `src/ustreamer/encoder.c` - Add MPP encoder pool handling
3. `src/ustreamer/options.c` - Add `--encoder=mpp-jpeg` option
4. `Makefile` - Add MPP library detection and linking
5. `CLAUDE.md` - Document new encoder option

## Branch Strategy
```
git checkout -b feature/mpp-encoder
```

## Success Criteria
1. Hardware JPEG encoding works with NV12 input
2. 2K @ 30fps achieved with HDMI-RX input
3. Graceful fallback to CPU encoder if MPP unavailable
4. CLI option `--encoder=mpp-jpeg` functional
5. Build system detects and links librockchip_mpp

## References
- [Rockchip MPP GitHub](https://github.com/rockchip-linux/mpp)
- [MPP Development Reference](https://opensource.rock-chips.com/wiki_Mpp)
- [ffmpeg-rockchip Encoder Wiki](https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Encoder)
- [Embedfire MPP Documentation](https://doc.embedfire.com/linux/rk3588/quick_start/en/latest/lubancat_rk_software_hardware/software/mpp/mpp.html)

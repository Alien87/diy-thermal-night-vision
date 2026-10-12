// Camera module: USB host + UVC stream from the thermal camera on the
// High-Speed Type-A port. Frames are handed to a callback; nothing else in the
// app needs to know about USB.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    CAMERA_PIXFMT_OTHER = 0,
    CAMERA_PIXFMT_NV12,    // width*height luma bytes, then interleaved half-res chroma
    CAMERA_PIXFMT_YUY2,
} camera_pixfmt_t;

typedef struct {
    const uint8_t *data;   // Valid only for the duration of the callback
    size_t len;            // Bytes actually received
    uint16_t width;        // Negotiated resolution
    uint16_t height;
    camera_pixfmt_t pixfmt;
    uint32_t seq;          // Frame counter since the stream started
} camera_frame_t;

// Called from the camera task for every received frame. Keep it short: the
// next frame can't be delivered until it returns.
typedef void (*camera_frame_cb_t)(const camera_frame_t *frame, void *ctx);

// Install the USB host and UVC drivers and start waiting for a camera. The
// NV12 256x192 image stream is requested (camera default if unavailable).
// Hot-plug is handled: the stream is reopened when the camera is reconnected.
esp_err_t camera_start(camera_frame_cb_t cb, void *ctx);

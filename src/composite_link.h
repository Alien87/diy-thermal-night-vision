// Composite link module: sends the camera's luma plane to the composite
// daughter ESP32 over SPI (protocol in shared/link_protocol.h). Runs in its
// own task, so it never holds up the camera or the LCD. If the daughter is
// busy or not connected, frames are dropped.

#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t frames_sent;
    uint32_t frames_dropped;    // Sender still busy with the previous frame
    uint32_t timeouts;          // No handshake from the daughter
} composite_link_stats_t;

esp_err_t composite_link_init(void);

// Queue a 256x192 luma plane for sending. Copies the data and returns at once.
void composite_link_submit(const uint8_t *luma, int width, int height, uint8_t palette);

// Counters since the last call; resets them.
composite_link_stats_t composite_link_take_stats(void);

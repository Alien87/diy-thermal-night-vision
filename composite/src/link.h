// SPI slave receiver for frames from the ESP32-P4 (protocol and wiring in
// shared/link_protocol.h). Reassembles chunks into whole 256x192 luma frames.

#pragma once

#include <stdint.h>
#include "link_protocol.h"

struct link_stats_t {
    uint32_t frames;        // Complete frames delivered
    uint32_t chunks;        // Good chunks
    uint32_t bad_header;    // Wrong magic or chunk number
    uint32_t bad_checksum;
    uint32_t short_rx;      // Transaction shorter than a chunk
    uint32_t short_bits;    // Bits received in the last short transaction
    uint32_t partial;       // Frames abandoned with chunks missing
};

void link_begin();

// Receives chunks for up to timeout_ms. Returns true as soon as a frame is
// complete in frame (LINK_WIDTH x LINK_HEIGHT bytes); *palette is set from it.
// A partly received frame is kept across calls.
bool link_receive(uint8_t *frame, uint8_t *palette, uint32_t timeout_ms);

// Counters since the last call; resets them.
link_stats_t link_take_stats();

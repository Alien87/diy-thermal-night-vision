// Processing module: 8-bit thermal luma -> palette-mapped RGB565 image, scaled
// to the display, plus the reticle overlay. Knows nothing about USB or SPI.

#pragma once

#include <stdint.h>

typedef enum {
    PALETTE_WHITE_HOT = 0,
    PALETTE_BLACK_HOT,
    PALETTE_IRONBOW,
    PALETTE_COUNT,
} palette_id_t;

void processing_init(void);

// Safe to call from another task: takes effect from the next rendered frame.
void processing_set_palette(palette_id_t id);
palette_id_t processing_get_palette(void);
const char *processing_palette_name(palette_id_t id);

// Crop a centred square of side src_h from the src_w x src_h luma plane and
// scale it (bilinear) to fill dst_w x dst_h. Output is RGB565,
// byte-swapped for SPI panels.
void processing_render(const uint8_t *luma, int src_w, int src_h,
                       uint16_t *dst, int dst_w, int dst_h);

// Crosshair with an open centre and a centre dot, coloured to suit the palette.
void processing_draw_reticle(uint16_t *dst, int w, int h);

// Thermal palettes for the composite output, in the 256-entry RGB form taken by
// ESP_8_BIT_composite::setPalette(). Colour stops match the P4's processing.c,
// so the eyepiece and the round LCD show the same colours.
//
// Frame buffer values:
//   0-253  thermal ramp, coldest to hottest (white-hot: black to white)
//   254    reticle colour (red on the grey palettes, cyan on ironbow)
//   255    white, for overlays and text
// Map 8-bit camera luma onto 0-253 with thermal_index() before writing pixels.

#pragma once

#include <stdint.h>

enum thermal_palette_t {
    PALETTE_WHITE_HOT,
    PALETTE_BLACK_HOT,
    PALETTE_IRONBOW,
    PALETTE_COUNT,
};

static const uint8_t PALETTE_RAMP_MAX = 253;
static const uint8_t PALETTE_RETICLE = 254;
static const uint8_t PALETTE_OVERLAY = 255;

// Fills rgb with the given palette.
void palette_build(thermal_palette_t id, uint8_t rgb[256][3]);

const char *palette_name(thermal_palette_t id);

// 0-255 camera luma -> 0-253 ramp index.
static inline uint8_t thermal_index(uint8_t luma)
{
    return (uint8_t)((luma * PALETTE_RAMP_MAX + 127) / 255);
}

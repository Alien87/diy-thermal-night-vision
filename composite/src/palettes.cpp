#include "palettes.h"

struct palette_stop_t {
    float pos;
    uint8_t r, g, b;
};

// Ironbow: black -> indigo -> purple -> magenta-red -> orange -> yellow -> white
// (same stops as src/processing.c on the P4)
static const palette_stop_t IRONBOW[] = {
    { 0.00f,   0,   0,   0 },
    { 0.12f,  20,   0,  90 },
    { 0.30f, 110,   0, 150 },
    { 0.50f, 200,  30,  90 },
    { 0.65f, 240,  90,  20 },
    { 0.82f, 255, 180,   0 },
    { 0.93f, 255, 235,  90 },
    { 1.00f, 255, 255, 255 },
};

static void set_rgb(uint8_t *c, uint8_t r, uint8_t g, uint8_t b)
{
    c[0] = r;
    c[1] = g;
    c[2] = b;
}

static void build_gradient(uint8_t rgb[256][3], const palette_stop_t *stops, int n)
{
    for (int i = 0; i <= PALETTE_RAMP_MAX; i++) {
        const float t = (float)i / PALETTE_RAMP_MAX;
        int k = 0;
        while (k < n - 2 && t > stops[k + 1].pos) {
            k++;
        }
        const palette_stop_t *a = &stops[k], *b = &stops[k + 1];
        const float f = (t - a->pos) / (b->pos - a->pos);
        set_rgb(rgb[i], (uint8_t)(a->r + f * (b->r - a->r)),
                        (uint8_t)(a->g + f * (b->g - a->g)),
                        (uint8_t)(a->b + f * (b->b - a->b)));
    }
}

void palette_build(thermal_palette_t id, uint8_t rgb[256][3])
{
    switch (id) {
    case PALETTE_BLACK_HOT:
    case PALETTE_WHITE_HOT:
        for (int i = 0; i <= PALETTE_RAMP_MAX; i++) {
            uint8_t v = (uint8_t)((i * 255 + PALETTE_RAMP_MAX / 2) / PALETTE_RAMP_MAX);
            if (id == PALETTE_BLACK_HOT) {
                v = 255 - v;
            }
            set_rgb(rgb[i], v, v, v);
        }
        set_rgb(rgb[PALETTE_RETICLE], 255, 0, 0);
        break;
    case PALETTE_IRONBOW:
    default:
        build_gradient(rgb, IRONBOW, sizeof(IRONBOW) / sizeof(IRONBOW[0]));
        set_rgb(rgb[PALETTE_RETICLE], 0, 255, 255);
        break;
    }
    set_rgb(rgb[PALETTE_OVERLAY], 255, 255, 255);
}

const char *palette_name(thermal_palette_t id)
{
    switch (id) {
    case PALETTE_WHITE_HOT: return "white-hot";
    case PALETTE_BLACK_HOT: return "black-hot";
    case PALETTE_IRONBOW:   return "ironbow";
    default:                return "?";
    }
}

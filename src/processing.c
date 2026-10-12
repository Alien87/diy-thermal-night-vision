#include "processing.h"

#include <stddef.h>

#define MAX_DST_DIM 480

typedef struct {
    float pos;      // 0..1 along the palette
    uint8_t r, g, b;
} palette_stop_t;

// Ironbow: black -> indigo -> purple -> magenta-red -> orange -> yellow -> white
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

static uint16_t s_lut[PALETTE_COUNT][256];
static volatile palette_id_t s_palette = PALETTE_WHITE_HOT;  // Set by the input task

static uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

static void build_gradient(uint16_t *lut, const palette_stop_t *stops, int n)
{
    for (int i = 0; i < 256; i++) {
        const float t = i / 255.0f;
        int k = 0;
        while (k < n - 2 && t > stops[k + 1].pos) {
            k++;
        }
        const palette_stop_t *a = &stops[k], *b = &stops[k + 1];
        const float f = (t - a->pos) / (b->pos - a->pos);
        lut[i] = rgb565_be((uint8_t)(a->r + f * (b->r - a->r)),
                           (uint8_t)(a->g + f * (b->g - a->g)),
                           (uint8_t)(a->b + f * (b->b - a->b)));
    }
}

void processing_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_lut[PALETTE_WHITE_HOT][i] = rgb565_be(i, i, i);
        s_lut[PALETTE_BLACK_HOT][i] = rgb565_be(255 - i, 255 - i, 255 - i);
    }
    build_gradient(s_lut[PALETTE_IRONBOW], IRONBOW, sizeof(IRONBOW) / sizeof(IRONBOW[0]));
}

void processing_set_palette(palette_id_t id)
{
    if (id < PALETTE_COUNT) {
        s_palette = id;
    }
}

palette_id_t processing_get_palette(void)
{
    return s_palette;
}

const char *processing_palette_name(palette_id_t id)
{
    switch (id) {
    case PALETTE_WHITE_HOT: return "white-hot";
    case PALETTE_BLACK_HOT: return "black-hot";
    case PALETTE_IRONBOW:   return "ironbow";
    default:                return "?";
    }
}

// For each destination coordinate, the source pixel to its left/top and the
// 8-bit weight of the next one. Pixel centres are aligned, and the far edge is
// clamped so idx + 1 stays in range.
static void build_axis_map(int dst_len, int src_len, int src_off, uint16_t *idx, uint8_t *frac)
{
    for (int d = 0; d < dst_len; d++) {
        int pos = ((2 * d + 1) * src_len * 256) / (2 * dst_len) - 128;   // 8.8 fixed point
        if (pos < 0) {
            pos = 0;
        }
        int i = pos >> 8;
        int f = pos & 0xFF;
        if (i >= src_len - 1) {
            i = src_len - 2;
            f = 255;
        }
        idx[d] = src_off + i;
        frac[d] = f;
    }
}

void processing_render(const uint8_t *luma, int src_w, int src_h,
                       uint16_t *dst, int dst_w, int dst_h)
{
    if (dst_w > MAX_DST_DIM || dst_h > MAX_DST_DIM) {
        return;
    }
    const int crop = src_h < src_w ? src_h : src_w;

    // Bilinear: blend luma from the 4 nearest source pixels, then apply the
    // palette. Nearest neighbour at 1.25x doubles every 4th row and column,
    // which shows as stepped edges.
    uint16_t xi[MAX_DST_DIM], yi[MAX_DST_DIM];
    uint8_t xf[MAX_DST_DIM], yf[MAX_DST_DIM];
    build_axis_map(dst_w, crop, (src_w - crop) / 2, xi, xf);
    build_axis_map(dst_h, crop, (src_h - crop) / 2, yi, yf);

    const uint16_t *lut = s_lut[s_palette];
    for (int y = 0; y < dst_h; y++) {
        const uint8_t *r0 = luma + (size_t)yi[y] * src_w;
        const uint8_t *r1 = r0 + src_w;
        const uint32_t wy1 = yf[y], wy0 = 256 - wy1;
        uint16_t *out = dst + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            const int sx = xi[x];
            const uint32_t wx1 = xf[x], wx0 = 256 - wx1;
            const uint32_t top = r0[sx] * wx0 + r0[sx + 1] * wx1;
            const uint32_t bot = r1[sx] * wx0 + r1[sx + 1] * wx1;
            out[x] = lut[(top * wy0 + bot * wy1) >> 16];
        }
    }
}

void processing_draw_reticle(uint16_t *dst, int w, int h)
{
    // Red on the greyscale palettes, cyan on ironbow (which is full of reds)
    const uint16_t color = s_palette == PALETTE_IRONBOW ? rgb565_be(0, 255, 255) : rgb565_be(255, 0, 0);
    const int cx = w / 2, cy = h / 2;
    const int gap = 8;          // Open centre so the target isn't covered
    const int arm = w / 3;      // Arms stop short of the bezel

    for (int d = gap; d < arm; d++) {
        dst[cy * w + cx + d] = color;
        dst[cy * w + cx - d] = color;
        dst[(cy + d) * w + cx] = color;
        dst[(cy - d) * w + cx] = color;
    }
    for (int dy = -1; dy <= 0; dy++) {
        for (int dx = -1; dx <= 0; dx++) {
            dst[(cy + dy) * w + cx + dx] = color;
        }
    }
}

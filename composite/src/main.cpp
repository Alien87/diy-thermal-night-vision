// Composite video daughter board: receives 256x192 thermal luma frames from the
// ESP32-P4 over SPI (link.cpp) and shows them as PAL on the eyepiece.
// Wiring: GPIO25 -> 7-pin connector pin 3 (CVBS), GND -> pin 4 (GND).
// SPI wiring to the P4 is in shared/link_protocol.h.
//
// Each frame is palette-mapped, stretched from 192 to 240 rows (composite
// pixels are 1.25x wider than tall, so this gives the correct 4:3 picture),
// and gets a reticle. The palette is chosen by the P4 (its BOOT button).
//
// With no frames from the P4 for NO_SIGNAL_MS, a test pattern is shown:
//   rows   0- 99  smooth ramp, coldest to hottest, left to right
//   rows 100-159  the same ramp in 16 steps
//   rows 160-239  moving hot square on a cool background
//
// Serial monitor keys: p = next palette (test pattern only), d = toggle field dither.

#include <Arduino.h>
#include <ESP_8_BIT_composite.h>
#include "link.h"
#include "palettes.h"

static const int W = 256;
static const int H = 240;
static const uint32_t NO_SIGNAL_MS = 1000;

static ESP_8_BIT_composite video(false);  // false = PAL (matches the camera's 25 fps)

static thermal_palette_t s_palette = PALETTE_WHITE_HOT;
static bool s_dither = true;

static uint8_t s_frame[LINK_WIDTH * LINK_HEIGHT];   // Last frame from the P4
static uint8_t s_lut[256];                          // Luma -> ramp index
static uint16_t s_row_idx[H];                       // Stretch: source row above
static uint8_t s_row_frac[H];                       //          and weight of the one below

static void apply_palette(thermal_palette_t id)
{
    static uint8_t rgb[256][3];
    s_palette = id;
    palette_build(id, rgb);
    video.setPalette(rgb);
    Serial.printf("palette: %s\n", palette_name(id));
}

// Pixel centres aligned, far edge clamped so idx + 1 stays in range (as in the
// P4's processing.c).
static void build_row_map()
{
    for (int d = 0; d < H; d++) {
        int pos = ((2 * d + 1) * LINK_HEIGHT * 256) / (2 * H) - 128;  // 8.8 fixed point
        if (pos < 0) {
            pos = 0;
        }
        int i = pos >> 8;
        int f = pos & 0xFF;
        if (i >= LINK_HEIGHT - 1) {
            i = LINK_HEIGHT - 2;
            f = 255;
        }
        s_row_idx[d] = i;
        s_row_frac[d] = f;
    }
}

static void draw_reticle(uint8_t **lines)
{
    const int cx = W / 2, cy = H / 2;
    for (int i = -20; i <= 20; i++) {
        if (i > -4 && i < 4) continue;  // gap in the centre
        lines[cy][cx + i] = PALETTE_RETICLE;
        lines[cy + i][cx] = PALETTE_RETICLE;
    }
}

static void draw_camera(uint8_t **lines)
{
    for (int y = 0; y < H; y++) {
        const uint8_t *a = s_frame + s_row_idx[y] * LINK_WIDTH;
        const uint8_t *b = a + LINK_WIDTH;
        const int fb = s_row_frac[y], fa = 256 - fb;
        uint8_t *dst = lines[y];
        for (int x = 0; x < W; x++) {
            dst[x] = s_lut[(a[x] * fa + b[x] * fb) >> 8];
        }
    }
    draw_reticle(lines);
}

static void draw_test_pattern(uint8_t **lines, uint32_t frame)
{
    for (int y = 0; y < 100; y++) {
        for (int x = 0; x < W; x++) {
            lines[y][x] = thermal_index(x);
        }
    }
    for (int y = 100; y < 160; y++) {
        for (int x = 0; x < W; x++) {
            lines[y][x] = thermal_index((x / 16) * 255 / 15);
        }
    }

    const uint8_t background = thermal_index(60);
    const int sq = 16;
    const int sx = (frame * 2) % (W - sq);
    for (int y = 160; y < H; y++) {
        memset(lines[y], background, W);
        if (y >= 192 && y < 192 + sq) {
            memset(&lines[y][sx], PALETTE_RAMP_MAX, sq);
        }
    }
    draw_reticle(lines);
}

static void handle_serial()
{
    while (Serial.available()) {
        switch (Serial.read()) {
        case 'p':
            apply_palette((thermal_palette_t)((s_palette + 1) % PALETTE_COUNT));
            break;
        case 'd':
            s_dither = !s_dither;
            video.setFieldDither(s_dither);
            Serial.printf("field dither %s\n", s_dither ? "on" : "off");
            break;
        }
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("composite daughter: PAL on GPIO25");
    Serial.println("keys: p = next palette (test pattern), d = toggle field dither");
    for (int i = 0; i < 256; i++) {
        s_lut[i] = thermal_index(i);
    }
    build_row_map();
    apply_palette(s_palette);
    video.begin();
    link_begin();
}

void loop()
{
    static uint32_t pattern_frame = 0;
    static uint32_t last_frame_ms = 0;
    static uint32_t last_log_ms = 0;
    static bool live = false;

    uint8_t palette;
    if (link_receive(s_frame, &palette, 20)) {
        if (palette < PALETTE_COUNT && palette != s_palette) {
            apply_palette((thermal_palette_t)palette);
        }
        draw_camera(video.getFrameBufferLines());
        video.waitForFrame();
        last_frame_ms = millis();
        if (!live) {
            live = true;
            Serial.println("link: receiving frames");
        }
    } else if (millis() - last_frame_ms > NO_SIGNAL_MS) {
        if (live) {
            live = false;
            Serial.println("link: no frames, showing test pattern");
        }
        draw_test_pattern(video.getFrameBufferLines(), pattern_frame++);
        video.waitForFrame();
    }

    handle_serial();

    const uint32_t now = millis();
    if (now - last_log_ms >= 1000) {
        last_log_ms = now;
        const link_stats_t st = link_take_stats();
        if (live || st.chunks || st.bad_header || st.bad_checksum || st.short_rx) {
            Serial.printf("link: %u fps, %u chunks, bad header %u, bad checksum %u, short %u (last %u of %u bits), partial %u\n",
                          st.frames, st.chunks, st.bad_header, st.bad_checksum, st.short_rx,
                          st.short_bits, LINK_CHUNK_BYTES * 8, st.partial);
        }
    }
}

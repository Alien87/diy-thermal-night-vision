// Frame link from the ESP32-P4 (SPI master) to the composite daughter ESP32
// (SPI slave). Shared by both projects: the P4 build adds this folder to its
// include path in src/CMakeLists.txt, the composite build in platformio.ini.
//
// Each camera frame is the raw 256x192 8-bit luma plane, sent as LINK_CHUNKS
// transactions of LINK_CHUNK_BYTES, one CS assertion each: a link_header_t
// followed by LINK_ROWS_PER_CHUNK rows. The daughter maps luma to its palette,
// stretches 192 rows to 240 (correct 4:3 on composite) and draws the reticle.
//
// Handshake: the daughter drives HANDSHAKE high while it has a receive buffer
// loaded and low after each transaction. The master waits for a rising edge
// before every chunk, so it never clocks data into a slave that is not ready.
//
// Wiring        P4 Pico    P4 NANO        ESP32 daughter (slave, VSPI IOMUX pins)
//   SCLK        GPIO14     GPIO45    ->   IO18
//   MOSI        GPIO15     GPIO46    ->   IO23
//   CS          GPIO16     GPIO47    ->   IO5
//   HANDSHAKE   GPIO17     GPIO48    <-   IO4
//   GND         GND        GND       --   GND
// NANO pins 45-48 are in the GPIO39-48 domain, which may run at 1.8 V (LDO4);
// the link was unreliable there. The Pico pins are 3.3 V.
// The P4 pins are #defined in src/composite_link.c, the daughter's in
// composite/src/link.cpp.

#pragma once

#include <stdint.h>

#define LINK_WIDTH          256
#define LINK_HEIGHT         192
#define LINK_ROWS_PER_CHUNK 16
#define LINK_CHUNKS         (LINK_HEIGHT / LINK_ROWS_PER_CHUNK)
#define LINK_PAYLOAD_BYTES  (LINK_WIDTH * LINK_ROWS_PER_CHUNK)
#define LINK_CHUNK_BYTES    (sizeof(link_header_t) + LINK_PAYLOAD_BYTES)  // 4104, a multiple of 4 as ESP32 slave DMA needs

#define LINK_MAGIC          0x4B4C  // "LK" little-endian

// Palette numbers match palette_id_t on the P4 and thermal_palette_t on the daughter.
#define LINK_PALETTE_WHITE_HOT  0
#define LINK_PALETTE_BLACK_HOT  1
#define LINK_PALETTE_IRONBOW    2

typedef struct __attribute__((packed)) {
    uint16_t magic;     // LINK_MAGIC
    uint8_t seq;        // Frame number, wraps; all chunks of a frame share it
    uint8_t chunk;      // 0 .. LINK_CHUNKS-1, top to bottom
    uint8_t palette;    // LINK_PALETTE_*
    uint8_t flags;      // Reserved, 0
    uint16_t checksum;  // Sum of the payload bytes, mod 65536
} link_header_t;

static inline uint16_t link_checksum(const uint8_t *p, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += p[i];
    }
    return (uint16_t)sum;
}

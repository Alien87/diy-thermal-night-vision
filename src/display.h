// Display module: 1.28" 240x240 round GC9A01 on SPI, double-buffered.
// Pixels are RGB565, byte-swapped (big-endian) as the panel expects over SPI.

#pragma once

#include <stdint.h>

#include "esp_err.h"

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240

esp_err_t display_init(void);

// Buffer to render the next frame into. Not being sent while you hold it.
uint16_t *display_acquire(void);

// Send a buffer from display_acquire() to the panel. Waits for the previous
// transfer to finish, starts this one and returns while it runs.
void display_present(uint16_t *buf);

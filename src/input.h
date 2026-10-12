// Input module: buttons -> events. Nothing else needs to know about GPIOs.

#pragma once

#include "esp_err.h"

typedef enum {
    INPUT_EVENT_PALETTE_NEXT = 0,
} input_event_t;

// Called from the input task for each event.
typedef void (*input_event_cb_t)(input_event_t event, void *ctx);

esp_err_t input_start(input_event_cb_t cb, void *ctx);

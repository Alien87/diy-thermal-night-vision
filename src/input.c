#include "input.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Palette buttons, any of which cycles the palette. All are active low with the
// internal pull-up: wire a momentary push button from the GPIO to GND.
// - GPIO35: onboard BOOT button on both the ESP32-P4-NANO and ESP32-P4-Pico. It is
//   a strapping pin, but a normal input once booted (holding it during reset
//   enters download mode).
// - GPIO2: external button. On the Pico header (bottom edge; not on the NANO
//   header), 3.3 V domain, not a strapping pin, unused.
static const int PALETTE_BUTTONS[] = {35, 2};
#define NUM_BUTTONS         (sizeof(PALETTE_BUTTONS) / sizeof(PALETTE_BUTTONS[0]))

#define POLL_MS             10
#define DEBOUNCE_POLLS      3   // Level must be stable this many polls (30 ms)

static const char *TAG = "input";

static input_event_cb_t s_cb;
static void *s_ctx;

static void input_task(void *arg)
{
    bool pressed[NUM_BUTTONS] = {0};
    int stable[NUM_BUTTONS] = {0};
    while (1) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            const bool level_pressed = gpio_get_level(PALETTE_BUTTONS[i]) == 0;
            if (level_pressed != pressed[i]) {
                if (++stable[i] >= DEBOUNCE_POLLS) {
                    pressed[i] = level_pressed;
                    stable[i] = 0;
                    if (pressed[i]) {
                        s_cb(INPUT_EVENT_PALETTE_NEXT, s_ctx);
                    }
                }
            } else {
                stable[i] = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t input_start(input_event_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "callback required");
    s_cb = cb;
    s_ctx = ctx;

    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    for (int i = 0; i < NUM_BUTTONS; i++) {
        cfg.pin_bit_mask |= 1ULL << PALETTE_BUTTONS[i];
    }
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "button gpio");

    ESP_RETURN_ON_FALSE(xTaskCreate(input_task, "input", 2048, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "input task");
    ESP_LOGI(TAG, "Palette buttons on GPIO%d (BOOT) and GPIO%d", PALETTE_BUTTONS[0], PALETTE_BUTTONS[1]);
    return ESP_OK;
}

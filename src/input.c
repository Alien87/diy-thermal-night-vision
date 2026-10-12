#include "input.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// BOOT button: GPIO35 on both the ESP32-P4-NANO and ESP32-P4-Pico. It is a
// strapping pin, but a normal input once booted (holding it during reset
// enters download mode). Active low; to use an external button instead, wire
// it from a free GPIO to GND and change this.
#define PIN_PALETTE_BUTTON  35

#define POLL_MS             10
#define DEBOUNCE_POLLS      3   // Level must be stable this many polls (30 ms)

static const char *TAG = "input";

static input_event_cb_t s_cb;
static void *s_ctx;

static void input_task(void *arg)
{
    bool pressed = false;
    int stable = 0;
    while (1) {
        const bool level_pressed = gpio_get_level(PIN_PALETTE_BUTTON) == 0;
        if (level_pressed != pressed) {
            if (++stable >= DEBOUNCE_POLLS) {
                pressed = level_pressed;
                stable = 0;
                if (pressed) {
                    s_cb(INPUT_EVENT_PALETTE_NEXT, s_ctx);
                }
            }
        } else {
            stable = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t input_start(input_event_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "callback required");
    s_cb = cb;
    s_ctx = ctx;

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_PALETTE_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "button gpio");

    ESP_RETURN_ON_FALSE(xTaskCreate(input_task, "input", 2048, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "input task");
    ESP_LOGI(TAG, "Palette button on GPIO%d", PIN_PALETTE_BUTTON);
    return ESP_OK;
}

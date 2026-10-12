// Display module, based on the GC9A01 setup in ESP-IDF's spi_lcd_touch example.

#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Wiring (NANO header). All clear of the C6 SDIO, SD card, Ethernet, audio and
// strapping pins.
#define PIN_SCLK        20
#define PIN_MOSI        21
#define PIN_CS          22
#define PIN_DC          23
#define PIN_RST         32
#define PIN_BL          33

#define LCD_HOST        SPI2_HOST
#define PIXEL_CLOCK_HZ  (40 * 1000 * 1000)  // ~23 ms per full frame
#define CHUNK_BYTES     (DISPLAY_WIDTH * 80 * sizeof(uint16_t))
#define FRAME_BYTES     (DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t))

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_idle;    // Given when no frame transfer is in flight
static uint16_t *s_buf[2];
static int s_back;                  // Index of the buffer being rendered into

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_idle, &woken);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    s_idle = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_idle, ESP_ERR_NO_MEM, TAG, "semaphore");

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_buf[i], ESP_ERR_NO_MEM, TAG, "frame buffer");
    }

    const gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight gpio");
    gpio_set_level(PIN_BL, 0);

    const spi_bus_config_t bus_cfg = GC9A01_PANEL_BUS_SPI_CONFIG(PIN_SCLK, PIN_MOSI, CHUNK_BYTES);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(PIN_CS, PIN_DC, on_trans_done, NULL);
    io_cfg.pclk_hz = PIXEL_CLOCK_HZ;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io), TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel), TAG, "gc9a01");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");

    // Clear to black before switching on, so no power-on garbage shows
    s_back = 0;
    xSemaphoreGive(s_idle);
    display_present(display_acquire());
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");
    gpio_set_level(PIN_BL, 1);

    ESP_LOGI(TAG, "GC9A01 %dx%d ready, SPI %d MHz", DISPLAY_WIDTH, DISPLAY_HEIGHT, PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}

uint16_t *display_acquire(void)
{
    return s_buf[s_back];
}

void display_present(uint16_t *buf)
{
    xSemaphoreTake(s_idle, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, buf);
    s_back ^= 1;
}

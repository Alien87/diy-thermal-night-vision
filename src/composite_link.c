#include "composite_link.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_protocol.h"

// Wiring, see shared/link_protocol.h. Clear of the LCD (20-23, 32, 33), SD card,
// audio and strapping pins.
#ifdef BOARD_ESP32_P4_PICO
// 3.3 V pins; free on the Pico, which has no C6
#define PIN_SCLK        14
#define PIN_MOSI        15
#define PIN_CS          16
#define PIN_HANDSHAKE   17
#else
// NANO header. GPIO39-48 may be a 1.8 V domain (LDO4) and GPIO45 also gates
// the SD card power switch: suspected cause of the lost clock edges.
#define PIN_SCLK        45
#define PIN_MOSI        46
#define PIN_CS          47
#define PIN_HANDSHAKE   48
#endif

#define LINK_HOST       SPI3_HOST           // SPI2 drives the LCD
#define LINK_CLOCK_HZ   (20 * 1000 * 1000)  // ~20 ms per frame (10 MHz gave only 13 fps). Drop to 1 MHz to test bad wiring.
#define HANDSHAKE_MS    100                 // Give up on a frame if the daughter stays busy this long

static const char *TAG = "link";

static spi_device_handle_t s_dev;
static SemaphoreHandle_t s_ready;       // Given on each handshake rising edge
static TaskHandle_t s_task;
static uint8_t *s_frame;                // Frame being sent (LINK_WIDTH x LINK_HEIGHT)
static uint8_t *s_tx;                   // One chunk, DMA-capable
static volatile bool s_busy;
static volatile uint8_t s_palette;
static uint8_t s_seq;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static composite_link_stats_t s_stats;

static void IRAM_ATTR on_handshake(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_ready, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void count(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stats_lock);
    (*counter)++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static bool send_frame(void)
{
    link_header_t *hdr = (link_header_t *)s_tx;
    uint8_t *payload = s_tx + sizeof(link_header_t);

    for (int c = 0; c < LINK_CHUNKS; c++) {
        memcpy(payload, s_frame + c * LINK_PAYLOAD_BYTES, LINK_PAYLOAD_BYTES);
        hdr->magic = LINK_MAGIC;
        hdr->seq = s_seq;
        hdr->chunk = c;
        hdr->palette = s_palette;
        hdr->flags = 0;
        hdr->checksum = link_checksum(payload, LINK_PAYLOAD_BYTES);

        if (xSemaphoreTake(s_ready, pdMS_TO_TICKS(HANDSHAKE_MS)) != pdTRUE) {
            return false;
        }
        spi_transaction_t t = {
            .length = LINK_CHUNK_BYTES * 8,
            .tx_buffer = s_tx,
        };
        ESP_ERROR_CHECK(spi_device_transmit(s_dev, &t));
    }
    s_seq++;
    return true;
}

static void link_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // The daughter may have come up (handshake high) without us seeing the edge
        if (gpio_get_level(PIN_HANDSHAKE)) {
            xSemaphoreGive(s_ready);
        }
        count(send_frame() ? &s_stats.frames_sent : &s_stats.timeouts);
        s_busy = false;
    }
}

esp_err_t composite_link_init(void)
{
    s_ready = xSemaphoreCreateBinary();
    s_frame = heap_caps_malloc(LINK_WIDTH * LINK_HEIGHT, MALLOC_CAP_INTERNAL);
    s_tx = heap_caps_malloc(LINK_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_ready && s_frame && s_tx, ESP_ERR_NO_MEM, TAG, "buffers");

    // Pulled down so a missing or unpowered daughter reads as "not ready"
    const gpio_config_t hs_cfg = {
        .pin_bit_mask = 1ULL << PIN_HANDSHAKE,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&hs_cfg), TAG, "handshake gpio");
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_HANDSHAKE, on_handshake, NULL), TAG, "handshake isr");

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LINK_CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LINK_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = LINK_CLOCK_HZ,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LINK_HOST, &dev_cfg, &s_dev), TAG, "spi device");

    ESP_RETURN_ON_FALSE(xTaskCreate(link_task, "link", 3072, NULL, 5, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "link task");
    ESP_LOGI(TAG, "Composite link on SPI3: SCLK %d, MOSI %d, CS %d, handshake %d, %d MHz",
             PIN_SCLK, PIN_MOSI, PIN_CS, PIN_HANDSHAKE, LINK_CLOCK_HZ / 1000000);
    return ESP_OK;
}

void composite_link_submit(const uint8_t *luma, int width, int height, uint8_t palette)
{
    if (!s_task || width != LINK_WIDTH || height != LINK_HEIGHT) {
        return;
    }
    if (s_busy) {
        count(&s_stats.frames_dropped);
        return;
    }
    s_busy = true;
    memcpy(s_frame, luma, LINK_WIDTH * LINK_HEIGHT);
    s_palette = palette;
    xTaskNotifyGive(s_task);
}

composite_link_stats_t composite_link_take_stats(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    const composite_link_stats_t st = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);
    return st;
}

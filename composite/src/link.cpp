#include "link.h"

#include <Arduino.h>
#include <string.h>
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"

// VSPI IOMUX pins (fastest path for the SPI slave). MISO (IO19) is not used.
static const int PIN_SCLK = 18;
static const int PIN_MOSI = 23;
static const int PIN_CS = 5;
static const int PIN_HANDSHAKE = 4;

#define LINK_HOST   SPI3_HOST   // VSPI
#define QUEUE_DEPTH 3           // Chunks the master can send while we are busy drawing

static uint8_t *s_buf[QUEUE_DEPTH];
static spi_slave_transaction_t s_trans[QUEUE_DEPTH];
static link_stats_t s_stats;
static int s_seq = -1;          // Frame being assembled, -1 = none
static uint32_t s_have;         // Bit per received chunk of s_seq

// Handshake high while a receive buffer is loaded, low once it has been used
static void IRAM_ATTR on_post_setup(spi_slave_transaction_t *t)
{
    WRITE_PERI_REG(GPIO_OUT_W1TS_REG, 1u << PIN_HANDSHAKE);
}

static void IRAM_ATTR on_post_trans(spi_slave_transaction_t *t)
{
    WRITE_PERI_REG(GPIO_OUT_W1TC_REG, 1u << PIN_HANDSHAKE);
}

void link_begin()
{
    pinMode(PIN_HANDSHAKE, OUTPUT);
    digitalWrite(PIN_HANDSHAKE, LOW);

    // Keep the inputs at idle levels when the P4 is unplugged
    gpio_set_pull_mode((gpio_num_t)PIN_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_CS, GPIO_PULLUP_ONLY);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = PIN_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = LINK_CHUNK_BYTES;

    spi_slave_interface_config_t dev = {};
    dev.spics_io_num = PIN_CS;
    dev.queue_size = QUEUE_DEPTH;
    dev.mode = 0;
    dev.post_setup_cb = on_post_setup;
    dev.post_trans_cb = on_post_trans;

    ESP_ERROR_CHECK(spi_slave_initialize(LINK_HOST, &bus, &dev, SPI_DMA_CH_AUTO));

    for (int i = 0; i < QUEUE_DEPTH; i++) {
        s_buf[i] = (uint8_t *)heap_caps_malloc(LINK_CHUNK_BYTES, MALLOC_CAP_DMA);
        if (!s_buf[i]) {
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
        }
        memset(&s_trans[i], 0, sizeof(s_trans[i]));
        s_trans[i].length = LINK_CHUNK_BYTES * 8;
        s_trans[i].rx_buffer = s_buf[i];
        ESP_ERROR_CHECK(spi_slave_queue_trans(LINK_HOST, &s_trans[i], portMAX_DELAY));
    }
    Serial.printf("link: SPI slave on VSPI (SCLK %d, MOSI %d, CS %d), handshake IO%d\n",
                  PIN_SCLK, PIN_MOSI, PIN_CS, PIN_HANDSHAKE);
}

// Returns true when this chunk completed a frame
static bool take_chunk(const spi_slave_transaction_t *t, uint8_t *frame, uint8_t *palette)
{
    if (t->trans_len < LINK_CHUNK_BYTES * 8) {
        s_stats.short_rx++;
        s_stats.short_bits = t->trans_len;
        return false;
    }
    const uint8_t *buf = (const uint8_t *)t->rx_buffer;
    link_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    const uint8_t *payload = buf + sizeof(hdr);

    if (hdr.magic != LINK_MAGIC || hdr.chunk >= LINK_CHUNKS) {
        s_stats.bad_header++;
        return false;
    }
    if (hdr.checksum != link_checksum(payload, LINK_PAYLOAD_BYTES)) {
        s_stats.bad_checksum++;
        return false;
    }
    s_stats.chunks++;

    if (hdr.seq != s_seq) {
        if (s_have) {
            s_stats.partial++;
        }
        s_seq = hdr.seq;
        s_have = 0;
    }
    memcpy(frame + hdr.chunk * LINK_PAYLOAD_BYTES, payload, LINK_PAYLOAD_BYTES);
    s_have |= 1u << hdr.chunk;

    if (s_have == (1u << LINK_CHUNKS) - 1) {
        *palette = hdr.palette;
        s_seq = -1;
        s_have = 0;
        s_stats.frames++;
        return true;
    }
    return false;
}

bool link_receive(uint8_t *frame, uint8_t *palette, uint32_t timeout_ms)
{
    const uint32_t start = millis();
    while (true) {
        const uint32_t elapsed = millis() - start;
        if (elapsed > timeout_ms) {
            return false;
        }
        spi_slave_transaction_t *t;
        if (spi_slave_get_trans_result(LINK_HOST, &t, pdMS_TO_TICKS(timeout_ms - elapsed) + 1) != ESP_OK) {
            return false;
        }
        const bool done = take_chunk(t, frame, palette);
        ESP_ERROR_CHECK(spi_slave_queue_trans(LINK_HOST, t, portMAX_DELAY));
        if (done) {
            return true;
        }
    }
}

link_stats_t link_take_stats()
{
    const link_stats_t st = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
    return st;
}

// Thermal scope: camera (USB UVC) -> processing (palette, scale, reticle) ->
// display (GC9A01); the raw luma also goes to the composite daughter board
// (composite_link). Input (BOOT button) cycles the palette on both. Logs frame
// rate, throughput, render time and link counters once a second.

#include <inttypes.h>
#include <stdio.h>

#include "camera.h"
#include "composite_link.h"
#include "display.h"
#include "input.h"
#include "processing.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "thermal";

static void log_chip_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    // info.revision is major * 100 + minor (e.g. 100 = v1.0, 3 = v0.3)
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip: %s, %d core(s), revision v%d.%d",
             CONFIG_IDF_TARGET, info.cores,
             info.revision / 100, info.revision % 100);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "Flash size: unknown");
    }

    ESP_LOGI(TAG, "Free heap: internal %u bytes, PSRAM %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

typedef struct {
    int64_t window_start_us;
    uint32_t frames;
    uint64_t bytes;
    int64_t render_us;      // Render time summed over the window
} stream_stats_t;

static void log_frame_size(const camera_frame_t *frame)
{
    const size_t nv12 = (size_t)frame->width * frame->height * 3 / 2;
    if (frame->len == nv12) {
        ESP_LOGI(TAG, "Frame %" PRIu32 ": %ux%u, %u bytes, as expected for NV12",
                 frame->seq, frame->width, frame->height, (unsigned)frame->len);
    } else {
        ESP_LOGW(TAG, "Frame %" PRIu32 ": %ux%u, %u bytes, NV12 needs %u",
                 frame->seq, frame->width, frame->height, (unsigned)frame->len, (unsigned)nv12);
    }
}

static void on_frame(const camera_frame_t *frame, void *ctx)
{
    stream_stats_t *st = ctx;
    const int64_t t0 = esp_timer_get_time();

    // Frame 0 may be partial (stream joined mid-frame), so check a few
    if (frame->seq < 3) {
        log_frame_size(frame);
    }
    if (frame->seq == 0) {
        st->window_start_us = t0;
        st->frames = 0;
        st->bytes = 0;
        st->render_us = 0;
    }

    if (frame->pixfmt == CAMERA_PIXFMT_NV12 && frame->len >= (size_t)frame->width * frame->height) {
        composite_link_submit(frame->data, frame->width, frame->height, processing_get_palette());
        uint16_t *fb = display_acquire();
        processing_render(frame->data, frame->width, frame->height, fb, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        processing_draw_reticle(fb, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        st->render_us += esp_timer_get_time() - t0;
        display_present(fb);
    }

    st->frames++;
    st->bytes += frame->len;

    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - st->window_start_us;
    if (elapsed >= 1000000) {
        const float secs = elapsed / 1e6f;
        const composite_link_stats_t link = composite_link_take_stats();
        ESP_LOGI(TAG, "%.1f fps, %.2f MB/s, render %.2f ms/frame | link: %" PRIu32 " sent, %"
                 PRIu32 " dropped, %" PRIu32 " timeouts",
                 st->frames / secs, st->bytes / secs / (1024 * 1024),
                 st->render_us / 1000.0f / st->frames,
                 link.frames_sent, link.frames_dropped, link.timeouts);
        st->window_start_us = now;
        st->frames = 0;
        st->bytes = 0;
        st->render_us = 0;
    }
}

static void on_input(input_event_t event, void *ctx)
{
    if (event == INPUT_EVENT_PALETTE_NEXT) {
        const palette_id_t next = (processing_get_palette() + 1) % PALETTE_COUNT;
        processing_set_palette(next);
        ESP_LOGI(TAG, "Palette: %s", processing_palette_name(next));
    }
}

void app_main(void)
{
    log_chip_info();

    processing_init();
    processing_set_palette(PALETTE_WHITE_HOT);
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(composite_link_init());
    ESP_ERROR_CHECK(input_start(on_input, NULL));

    static stream_stats_t stats;
    ESP_ERROR_CHECK(camera_start(on_frame, &stats));
}

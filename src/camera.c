// Camera module, based on Espressif's usb_host_uvc basic_uvc_stream example.

#include "camera.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#define USB_HOST_PRIORITY   15
#define FRAME_BUFFERS       3
#define URB_COUNT           4
#define URB_SIZE            (10 * 1024)

// The camera's image-only mode (temperature data is not used)
#define IMAGE_WIDTH         256
#define IMAGE_HEIGHT        192

// dwFrameInterval is in 100 ns units
#define INTERVAL_TO_FPS(i)  ((i) ? 10000000.0f / (float)(i) : 0.0f)

static const char *TAG = "camera";

typedef struct {
    uint8_t dev_addr;
    uint8_t stream_index;
    size_t frame_info_num;
} connect_info_t;

static QueueHandle_t s_connect_q;   // connect_info_t from the UVC driver event
static QueueHandle_t s_frame_q;     // uvc_host_frame_t * waiting for the camera task
static volatile bool s_disconnected;
static camera_frame_cb_t s_frame_cb;
static void *s_frame_ctx;

static const char *format_name(enum uvc_host_stream_format f)
{
    switch (f) {
    case UVC_VS_FORMAT_MJPEG: return "MJPEG";
    case UVC_VS_FORMAT_YUY2:  return "YUY2";
    case UVC_VS_FORMAT_H264:  return "H264";
    case UVC_VS_FORMAT_H265:  return "H265";
    case UVC_VS_FORMAT_NV12:  return "NV12";
    default:                  return "other/unknown GUID";
    }
}

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }
    const connect_info_t info = {
        .dev_addr = event->device_connected.dev_addr,
        .stream_index = event->device_connected.uvc_stream_index,
        .frame_info_num = event->device_connected.frame_info_num,
    };
    ESP_LOGI(TAG, "UVC device connected: addr %u, stream %u, %u frame descriptor(s)",
             info.dev_addr, info.stream_index, (unsigned)info.frame_info_num);
    if (xQueueSend(s_connect_q, &info, 0) != pdPASS) {
        ESP_LOGW(TAG, "Already handling a stream, ignoring this one");
    }
}

static bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    if (xQueueSend(s_frame_q, &frame, 0) != pdPASS) {
        return true;  // Queue full: drop it and give the buffer straight back
    }
    return false;     // Returned later with uvc_host_frame_return()
}

static void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB transfer error: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Camera disconnected");
        s_disconnected = true;
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame larger than buffer, dropped");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "No free frame buffer, frame dropped");
        break;
    default:
        break;
    }
}

// Log every frame descriptor the camera offers and pick the image stream:
// NV12 at IMAGE_WIDTH x IMAGE_HEIGHT (what ffplay uses on the PC). Falls back to
// the camera's default format. YUY2 modes other than the first descriptor's
// can't be selected: the driver only negotiates the first format of each type.
static uvc_host_stream_format_t choose_format(const connect_info_t *info)
{
    uvc_host_stream_format_t fmt = { .format = UVC_VS_FORMAT_DEFAULT };

    size_t n = info->frame_info_num;
    uvc_host_frame_info_t *list = calloc(n ? n : 1, sizeof(*list));
    if (!list || uvc_host_get_frame_list(info->dev_addr, info->stream_index,
                                         (uvc_host_frame_info_t (*)[])list, &n) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read frame list");
        free(list);
        return fmt;
    }

    for (int i = 0; i < (int)n; i++) {
        const uvc_host_frame_info_t *f = &list[i];
        char rates[96];
        if (f->interval_type == 0) {
            snprintf(rates, sizeof(rates), "continuous %.1f-%.1f fps",
                     INTERVAL_TO_FPS(f->interval_max), INTERVAL_TO_FPS(f->interval_min));
        } else {
            int pos = snprintf(rates, sizeof(rates), "discrete");
            for (int k = 0; k < f->interval_type && k < CONFIG_UVC_INTERVAL_ARRAY_SIZE
                    && pos < (int)sizeof(rates); k++) {
                pos += snprintf(rates + pos, sizeof(rates) - pos, " %.1f", INTERVAL_TO_FPS(f->interval[k]));
            }
        }
        ESP_LOGI(TAG, "  [%d] %s %ux%u, default %.1f fps, %s", i, format_name(f->format),
                 f->h_res, f->v_res, INTERVAL_TO_FPS(f->default_interval), rates);

        if (fmt.format == UVC_VS_FORMAT_DEFAULT && f->format == UVC_VS_FORMAT_NV12
                && f->h_res == IMAGE_WIDTH && f->v_res == IMAGE_HEIGHT) {
            fmt = (uvc_host_stream_format_t) {
                .h_res = f->h_res,
                .v_res = f->v_res,
                .fps = INTERVAL_TO_FPS(f->default_interval),
                .format = UVC_VS_FORMAT_NV12,
            };
            ESP_LOGI(TAG, "Selected [%d]", i);
        }
    }
    if (fmt.format == UVC_VS_FORMAT_DEFAULT) {
        ESP_LOGW(TAG, "No NV12 %dx%d mode, using the camera's default format", IMAGE_WIDTH, IMAGE_HEIGHT);
    }
    free(list);
    return fmt;
}

static void dump_client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
}

// Hex-dump the raw configuration descriptor at debug level, for diagnosing how
// the UVC driver reads the camera's formats. Uses a temporary USB host client.
// Enable with esp_log_level_set("camera", ESP_LOG_DEBUG).
static void dump_config_descriptor(uint8_t dev_addr)
{
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = dump_client_cb },
    };
    usb_host_client_handle_t client;
    if (usb_host_client_register(&client_config, &client) != ESP_OK) {
        ESP_LOGW(TAG, "Descriptor dump: client register failed");
        return;
    }
    usb_device_handle_t dev;
    if (usb_host_device_open(client, dev_addr, &dev) == ESP_OK) {
        const usb_config_desc_t *cd;
        if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK) {
            ESP_LOGD(TAG, "Configuration descriptor, %u bytes:", cd->wTotalLength);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, cd, cd->wTotalLength, ESP_LOG_DEBUG);
        }
        usb_host_device_close(client, dev);
    } else {
        ESP_LOGW(TAG, "Descriptor dump: device open failed");
    }
    usb_host_client_deregister(client);
}

static camera_pixfmt_t to_pixfmt(enum uvc_host_stream_format f)
{
    switch (f) {
    case UVC_VS_FORMAT_NV12: return CAMERA_PIXFMT_NV12;
    case UVC_VS_FORMAT_YUY2: return CAMERA_PIXFMT_YUY2;
    default:                 return CAMERA_PIXFMT_OTHER;
    }
}

static void stream_frames(uvc_host_stream_hdl_t stream)
{
    uint32_t seq = 0;
    while (!s_disconnected) {
        uvc_host_frame_t *frame;
        if (xQueueReceive(s_frame_q, &frame, pdMS_TO_TICKS(1000)) != pdPASS) {
            ESP_LOGW(TAG, "No frame for 1 s");
            continue;
        }
        const camera_frame_t out = {
            .data = frame->data,
            .len = frame->data_len,
            .width = frame->vs_format.h_res,
            .height = frame->vs_format.v_res,
            .pixfmt = to_pixfmt(frame->vs_format.format),
            .seq = seq++,
        };
        s_frame_cb(&out, s_frame_ctx);
        uvc_host_frame_return(stream, frame);
    }

    // Every frame must be returned before the stream can be closed
    uvc_host_frame_t *frame;
    while (xQueueReceive(s_frame_q, &frame, 0) == pdPASS) {
        uvc_host_frame_return(stream, frame);
    }
}

static void camera_task(void *arg)
{
    while (1) {
        connect_info_t info;
        xQueueReceive(s_connect_q, &info, portMAX_DELAY);
        dump_config_descriptor(info.dev_addr);

        const uvc_host_stream_config_t cfg = {
            .event_cb = stream_event_cb,
            .frame_cb = frame_cb,
            .usb = {
                .dev_addr = info.dev_addr,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = info.stream_index,
            },
            .vs_format = choose_format(&info),
            .advanced = {
                .number_of_frame_buffers = FRAME_BUFFERS,
                .frame_size = 0,  // Use dwMaxVideoFrameSize from negotiation
                .frame_heap_caps = MALLOC_CAP_SPIRAM,
                .number_of_urbs = URB_COUNT,
                .urb_size = URB_SIZE,
            },
        };

        s_disconnected = false;
        uvc_host_stream_hdl_t stream = NULL;
        esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &stream);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Stream open failed: %s. Replug the camera to retry.", esp_err_to_name(err));
            continue;
        }

        if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
            uvc_host_desc_print(stream);
        }

        uvc_host_stream_format_t got;
        if (uvc_host_stream_format_get(stream, &got) == ESP_OK) {
            ESP_LOGI(TAG, "Negotiated: %s %ux%u @ %.1f fps",
                     format_name(got.format), got.h_res, got.v_res, got.fps);
        }

        err = uvc_host_stream_start(stream);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Streaming");
            stream_frames(stream);
        } else {
            ESP_LOGE(TAG, "Stream start failed: %s", esp_err_to_name(err));
        }

        err = uvc_host_stream_close(stream);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Stream close: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t camera_start(camera_frame_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "frame callback required");
    s_frame_cb = cb;
    s_frame_ctx = ctx;

    s_connect_q = xQueueCreate(1, sizeof(connect_info_t));
    s_frame_q = xQueueCreate(FRAME_BUFFERS, sizeof(uvc_host_frame_t *));
    ESP_RETURN_ON_FALSE(s_connect_q && s_frame_q, ESP_ERR_NO_MEM, TAG, "queue alloc");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = BIT0,  // High-Speed OTG controller = Type-A port
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG, "usb_host_install");

    ESP_RETURN_ON_FALSE(xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, USB_HOST_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "usb_lib task");

    const uvc_host_driver_config_t uvc_config = {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority = USB_HOST_PRIORITY + 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = driver_event_cb,
    };
    ESP_RETURN_ON_ERROR(uvc_host_install(&uvc_config), TAG, "uvc_host_install");

    ESP_RETURN_ON_FALSE(xTaskCreate(camera_task, "camera", 6144, NULL, USB_HOST_PRIORITY - 2, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "camera task");

    ESP_LOGI(TAG, "Waiting for camera on the Type-A port");
    return ESP_OK;
}

#include "camera.h"

#include "camera_config.h"

#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "camera";

esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        /* Allocate for the LARGEST preset the stream can switch to, not the
         * default one. Frame buffers are sized at init; dropping to a smaller
         * framesize later is free, but `?quality=high` raising it above the
         * allocation would overflow the buffer and corrupt frames. */
        .frame_size = CAM_FRAME_SIZE,
        .jpeg_quality = CAM_JPEG_QUALITY,
        .fb_count = CAM_FB_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* LATEST, not WHEN_EMPTY: always hand out the newest frame and discard
         * anything queued behind it. WHEN_EMPTY drains buffers in order, so the
         * viewer ends up watching the oldest frame the driver is holding — the
         * feed stays smooth but lags reality, which is useless for driving. */
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Step down to the fast streaming size now that the buffers exist. */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_framesize(sensor, CAM_STREAM_FRAME_SIZE);
    }

    ESP_LOGI(TAG, "Camera initialized — buffers for VGA, streaming at QVGA, newest-frame grab");
    return ESP_OK;
}

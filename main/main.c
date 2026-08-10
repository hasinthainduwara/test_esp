#include "camera.h"
#include "http_stream.h"
#include "motor.h"
#include "stream_server.h"
#include "uart_gateway.h"
#include "wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Robot master firmware starting (S3 motors + UART gateway)");

    ESP_ERROR_CHECK(wifi_init());

    /* Bring up HTTP/WS first so the phone can connect even if camera/UART hang. */
    ESP_ERROR_CHECK(http_stream_start());

    esp_err_t cam_err = camera_init();
    if (cam_err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed (%s) — continuing without camera", esp_err_to_name(cam_err));
    } else {
        esp_err_t stream_err = stream_server_start();
        if (stream_err != ESP_OK) {
            ESP_LOGE(TAG, "Stream server failed to start (%s)", esp_err_to_name(stream_err));
        }
    }

    esp_err_t motor_err = motor_init();
    if (motor_err != ESP_OK) {
        ESP_LOGE(TAG, "Motor init failed (%s) — continuing without motors", esp_err_to_name(motor_err));
    }

    esp_err_t uart_err = uart_gateway_init();
    if (uart_err != ESP_OK) {
        ESP_LOGE(TAG, "UART gateway init failed (%s)", esp_err_to_name(uart_err));
    }

    char ip[16];
    if (wifi_is_connected() == ESP_OK && wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK) {
        ESP_LOGI(TAG, "Ready — http://%s/  ws://%s/ws  stream :81", ip, ip);
        ESP_LOGI(TAG, "Ready — ws://%s.local/ws (same Wi-Fi network)", CONFIG_ROBOT_MDNS_HOSTNAME);
    } else {
        ESP_LOGW(TAG, "Servers up but Wi-Fi not joined yet — address will be logged once it connects");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

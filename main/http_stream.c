#include "http_stream.h"

#include <string.h>

#include "camera_config.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_log.h"
#include "sdkconfig.h"
#include "wifi.h"
#include "ws_server.h"

static const char *TAG = "http_stream";

/* Video runs on its own HTTP server (port+1) with its own task so the
 * blocking MJPEG loop cannot starve WebSocket command processing on port 80. */
#define STREAM_SERVER_PORT (CONFIG_ROBOT_STREAM_PORT + 1)

static httpd_handle_t s_server = NULL;        /* control: /, /health, /ws, ... */
static httpd_handle_t s_stream_server = NULL; /* video: /stream */
static volatile int s_stream_clients = 0;
static volatile bool s_stream_enabled = true;

bool http_stream_is_active(void)
{
    return s_stream_clients > 0 && s_stream_enabled;
}

void http_stream_set_enabled(bool enabled)
{
    s_stream_enabled = enabled;
    ESP_LOGI(TAG, "MJPEG stream %s", enabled ? "enabled" : "paused");
}

bool http_stream_is_enabled(void)
{
    return s_stream_enabled;
}

#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BOUNDARY     "\r\n--frame\r\n"
#define STREAM_PART         "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static void stream_apply_quality(httpd_req_t *req)
{
    char query[64] = {0};
    char quality[16] = "medium";

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "quality", value, sizeof(value)) == ESP_OK) {
            strncpy(quality, value, sizeof(quality) - 1);
        }
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return;
    }

    if (strcmp(quality, "low") == 0) {
        sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        sensor->set_quality(sensor, 20);
        ESP_LOGI(TAG, "Stream quality: low (QVGA)");
    } else if (strcmp(quality, "high") == 0) {
        sensor->set_framesize(sensor, CAM_FRAME_SIZE);
        sensor->set_quality(sensor, 12);
        ESP_LOGI(TAG, "Stream quality: high (VGA)");
    } else {
        sensor->set_framesize(sensor, CAM_FRAME_SIZE);
        sensor->set_quality(sensor, CAM_JPEG_QUALITY);
        ESP_LOGI(TAG, "Stream quality: medium (VGA)");
    }
}

static esp_err_t health_handler(httpd_req_t *req)
{
    net_log_http_rx(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    net_log_http_rx(req);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t res = ESP_OK;
    if (fb->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(fb);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    res = httpd_resp_set_type(req, "image/jpeg");
    if (res == ESP_OK) {
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    net_log_http_rx(req);
    s_stream_clients++;
    ESP_LOGI(TAG, "Stream client connected (%d active)", s_stream_clients);

    stream_apply_quality(req);

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        s_stream_clients--;
        return res;
    }

    char part_buf[64];

    while (true) {
        /* Temporary pause via WS `stream` action — keep connection, skip frames. */
        if (!s_stream_enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(CAM_STREAM_FRAME_MS));
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            esp_camera_fb_return(fb);
            res = ESP_FAIL;
            break;
        }

        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            ESP_LOGD(TAG, "Stream ended (client disconnected or send backlog)");
            res = ESP_OK;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(CAM_STREAM_FRAME_MS));
    }

    s_stream_clients--;
    ESP_LOGI(TAG, "Stream client disconnected (%d active)", s_stream_clients);
    return ESP_OK;
}

static esp_err_t info_handler(httpd_req_t *req)
{
    net_log_http_rx(req);
    char ip[16] = "0.0.0.0";
    wifi_get_ip_str(ip, sizeof(ip));

    char json[400];
    snprintf(json, sizeof(json),
             "{\"ip\":\"%s\",\"port\":%d,\"stream_port\":%d,"
             "\"health_path\":\"/health\",\"ws_path\":\"/ws\","
             "\"stream_path\":\"/stream\",\"capture_path\":\"/capture\","
             "\"health_url\":\"http://%s:%d/health\","
             "\"ws_url\":\"ws://%s:%d/ws\","
             "\"stream_url\":\"http://%s:%d/stream\",\"capture_url\":\"http://%s:%d/capture\"}",
             ip, CONFIG_ROBOT_STREAM_PORT, STREAM_SERVER_PORT,
             ip, CONFIG_ROBOT_STREAM_PORT,
             ip, CONFIG_ROBOT_STREAM_PORT,
             ip, STREAM_SERVER_PORT, ip, CONFIG_ROBOT_STREAM_PORT);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    net_log_http_rx(req);
    const char *html =
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Robot Camera</title></head><body>"
        "<h1>Robot Camera</h1>"
        "<p><a href=\"http://192.168.4.1:81/stream\">MJPEG stream</a> | "
        "<a href=\"/capture\">Snapshot</a> | "
        "<a href=\"/api/info\">API info (JSON)</a></p>"
        "<img src=\"http://192.168.4.1:81/stream\" style=\"max-width:100%\">"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_stream_start(void)
{
    /* Control server: WebSocket commands, health, capture, info. Port 80. */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_ROBOT_STREAM_PORT;
    config.ctrl_port = 32768;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start control HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
    };
    httpd_uri_t info_uri = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_handler,
    };

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &health_uri);
    httpd_register_uri_handler(s_server, &capture_uri);
    httpd_register_uri_handler(s_server, &info_uri);

    ESP_ERROR_CHECK(ws_server_register(s_server));

    /* Stream server: dedicated task so the MJPEG loop cannot block commands. */
    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = STREAM_SERVER_PORT;
    stream_config.ctrl_port = 32769;
    stream_config.stack_size = 8192;
    stream_config.max_uri_handlers = 1;
    stream_config.lru_purge_enable = true;

    if (httpd_start(&s_stream_server, &stream_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start stream HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
    };
    httpd_register_uri_handler(s_stream_server, &stream_uri);

    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, "Control server on http://%s:%d (health: /health, ws: /ws)", ip,
             CONFIG_ROBOT_STREAM_PORT);
    ESP_LOGI(TAG, "Stream server on http://%s:%d/stream", ip, STREAM_SERVER_PORT);

    return ESP_OK;
}

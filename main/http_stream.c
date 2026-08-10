#include "http_stream.h"

#include <string.h>

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "net_log.h"
#include "sdkconfig.h"
#include "stream_server.h"
#include "wifi.h"
#include "ws_server.h"

static const char *TAG = "http_stream";

static httpd_handle_t s_server = NULL; /* control: /, /health, /capture, /ws */

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

/* Video lives in stream_server.c on port STREAM_SERVER_PORT — deliberately not
 * an httpd handler. An MJPEG handler never returns, and esp_http_server runs one
 * request at a time per server, so a single viewer would monopolise the task and
 * leave every other viewer waiting indefinitely. */

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
    char ip[16] = "0.0.0.0";
    wifi_get_ip_str(ip, sizeof(ip));

    char html[512];
    snprintf(html, sizeof(html),
             "<!DOCTYPE html><html><head><meta charset=utf-8>"
             "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
             "<title>Robot Camera</title></head><body>"
             "<h1>Robot Camera</h1>"
             "<p>IP %s &middot; WebSocket <code>ws://%s:%d/ws</code></p>"
             "<p><a href=\"http://%s:%d/stream\">MJPEG stream</a> | "
             "<a href=\"/capture\">Snapshot</a> | "
             "<a href=\"/api/info\">API info (JSON)</a></p>"
             "<img src=\"http://%s:%d/stream\" style=\"max-width:100%%\">"
             "</body></html>",
             ip, ip, CONFIG_ROBOT_STREAM_PORT,
             ip, STREAM_SERVER_PORT, ip, STREAM_SERVER_PORT);

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
    /* Two servers plus mDNS share CONFIG_LWIP_MAX_SOCKETS; keep each one inside
     * its budget so accept() never fails for want of a socket. */
    config.max_open_sockets = 6;
    /* Generous on purpose: a short timeout becomes EAGAIN, which httpd treats as
     * a fatal send error and uses to drop the session — that showed up as failed
     * command acks and a torn-down WebSocket whenever the video saturated the
     * link. Long enough to only catch a peer that is genuinely gone. */
    config.send_wait_timeout = 10;
    config.recv_wait_timeout = 10;
    /* Commands outrank video: this task preempts the stream task, and the two
     * sit on separate cores so a frame being encoded and pushed never delays a
     * key press. Core 0 also keeps commands off the core the camera driver and
     * MJPEG loop are busy saturating. */
    config.task_priority = 6;
    config.core_id = 0;

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

    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, "Control server on http://%s:%d (health: /health, ws: /ws)", ip,
             CONFIG_ROBOT_STREAM_PORT);

    return ESP_OK;
}

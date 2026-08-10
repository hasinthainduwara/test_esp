#include "ws_server.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_stream.h"
#include "cmd_handler.h"
#include "net_log.h"
#include "slave_telemetry.h"

static const char *TAG = "ws_server";

#define WS_MAX_CLIENTS 4
#define WS_TELEMETRY_INTERVAL_MS 200

static httpd_handle_t s_server;
static int s_clients[WS_MAX_CLIENTS];
static portMUX_TYPE s_clients_lock = portMUX_INITIALIZER_UNLOCKED;

static void ws_client_add(int fd)
{
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            portEXIT_CRITICAL(&s_clients_lock);
            return;
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i] < 0) {
            s_clients[i] = fd;
            portEXIT_CRITICAL(&s_clients_lock);
            return;
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
    ESP_LOGW(TAG, "WebSocket client list full, fd=%d not tracked", fd);
}

static void ws_client_remove(int fd)
{
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = -1;
            break;
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
}

static int ws_get_client_rssi(void)
{
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK || sta_list.num == 0) {
        return -50;
    }
    return sta_list.sta[0].rssi;
}

static int ws_build_telemetry_json(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ts_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int rssi = ws_get_client_rssi();
    bool streaming = http_stream_is_active();

    slave_telemetry_t slave = {0};
    slave_telemetry_get(&slave);
    bool slave_online = slave_telemetry_is_online(3000);

    const char *stability = slave.status[0] != '\0' ? slave.status : "STABLE";
    if (strcmp(stability, "OK") == 0) {
        stability = "STABLE";
    }

    return snprintf(buf, len,
                    "{\"type\":\"telemetry\",\"ts\":%lld,\"robotId\":\"HVAC-Robot-001\","
                    "\"mode\":\"MANUAL\","
                    "\"left_distance\":%.1f,\"right_distance\":%.1f,"
                    "\"roll\":%.1f,\"pitch\":%.1f,"
                    "\"status\":\"%s\",\"adjustment\":\"%s\","
                    "\"slave_connected\":%s,"
                    "\"sensors\":{\"ultrasonic\":{\"leftCm\":%.1f,\"rightCm\":%.1f},"
                    "\"imu\":{\"rollDeg\":%.1f,\"pitchDeg\":%.1f}},"
                    "\"stability\":{\"status\":\"%s\"},"
                    "\"camera\":{\"streaming\":%s,\"signalStrengthDbm\":%d}}",
                    (long long)ts_ms,
                    slave.left_cm, slave.right_cm,
                    slave.roll_deg, slave.pitch_deg,
                    slave.status[0] ? slave.status : "OK",
                    slave.adjust[0] ? slave.adjust : "IDLE",
                    slave_online ? "true" : "false",
                    slave.left_cm, slave.right_cm,
                    slave.roll_deg, slave.pitch_deg,
                    stability,
                    streaming ? "true" : "false", rssi);
}

static esp_err_t ws_send_text(int fd, const char *text)
{
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    return httpd_ws_send_data(s_server, fd, &frame);
}

typedef struct {
    char *json;
    int fd;
} ws_async_send_ctx_t;

/* Runs on the httpd worker task via httpd_queue_work — NOT on ws_telemetry_task.
 * httpd_ws_send_data() is only safe from the connection's own handler/worker
 * context; calling it directly from another FreeRTOS task races with the
 * socket used to receive incoming commands and can wedge the connection
 * (symptom: first command works, then the client silently stops receiving
 * replies until the connection times out and drops). */
static void ws_async_send_cb(void *arg)
{
    ws_async_send_ctx_t *ctx = (ws_async_send_ctx_t *)arg;
    if (httpd_ws_get_fd_info(s_server, ctx->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        ws_client_remove(ctx->fd);
    } else if (ws_send_text(ctx->fd, ctx->json) != ESP_OK) {
        ws_client_remove(ctx->fd);
    }
    free(ctx->json);
    free(ctx);
}

static void ws_broadcast_telemetry(void)
{
    char json[512];
    int json_len = ws_build_telemetry_json(json, sizeof(json));
    if (json_len <= 0 || json_len >= (int)sizeof(json)) {
        return;
    }

    int fds[WS_MAX_CLIENTS];
    int count = 0;

    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i] >= 0) {
            fds[count++] = s_clients[i];
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);

    for (int i = 0; i < count; i++) {
        ws_async_send_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            continue;
        }
        ctx->json = strdup(json);
        ctx->fd = fds[i];
        if (!ctx->json) {
            free(ctx);
            continue;
        }
        if (httpd_queue_work(s_server, ws_async_send_cb, ctx) != ESP_OK) {
            free(ctx->json);
            free(ctx);
        }
    }
}

static void ws_telemetry_task(void *arg)
{
    (void)arg;
    while (true) {
        ws_broadcast_telemetry();
        vTaskDelay(pdMS_TO_TICKS(WS_TELEMETRY_INTERVAL_MS));
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        net_log_http_rx(req);
        ws_client_add(fd);
        ESP_LOGI(TAG, "WebSocket client connected (fd=%d)", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WebSocket frame length: %s", esp_err_to_name(ret));
        ws_client_remove(httpd_req_to_sockfd(req));
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        net_log_ws_rx(fd, ws_pkt.type, NULL, 0);
        ESP_LOGI(TAG, "WebSocket client disconnected (fd=%d)", fd);
        ws_client_remove(fd);
        return ESP_OK;
    }

    if (ws_pkt.len == 0) {
        net_log_ws_rx(fd, ws_pkt.type, NULL, 0);
        return ESP_OK;
    }

    char *buf = calloc(1, ws_pkt.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = (uint8_t *)buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        ws_client_remove(fd);
        return ret;
    }

    net_log_ws_rx(fd, ws_pkt.type, ws_pkt.payload, ws_pkt.len);
    cmd_handler_process(s_server, fd, buf);
    free(buf);
    return ESP_OK;
}

esp_err_t ws_server_register(httpd_handle_t server)
{
    s_server = server;

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_clients[i] = -1;
    }

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };

    esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws handler");
        return err;
    }

    BaseType_t created = xTaskCreate(ws_telemetry_task, "ws_telemetry", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start telemetry task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebSocket endpoint registered at /ws");
    return ESP_OK;
}

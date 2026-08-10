#include "ws_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

/* TCP_NODELAY / IPPROTO_TCP for the setsockopt calls below. */
#include "lwip/sockets.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cmd_handler.h"
#include "net_log.h"
#include "sdkconfig.h"
#include "slave_telemetry.h"
#include "stream_server.h"

static const char *TAG = "ws_server";

#define WS_MAX_CLIENTS 4
#define WS_TELEMETRY_INTERVAL_MS 500

/* Consecutive not-writable telemetry attempts before a client is dropped.
 * Tolerates a couple of ticks of Wi-Fi jitter (skip, don't drop) but gives up
 * on a client that has been unresponsive for WS_STALL_LIMIT * telemetry
 * interval (~1.5 s at the default 500 ms). */
#define WS_STALL_LIMIT 3

typedef struct {
    int fd;
    /* True while a telemetry frame for this client is queued on the httpd task
     * and has not completed. Commands are read by that same task, so a client
     * that stops draining its socket must never be given a second frame — see
     * ws_broadcast_telemetry(). */
    bool send_pending;
    /* Consecutive ticks where the socket was not writable within the check
     * window. Reset to 0 on every successful send. */
    int stall_count;
} ws_client_t;

static httpd_handle_t s_server;
static ws_client_t s_clients[WS_MAX_CLIENTS];
static portMUX_TYPE s_clients_lock = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_ROBOT_WS_TELEMETRY
static uint32_t s_telemetry_skipped = 0;
#endif

static void ws_client_add(int fd)
{
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].send_pending = false;
            s_clients[i].stall_count = 0;
            portEXIT_CRITICAL(&s_clients_lock);
            return;
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) {
            s_clients[i].fd = fd;
            s_clients[i].send_pending = false;
            s_clients[i].stall_count = 0;
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
        if (s_clients[i].fd == fd) {
            s_clients[i].fd = -1;
            s_clients[i].send_pending = false;
            s_clients[i].stall_count = 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
}

#if CONFIG_ROBOT_WS_TELEMETRY

/* Everything below to the end of ws_telemetry_task exists only for the periodic
 * telemetry push. It is compiled out by default: these sends run on the httpd
 * task that also reads commands, and that coupling is what made motor commands
 * stop arriving while the browser still showed a healthy WebSocket. */

/* Clears only the in-flight flag; the stall streak is left untouched so a
 * below-threshold "not writable yet" tick still counts toward WS_STALL_LIMIT. */
static void ws_client_clear_pending(int fd)
{
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].send_pending = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
}

/* A send actually went through: the client is alive, so forget any prior stalls. */
static void ws_client_note_success(int fd)
{
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].send_pending = false;
            s_clients[i].stall_count = 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
}

/* Records one more not-writable tick and returns the new consecutive count. */
static int ws_client_note_stall(int fd)
{
    int count = 0;
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].stall_count++;
            count = s_clients[i].stall_count;
            break;
        }
    }
    portEXIT_CRITICAL(&s_clients_lock);
    return count;
}

/* Forget the client *and* close the socket. A half-dead session left open holds
 * an lwIP socket and keeps costing the httpd task a blocking send every round. */
static void ws_client_drop(int fd, const char *why)
{
    ws_client_remove(fd);
    ESP_LOGW(TAG, "Dropping WebSocket client fd=%d (%s)", fd, why);
    httpd_sess_trigger_close(s_server, fd);
}

static int ws_get_client_rssi(void)
{
    /* Station mode: the meaningful number is our link to the router. */
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }

    /* SoftAP builds: report the first joined station instead. */
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
        return sta_list.sta[0].rssi;
    }
    return -50;
}

static int ws_build_telemetry_json(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ts_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int rssi = ws_get_client_rssi();
    bool streaming = stream_server_is_active();

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

/* select()'s timeout is enforced by the kernel/lwIP regardless of whatever
 * blocking mode or internal retry logic esp_http_server uses for the socket —
 * unlike httpd_config_t's send_wait_timeout, which was tried first here and
 * did not bound the stall (see ws_async_send_cb below). This is what actually
 * guarantees the shared httpd task never sits inside a send() call. */
static bool ws_socket_writable(int fd, int timeout_ms)
{
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int rv = select(fd + 1, NULL, &wfds, NULL, &tv);
    return rv > 0 && FD_ISSET(fd, &wfds);
}

typedef struct {
    char *json;
    int fd;
} ws_async_send_ctx_t;

/* Runs on the httpd worker task via httpd_queue_work — NOT on ws_telemetry_task.
 * httpd_ws_send_data() is only safe from the connection's own handler/worker
 * context; calling it directly from another FreeRTOS task races with the
 * socket used to receive incoming commands and can wedge the connection.
 *
 * That same task also reads every incoming command frame, for every client —
 * so before ever calling httpd_ws_send_data we confirm the socket is actually
 * writable with a hard-bounded select(). A client that isn't ready yet (a
 * couple of ticks of Wi-Fi jitter) is skipped, not sent to and not dropped;
 * one that stays unwritable for WS_STALL_LIMIT ticks in a row is assumed dead
 * and dropped so its slot and the shared task stop paying for it. */
static void ws_async_send_cb(void *arg)
{
    ws_async_send_ctx_t *ctx = (ws_async_send_ctx_t *)arg;

    if (httpd_ws_get_fd_info(s_server, ctx->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        ws_client_drop(ctx->fd, "session gone");
        free(ctx->json);
        free(ctx);
        return;
    }

    if (!ws_socket_writable(ctx->fd, 100)) {
        int stalls = ws_client_note_stall(ctx->fd);
        if (stalls >= WS_STALL_LIMIT) {
            ws_client_drop(ctx->fd, "not writable after repeated tries");
        } else {
            ws_client_clear_pending(ctx->fd);
        }
        free(ctx->json);
        free(ctx);
        return;
    }

    if (ws_send_text(ctx->fd, ctx->json) != ESP_OK) {
        ws_client_drop(ctx->fd, "telemetry send failed");
    } else {
        ws_client_note_success(ctx->fd);
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
    uint32_t skipped = 0;

    /* Claim only the clients whose previous frame has already gone out. The
     * httpd task that performs these sends is the same one that reads incoming
     * commands, so queueing a second frame for a client that is not draining
     * its socket would park that task in a blocking send and stop commands from
     * being read at all — for every client, not just the stalled one. */
    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) {
            continue;
        }
        if (s_clients[i].send_pending) {
            skipped++;
            continue;
        }
        s_clients[i].send_pending = true;
        fds[count++] = s_clients[i].fd;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    if (skipped > 0) {
        s_telemetry_skipped += skipped;
    }

    for (int i = 0; i < count; i++) {
        ws_async_send_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            ws_client_clear_pending(fds[i]);
            continue;
        }
        ctx->json = strdup(json);
        ctx->fd = fds[i];
        if (!ctx->json) {
            free(ctx);
            ws_client_clear_pending(fds[i]);
            continue;
        }
        if (httpd_queue_work(s_server, ws_async_send_cb, ctx) != ESP_OK) {
            ESP_LOGW(TAG, "Could not queue telemetry for fd=%d", fds[i]);
            free(ctx->json);
            free(ctx);
            ws_client_clear_pending(fds[i]);
        }
    }
}

static void ws_telemetry_task(void *arg)
{
    (void)arg;
    uint32_t reported_skips = 0;
    int ticks = 0;

    while (true) {
        ws_broadcast_telemetry();

        /* A steadily climbing skip count means a client is not draining its
         * socket — the one condition that used to silently stall commands. */
        if (++ticks >= 20) {
            ticks = 0;
            if (s_telemetry_skipped != reported_skips) {
                ESP_LOGW(TAG, "Telemetry frames skipped so far: %u (slow or dead client)",
                         (unsigned)s_telemetry_skipped);
                reported_skips = s_telemetry_skipped;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WS_TELEMETRY_INTERVAL_MS));
    }
}

#endif /* CONFIG_ROBOT_WS_TELEMETRY */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        net_log_http_rx(req);
        ws_client_add(fd);

        /* Deliberately no SO_SNDTIMEO. A short send timeout surfaces as EAGAIN
         * (errno 11), which esp_http_server treats as fatal: acks started
         * failing and the connection was torn down whenever video congested the
         * link. Nothing sends on this socket asynchronously any more — replies
         * go out via httpd_ws_send_frame() from inside this handler, only in
         * response to a frame we just read — so a blocking send here is bounded
         * by a live client that is already talking to us. */

        /* Commands and acks are ~100-byte writes. Nagle's algorithm holds a
         * small write back until the previous one is ACKed, which on a busy
         * link (video streaming alongside) adds tens of ms to every key press
         * and every ack. Turn it off — latency matters here, not efficiency. */
        int nodelay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
            ESP_LOGW(TAG, "Could not set TCP_NODELAY on fd=%d (errno=%d)", fd, errno);
        }

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

    /* Logged at INFO on purpose: this is the line that proves a frame actually
     * reached the firmware, separating "the app's command never arrived" from
     * "it arrived but was not dispatched". Two lines per button press. */
    ESP_LOGI(TAG, "WS frame in: fd=%d type=%d len=%u", fd, (int)ws_pkt.type, (unsigned)ws_pkt.len);

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
    cmd_handler_process(req, buf);
    free(buf);
    return ESP_OK;
}

esp_err_t ws_server_register(httpd_handle_t server)
{
    s_server = server;

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
        s_clients[i].send_pending = false;
        s_clients[i].stall_count = 0;
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

#if CONFIG_ROBOT_WS_TELEMETRY
    BaseType_t created = xTaskCreate(ws_telemetry_task, "ws_telemetry", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start telemetry task");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "Telemetry push enabled — if commands stop arriving, disable "
                  "CONFIG_ROBOT_WS_TELEMETRY (it shares the command-reading task)");
#else
    ESP_LOGI(TAG, "Telemetry push disabled — command path has the httpd task to itself");
#endif

    ESP_LOGI(TAG, "WebSocket endpoint registered at /ws");
    return ESP_OK;
}

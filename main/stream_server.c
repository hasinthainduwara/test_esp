#include "stream_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "camera_config.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "stream_srv";

#define STREAM_MAX_VIEWERS 3
/**
 * A viewer that cannot absorb one frame within this budget is dropped.
 * Deliberately generous: frames are only a few KB, so anything short enough to
 * trip on a browser's momentary hiccup costs a reconnect and a black screen,
 * which is far worse than waiting. This only needs to catch a peer that is gone.
 */
#define STREAM_FRAME_DEADLINE_MS 3000
#define STREAM_STATS_INTERVAL_US (5 * 1000 * 1000)

static const char RESP_HEADER[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
    "Cache-Control: no-store\r\n"
    "Pragma: no-cache\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "\r\n";

static const char RESP_BUSY[] =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Too many stream viewers\r\n";

static int s_viewers[STREAM_MAX_VIEWERS];
static volatile bool s_enabled = true;
static volatile int s_viewer_count = 0;

void stream_server_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "MJPEG output %s", enabled ? "resumed" : "paused");
}

bool stream_server_is_enabled(void)
{
    return s_enabled;
}

bool stream_server_is_active(void)
{
    return s_viewer_count > 0 && s_enabled;
}

/**
 * Apply `?quality=low|medium|high` from the raw request line.
 *
 * The sensor has one set of registers, so the most recent request wins for all
 * viewers — acceptable here, where extra viewers are the exception.
 */
static void apply_quality(const char *request)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !request) {
        return;
    }

    if (strstr(request, "quality=high") != NULL) {
        sensor->set_framesize(sensor, CAM_FRAME_SIZE);
        sensor->set_quality(sensor, 12);
        ESP_LOGI(TAG, "Stream quality: high (VGA) — detail over latency");
    } else if (strstr(request, "quality=low") != NULL) {
        sensor->set_framesize(sensor, FRAMESIZE_QQVGA);
        sensor->set_quality(sensor, 18);
        ESP_LOGI(TAG, "Stream quality: low (QQVGA) — lowest latency");
    } else {
        sensor->set_framesize(sensor, CAM_STREAM_FRAME_SIZE);
        sensor->set_quality(sensor, CAM_JPEG_QUALITY);
        ESP_LOGI(TAG, "Stream quality: medium (QVGA)");
    }
}

static void viewer_close(int idx)
{
    if (s_viewers[idx] >= 0) {
        close(s_viewers[idx]);
        s_viewers[idx] = -1;
        if (s_viewer_count > 0) {
            s_viewer_count--;
        }
        ESP_LOGI(TAG, "Viewer left (%d active)", s_viewer_count);
    }
}

static bool sock_wait_writable(int fd, int timeout_ms)
{
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    return select(fd + 1, NULL, &wfds, NULL, &tv) > 0;
}

typedef enum {
    SEND_OK = 0,
    SEND_TIMEOUT,   /* peer alive but not draining fast enough */
    SEND_CLOSED,    /* peer went away */
} send_result_t;

/**
 * Send the whole buffer or fail, never blocking past the deadline.
 *
 * A partially sent frame would corrupt the multipart stream, so the caller must
 * drop the viewer on failure rather than continue with the next frame. The
 * distinction in the return value matters for diagnosis: "too slow" and "gone"
 * call for completely different fixes.
 */
static send_result_t send_all(int fd, const void *data, size_t len, int64_t deadline_us)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;

    while (left > 0) {
        int n = send(fd, p, left, 0);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n == 0) {
            return SEND_CLOSED;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int64_t remain_us = deadline_us - esp_timer_get_time();
            if (remain_us <= 0) {
                return SEND_TIMEOUT;
            }
            int wait_ms = (int)(remain_us / 1000);
            if (wait_ms > 100) {
                wait_ms = 100;
            }
            sock_wait_writable(fd, wait_ms > 0 ? wait_ms : 1);
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return SEND_CLOSED;
    }
    return SEND_OK;
}

static void accept_new_viewers(int listen_fd)
{
    while (true) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
        if (fd < 0) {
            return; /* EAGAIN: nothing pending */
        }

        int slot = -1;
        for (int i = 0; i < STREAM_MAX_VIEWERS; i++) {
            if (s_viewers[i] < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            send(fd, RESP_BUSY, strlen(RESP_BUSY), 0);
            close(fd);
            ESP_LOGW(TAG, "Refused viewer — all %d slots busy", STREAM_MAX_VIEWERS);
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        /* Best-effort read of the request line. Any path on this port serves the
         * stream; only `?quality=` is looked at. The socket is non-blocking, so
         * a client that has not sent its request yet simply keeps the default. */
        char request[192] = {0};
        int got = recv(fd, request, sizeof(request) - 1, 0);
        if (got > 0) {
            request[got] = '\0';
            apply_quality(request);
        }

        int64_t deadline = esp_timer_get_time() + 1000 * 1000;
        if (send_all(fd, RESP_HEADER, strlen(RESP_HEADER), deadline) != SEND_OK) {
            close(fd);
            ESP_LOGW(TAG, "Viewer dropped before the first frame");
            continue;
        }

        s_viewers[slot] = fd;
        s_viewer_count++;
        ESP_LOGI(TAG, "Viewer joined on fd=%d (%d active)", fd, s_viewer_count);
    }
}

static void stream_task(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed (errno=%d)", errno);
        vTaskDelete(NULL);
        return;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(STREAM_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "bind(%d) failed (errno=%d)", STREAM_SERVER_PORT, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, STREAM_MAX_VIEWERS) != 0) {
        ESP_LOGE(TAG, "listen() failed (errno=%d)", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "MJPEG stream server listening on port %d", STREAM_SERVER_PORT);

    uint32_t frames = 0;
    uint32_t bytes = 0;
    int64_t stats_start = esp_timer_get_time();

    while (true) {
        accept_new_viewers(listen_fd);

        if (s_viewer_count == 0 || !s_enabled) {
            /* Nobody watching: leave the camera alone and stop burning airtime. */
            vTaskDelay(pdMS_TO_TICKS(50));
            stats_start = esp_timer_get_time();
            frames = 0;
            bytes = 0;
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(1);
            continue;
        }
        if (fb->format != PIXFORMAT_JPEG) {
            esp_camera_fb_return(fb);
            vTaskDelay(1);
            continue;
        }

        char part[96];
        int part_len = snprintf(part, sizeof(part),
                                "\r\n--frame\r\nContent-Type: image/jpeg\r\n"
                                "Content-Length: %u\r\n\r\n",
                                (unsigned)fb->len);

        int64_t deadline = esp_timer_get_time() + STREAM_FRAME_DEADLINE_MS * 1000;
        for (int i = 0; i < STREAM_MAX_VIEWERS; i++) {
            if (s_viewers[i] < 0) {
                continue;
            }
            send_result_t rc = send_all(s_viewers[i], part, (size_t)part_len, deadline);
            if (rc == SEND_OK) {
                rc = send_all(s_viewers[i], fb->buf, fb->len, deadline);
            }
            if (rc != SEND_OK) {
                ESP_LOGW(TAG, "Viewer fd=%d dropped: %s (errno=%d, frame %u B)",
                         s_viewers[i],
                         rc == SEND_TIMEOUT ? "not draining within deadline" : "connection closed",
                         errno, (unsigned)fb->len);
                viewer_close(i);
            }
        }

        frames++;
        bytes += fb->len;
        esp_camera_fb_return(fb);

        /* Integer maths on purpose: %f in a log line is not reliable when
         * newlib nano formatting is in play. */
        int64_t now = esp_timer_get_time();
        int64_t elapsed_us = now - stats_start;
        if (elapsed_us >= STREAM_STATS_INTERVAL_US) {
            uint32_t fps_x10 = (uint32_t)((int64_t)frames * 10 * 1000000 / elapsed_us);
            uint32_t kb_per_s = (uint32_t)((int64_t)bytes * 1000000 / elapsed_us / 1024);
            uint32_t avg_frame = frames > 0 ? bytes / frames : 0;
            ESP_LOGI(TAG, "%u.%u fps, %u KB/s, avg frame %u B, %d viewer(s)",
                     (unsigned)(fps_x10 / 10), (unsigned)(fps_x10 % 10),
                     (unsigned)kb_per_s, (unsigned)avg_frame, s_viewer_count);
            frames = 0;
            bytes = 0;
            stats_start = now;
        }

#if CAM_STREAM_FRAME_MS > 0
        vTaskDelay(pdMS_TO_TICKS(CAM_STREAM_FRAME_MS));
#else
        taskYIELD();
#endif
    }
}

esp_err_t stream_server_start(void)
{
    for (int i = 0; i < STREAM_MAX_VIEWERS; i++) {
        s_viewers[i] = -1;
    }

    /* Core 1, below the control server: video yields to commands, never the
     * other way round. */
    BaseType_t created = xTaskCreatePinnedToCore(stream_task, "mjpeg_stream", 5120,
                                                 NULL, 4, NULL, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start stream task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

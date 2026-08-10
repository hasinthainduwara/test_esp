#include "slave_telemetry.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "uart_protocol.h"

static slave_telemetry_t s_data;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void mark_online(void)
{
    s_data.slave_online = true;
    s_data.last_update_ms = now_ms();
}

void slave_telemetry_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_data, 0, sizeof(s_data));
    strncpy(s_data.status, "OK", sizeof(s_data.status));
    strncpy(s_data.adjust, "IDLE", sizeof(s_data.adjust));
    portEXIT_CRITICAL(&s_lock);
}

void slave_telemetry_update_from_line(const char *line)
{
    if (!line || line[0] == '\0') {
        return;
    }

    portENTER_CRITICAL(&s_lock);

    if (strcmp(line, UART_MSG_HELLO) == 0) {
        mark_online();
    } else if (strncmp(line, UART_PREFIX_HEART, strlen(UART_PREFIX_HEART)) == 0) {
        mark_online();
    } else if (strncmp(line, UART_PREFIX_ACK, strlen(UART_PREFIX_ACK)) == 0) {
        mark_online();
    } else if (strncmp(line, UART_PREFIX_DIST, strlen(UART_PREFIX_DIST)) == 0) {
        float l = 0, r = 0;
        if (sscanf(line + strlen(UART_PREFIX_DIST), "%f,%f", &l, &r) == 2) {
            s_data.left_cm = l;
            s_data.right_cm = r;
            mark_online();
        }
    } else if (strncmp(line, UART_PREFIX_IMU, strlen(UART_PREFIX_IMU)) == 0) {
        float roll = 0, pitch = 0;
        if (sscanf(line + strlen(UART_PREFIX_IMU), "ROLL=%f,PITCH=%f", &roll, &pitch) == 2) {
            s_data.roll_deg = roll;
            s_data.pitch_deg = pitch;
            mark_online();
        }
    } else if (strncmp(line, UART_PREFIX_STATUS, strlen(UART_PREFIX_STATUS)) == 0) {
        const char *val = line + strlen(UART_PREFIX_STATUS);
        strncpy(s_data.status, val, sizeof(s_data.status) - 1);
        s_data.status[sizeof(s_data.status) - 1] = '\0';
        mark_online();
    } else if (strncmp(line, UART_PREFIX_ADJUST, strlen(UART_PREFIX_ADJUST)) == 0) {
        const char *val = line + strlen(UART_PREFIX_ADJUST);
        strncpy(s_data.adjust, val, sizeof(s_data.adjust) - 1);
        s_data.adjust[sizeof(s_data.adjust) - 1] = '\0';
        mark_online();
    }

    portEXIT_CRITICAL(&s_lock);
}

void slave_telemetry_get(slave_telemetry_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_data;
    portEXIT_CRITICAL(&s_lock);
}

bool slave_telemetry_is_online(int stale_ms)
{
    slave_telemetry_t snap;
    slave_telemetry_get(&snap);
    if (!snap.slave_online) {
        return false;
    }
    return (now_ms() - snap.last_update_ms) < stale_ms;
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float left_cm;
    float right_cm;
    float roll_deg;
    float pitch_deg;
    char status[16];
    char adjust[16];
    bool slave_online;
    int64_t last_update_ms;
} slave_telemetry_t;

void slave_telemetry_init(void);
void slave_telemetry_update_from_line(const char *line);
void slave_telemetry_get(slave_telemetry_t *out);
bool slave_telemetry_is_online(int stale_ms);

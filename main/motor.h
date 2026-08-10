#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t motor_init(void);
void motor_stop(void);
void motor_emergency_stop(void);
void motor_clear_emergency_stop(void);
bool motor_is_estop(void);

/** direction: forward, reverse, left, right, stop */
esp_err_t motor_move(const char *direction);

void motor_led_set(bool on);

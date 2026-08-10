#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t uart_gateway_init(void);
esp_err_t uart_gateway_start_rx_task(void);
esp_err_t uart_gateway_send_line(const char *line);
/** Same as uart_gateway_send_line() but logged at DEBUG (for high-frequency traffic). */
esp_err_t uart_gateway_send_line_quiet(const char *line);

/** Map app direction string to UART command and send. */
esp_err_t uart_gateway_send_move(const char *direction);
esp_err_t uart_gateway_send_led(bool on);
esp_err_t uart_gateway_send_estop(bool active);
esp_err_t uart_gateway_send_adjust(void);

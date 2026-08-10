#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Parse app JSON command and execute. Sends ack/error on WebSocket when possible. */
esp_err_t cmd_handler_process(httpd_handle_t server, int client_fd, const char *json);

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

void net_log_http_rx(httpd_req_t *req);
void net_log_ws_rx(int client_fd, httpd_ws_type_t type, const uint8_t *payload, size_t len);

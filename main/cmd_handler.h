#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Parse an app JSON command and execute it, replying with ack/error.
 *
 * Takes the live request rather than (handle, fd) so replies go out through
 * httpd_ws_send_frame() — the API meant for use inside a WebSocket handler.
 * The async fd-based send is deliberately avoided on this path: it runs on the
 * same httpd task that reads incoming frames, and a send that does not return
 * there stops every command from being read.
 */
esp_err_t cmd_handler_process(httpd_req_t *req, const char *json);

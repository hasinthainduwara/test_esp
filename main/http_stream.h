#pragma once

#include "esp_err.h"

/**
 * Start the control HTTP server: `/`, `/health`, `/capture`, `/api/info` and the
 * `/ws` WebSocket upgrade.
 *
 * Video is not served here — see stream_server.h for the MJPEG port and why it
 * runs on its own plain-TCP task instead of a second esp_http_server instance.
 */
esp_err_t http_stream_start(void);

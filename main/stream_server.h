#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

/** MJPEG viewers connect here; control/WebSocket stay on CONFIG_ROBOT_STREAM_PORT. */
#define STREAM_SERVER_PORT (CONFIG_ROBOT_STREAM_PORT + 1)

/**
 * Start the dedicated MJPEG streaming task.
 *
 * Deliberately a plain TCP server rather than another esp_http_server instance:
 * that component services one request at a time per server, and an MJPEG
 * handler never returns, so a single viewer monopolises the task and every
 * other viewer waits forever. This fans one captured frame out to several
 * viewers and drops any that cannot keep up.
 */
esp_err_t stream_server_start(void);

/** Pause/resume frame output without disconnecting viewers. */
void stream_server_set_enabled(bool enabled);
bool stream_server_is_enabled(void);

/** True when at least one viewer is connected and output is not paused. */
bool stream_server_is_active(void);

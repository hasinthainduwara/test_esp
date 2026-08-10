#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t http_stream_start(void);

/** True when ≥1 MJPEG client is connected and streaming is not paused. */
bool http_stream_is_active(void);

/** Pause/resume MJPEG frame output (HTTP clients stay connected). Default: enabled. */
void http_stream_set_enabled(bool enabled);
bool http_stream_is_enabled(void);

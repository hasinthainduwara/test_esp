#pragma once

#include "esp_err.h"

/**
 * Initialize and start WiFi.
 * In AP mode, this starts the Access Point and returns ESP_OK.
 * In STA mode, this attempts to connect to the configured network and blocks
 * until a connection is established (returns ESP_OK) or fails (returns ESP_FAIL).
 */
esp_err_t wifi_init(void);

/**
 * Returns ESP_OK if WiFi is successfully connected (STA mode) or if the AP is running (AP mode).
 */
esp_err_t wifi_is_connected(void);

/**
 * Copies the WiFi IP address string into the buffer (e.g. "192.168.4.1" or "192.168.1.100").
 */
esp_err_t wifi_get_ip_str(char *buf, size_t len);

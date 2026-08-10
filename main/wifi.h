#pragma once

#include "esp_err.h"

/**
 * Initialize and start WiFi, and advertise CONFIG_ROBOT_MDNS_HOSTNAME.local.
 * In AP mode, this starts the Access Point.
 * In STA mode, this waits up to ~20 s for the first DHCP lease and then returns
 * regardless, leaving a background retry loop running — so a router that is
 * slow, down, or out of range does not stop the firmware from booting.
 * Returns ESP_OK; use wifi_is_connected() to test link state.
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

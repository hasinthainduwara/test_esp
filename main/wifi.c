#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* How long app_main waits for the first DHCP lease before booting anyway. */
#define WIFI_STA_FIRST_CONNECT_TIMEOUT_MS 20000
/* Retry pause once the fast retries are used up (router down, out of range). */
#define WIFI_STA_SLOW_RETRY_US (5 * 1000 * 1000)

static EventGroupHandle_t s_wifi_event_group;
#if CONFIG_ROBOT_WIFI_MODE_STA
static int s_retry_num = 0;
static esp_timer_handle_t s_reconnect_timer = NULL;
#endif
static bool s_connected = false;

#if CONFIG_ROBOT_WIFI_MODE_STA
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}
#endif

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
#if CONFIG_ROBOT_WIFI_MODE_STA
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        if (s_retry_num < CONFIG_ROBOT_WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected (reason %d) — retry %d/%d",
                     event ? event->reason : 0, s_retry_num, CONFIG_ROBOT_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            /* Never give up permanently: keep trying slowly so the robot comes
             * back on its own when the router returns or it is carried in range. */
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "Still down (reason %d) — retrying every %d s",
                     event ? event->reason : 0, (int)(WIFI_STA_SLOW_RETRY_US / 1000000));
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, WIFI_STA_SLOW_RETRY_US);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR " (gateway " IPSTR ")",
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
#elif CONFIG_ROBOT_WIFI_MODE_AP
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        ESP_LOGI(TAG, "AP broadcasting — BSSID " MACSTR " channel %d",
                 MAC2STR(mac), CONFIG_ROBOT_WIFI_AP_CHANNEL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " got IP " IPSTR,
                 MAC2STR(event->mac), IP2STR(&event->ip));
    }
#endif
}

/* Advertise <hostname>.local so the app never has to chase the DHCP address. */
static void wifi_start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed (%s) — use the IP address instead", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(CONFIG_ROBOT_MDNS_HOSTNAME);
    mdns_instance_name_set("ESP32 Robot");
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_ROBOT_STREAM_PORT, NULL, 0);

    ESP_LOGI(TAG, "mDNS up — http://%s.local:%d/  ws://%s.local:%d/ws",
             CONFIG_ROBOT_MDNS_HOSTNAME, CONFIG_ROBOT_STREAM_PORT,
             CONFIG_ROBOT_MDNS_HOSTNAME, CONFIG_ROBOT_STREAM_PORT);
}

esp_err_t wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_ROBOT_WIFI_MODE_STA
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
#elif CONFIG_ROBOT_WIFI_MODE_AP
    esp_netif_create_default_wifi_ap();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
#if CONFIG_ROBOT_WIFI_MODE_STA
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, &instance_got_ip));

    const esp_timer_create_args_t timer_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    /* Router credentials live in RAM only, so a re-flash with new ones always wins. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .sta = {
            /* Threshold is a *minimum* on the enum order, so WPA_PSK here admits
             * WPA2-only, WPA/WPA2 mixed and WPA3 routers alike — anything but
             * open/WEP. WPA2_PSK would reject a router advertising plain WPA. */
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, CONFIG_ROBOT_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_ROBOT_WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    if (sta_netif) {
        esp_netif_set_hostname(sta_netif, CONFIG_ROBOT_MDNS_HOSTNAME);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Power save adds up to ~100 ms of latency per command — the robot needs
     * snappy motor response far more than it needs the milliamps. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to SSID: \"%s\"", CONFIG_ROBOT_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_STA_FIRST_CONNECT_TIMEOUT_MS));

    wifi_start_mdns();

    if (bits & WIFI_CONNECTED_BIT) {
        char ip[16] = "0.0.0.0";
        wifi_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "Joined \"%s\" — open the app on the same Wi-Fi and connect to %s (or %s.local)",
                 CONFIG_ROBOT_WIFI_SSID, ip, CONFIG_ROBOT_MDNS_HOSTNAME);
    } else {
        /* Boot anyway: servers come up and the retry loop keeps working in the
         * background, so a router that is slow or briefly down is not fatal. */
        ESP_LOGW(TAG, "Not connected to \"%s\" yet — continuing to retry in the background. "
                      "Check the SSID/password in menuconfig if this never clears.",
                 CONFIG_ROBOT_WIFI_SSID);
    }
    return ESP_OK;

#elif CONFIG_ROBOT_WIFI_MODE_AP
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                         &wifi_event_handler, NULL, &instance_got_ip));

    /* RAM-only: ignore stale WiFi settings in NVS that can break the AP */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(CONFIG_ROBOT_WIFI_AP_SSID),
            .channel = CONFIG_ROBOT_WIFI_AP_CHANNEL,
            .max_connection = CONFIG_ROBOT_WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 0,
            .beacon_interval = 100,
            .pmf_cfg = {
                .capable = false,
                .required = false,
            },
        },
    };
    strncpy((char *)wifi_config.ap.ssid, CONFIG_ROBOT_WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, CONFIG_ROBOT_WIFI_AP_PASSWORD, sizeof(wifi_config.ap.password));

    if (strlen(CONFIG_ROBOT_WIFI_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(78)); /* ~19.5 dBm for better range */

    s_connected = true;
    wifi_start_mdns();
    char ip[16] = "192.168.4.1";
    wifi_get_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, "Connect phone to WiFi \"%s\" password \"%s\" then open http://%s/",
             CONFIG_ROBOT_WIFI_AP_SSID,
             strlen(CONFIG_ROBOT_WIFI_AP_PASSWORD) > 0 ? CONFIG_ROBOT_WIFI_AP_PASSWORD : "(none)",
             ip);
    return ESP_OK;
#endif
}

esp_err_t wifi_is_connected(void)
{
    return s_connected ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_get_ip_str(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ifkey = NULL;
#if CONFIG_ROBOT_WIFI_MODE_STA
    ifkey = "WIFI_STA_DEF";
#elif CONFIG_ROBOT_WIFI_MODE_AP
    ifkey = "WIFI_AP_DEF";
#endif

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!netif) {
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return ESP_FAIL;
    }

    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

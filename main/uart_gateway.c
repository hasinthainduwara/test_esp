#include "uart_gateway.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "slave_telemetry.h"
#include "uart_protocol.h"

static const char *TAG = "uart_gw";

#define UART_BUF_SIZE 1024
#define UART_PING_INTERVAL_MS 1000

esp_err_t uart_gateway_send_line(const char *line);

static void uart_rx_task(void *arg)
{
    (void)arg;
    char line[UART_LINE_MAX];
    size_t pos = 0;

    while (true) {
        uint8_t ch;
        int n = uart_read_bytes(CONFIG_MASTER_UART_PORT, &ch, 1, pdMS_TO_TICKS(50));
        if (n != 1) {
            continue;
        }

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                ESP_LOGI(TAG, "RX: %s", line);
                slave_telemetry_update_from_line(line);
            }
            pos = 0;
            continue;
        }

        if (pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        } else {
            pos = 0;
        }
    }
}

static void uart_ping_task(void *arg)
{
    (void)arg;
    bool was_online = false;

    while (true) {
        bool online = slave_telemetry_is_online(2500);
        if (online != was_online) {
            ESP_LOGI(TAG, "Slave link %s", online ? "ONLINE" : "OFFLINE");
            was_online = online;
        }
        if (!online) {
            /* Quiet on purpose — fires every second while the slave is offline
             * and would otherwise flood the log. Link state change above still logs. */
            uart_gateway_send_line_quiet(UART_CMD_PING);
        }
        vTaskDelay(pdMS_TO_TICKS(UART_PING_INTERVAL_MS));
    }
}

esp_err_t uart_gateway_start_rx_task(void)
{
    BaseType_t rx = xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, NULL);
    BaseType_t ping = xTaskCreate(uart_ping_task, "uart_ping", 3072, NULL, 5, NULL);
    if (rx != pdPASS || ping != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t uart_gateway_init(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_MASTER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* RX buffer must be large — slave sends several lines every 200 ms.
     * TX buffer lets ping/commands not block. */
    esp_err_t err = uart_driver_install(CONFIG_MASTER_UART_PORT, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(CONFIG_MASTER_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_set_pin(CONFIG_MASTER_UART_PORT,
                       CONFIG_MASTER_UART_TX_GPIO,
                       CONFIG_MASTER_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_pull_mode(CONFIG_MASTER_UART_RX_GPIO, GPIO_PULLUP_ONLY);

    slave_telemetry_init();

    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d @ %d baud — wire slave TX->GPIO%d RX->GPIO%d",
             CONFIG_MASTER_UART_PORT,
             CONFIG_MASTER_UART_TX_GPIO,
             CONFIG_MASTER_UART_RX_GPIO,
             CONFIG_MASTER_UART_BAUD,
             CONFIG_MASTER_UART_RX_GPIO,
             CONFIG_MASTER_UART_TX_GPIO);

    ESP_ERROR_CHECK(uart_gateway_start_rx_task());
    return ESP_OK;
}

static esp_err_t uart_gateway_send_line_ex(const char *line, bool log_it)
{
    if (!line || line[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[UART_LINE_MAX + 2];
    size_t len = strlen(line);
    if (line[len - 1] == '\n') {
        snprintf(buf, sizeof(buf), "%s", line);
    } else {
        snprintf(buf, sizeof(buf), "%s\n", line);
    }

    int written = uart_write_bytes(CONFIG_MASTER_UART_PORT, buf, strlen(buf));
    if (written <= 0) {
        return ESP_FAIL;
    }
    if (log_it) {
        ESP_LOGI(TAG, "TX: %s", line);
    } else {
        ESP_LOGD(TAG, "TX: %s", line);
    }
    return ESP_OK;
}

esp_err_t uart_gateway_send_line(const char *line)
{
    return uart_gateway_send_line_ex(line, true);
}

/** Same as uart_gateway_send_line() but logged at DEBUG — for high-frequency,
 * low-value traffic (e.g. the offline-slave keep-alive ping). */
esp_err_t uart_gateway_send_line_quiet(const char *line)
{
    return uart_gateway_send_line_ex(line, false);
}

esp_err_t uart_gateway_send_move(const char *direction)
{
    if (!direction) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *cmd = UART_CMD_STOP;
    if (strcmp(direction, "forward") == 0) {
        cmd = UART_CMD_FORWARD;
    } else if (strcmp(direction, "reverse") == 0) {
        cmd = UART_CMD_BACKWARD;
    } else if (strcmp(direction, "left") == 0) {
        cmd = UART_CMD_LEFT;
    } else if (strcmp(direction, "right") == 0) {
        cmd = UART_CMD_RIGHT;
    } else if (strcmp(direction, "stop") == 0) {
        cmd = UART_CMD_STOP;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return uart_gateway_send_line(cmd);
}

esp_err_t uart_gateway_send_led(bool on)
{
    return uart_gateway_send_line(on ? UART_CMD_LED_ON : UART_CMD_LED_OFF);
}

esp_err_t uart_gateway_send_estop(bool active)
{
    return uart_gateway_send_line(active ? UART_CMD_ESTOP : UART_CMD_ESTOP_CLR);
}

esp_err_t uart_gateway_send_adjust(void)
{
    return uart_gateway_send_line(UART_CMD_ADJUST);
}

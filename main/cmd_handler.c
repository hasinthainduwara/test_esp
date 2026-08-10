#include "cmd_handler.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_stream.h"
#include "motor.h"
#include "uart_gateway.h"

static const char *TAG = "cmd_handler";

static esp_err_t ws_reply(httpd_handle_t server, int client_fd, const char *json)
{
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    return httpd_ws_send_data(server, client_fd, &frame);
}

static void send_ack(httpd_handle_t server, int client_fd, const char *command_id, bool success)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"ack\",\"commandId\":\"%s\",\"success\":%s}",
             command_id != NULL ? command_id : "",
             success ? "true" : "false");
    ws_reply(server, client_fd, buf);
}

static void send_error(httpd_handle_t server, int client_fd, const char *command_id,
                       const char *code, const char *message)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"error\",\"commandId\":\"%s\",\"code\":\"%s\",\"message\":\"%s\"}",
             command_id != NULL ? command_id : "",
             code != NULL ? code : "ERROR",
             message != NULL ? message : "");
    ws_reply(server, client_fd, buf);
}

esp_err_t cmd_handler_process(httpd_handle_t server, int client_fd, const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid JSON");
        send_error(server, client_fd, NULL, "INVALID_MESSAGE", "Could not parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "command") != 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    const char *command_id = cJSON_IsString(id) ? id->valuestring : "";

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) {
        send_error(server, client_fd, command_id, "INVALID_MESSAGE", "Missing action");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *action_str = action->valuestring;
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    esp_err_t err = ESP_OK;
    bool success = true;

    if (strcmp(action_str, "move") == 0) {
        cJSON *direction = payload != NULL ? cJSON_GetObjectItem(payload, "direction") : NULL;
        if (!cJSON_IsString(direction)) {
            ESP_LOGW(TAG, "CMD move: rejected (missing direction)");
            send_error(server, client_fd, command_id, "INVALID_MESSAGE", "Missing direction");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGI(TAG, "CMD move: %s pressed (id=%s, fd=%d)",
                 direction->valuestring, command_id, client_fd);
        err = motor_move(direction->valuestring);
        success = (err == ESP_OK);
        if (!success) {
            ESP_LOGW(TAG, "CMD move: %s rejected (%s)", direction->valuestring, esp_err_to_name(err));
            send_error(server, client_fd, command_id, "MOTOR_ERROR", "Move rejected");
        }
    } else if (strcmp(action_str, "led") == 0) {
        cJSON *enabled = payload != NULL ? cJSON_GetObjectItem(payload, "enabled") : NULL;
        bool on = cJSON_IsTrue(enabled);
        ESP_LOGI(TAG, "CMD led: %s (id=%s, fd=%d)", on ? "on" : "off", command_id, client_fd);
        motor_led_set(on);
        err = ESP_OK;
        success = true;
    } else if (strcmp(action_str, "estop") == 0) {
        ESP_LOGI(TAG, "CMD estop: pressed (id=%s, fd=%d)", command_id, client_fd);
        motor_emergency_stop();
        err = ESP_OK;
        success = true;
    } else if (strcmp(action_str, "estop_reset") == 0) {
        ESP_LOGI(TAG, "CMD estop_reset: pressed (id=%s, fd=%d)", command_id, client_fd);
        motor_clear_emergency_stop();
        err = ESP_OK;
        success = true;
    } else if (strcmp(action_str, "stream") == 0) {
        cJSON *enabled = payload != NULL ? cJSON_GetObjectItem(payload, "enabled") : NULL;
        /* enabled:false → temporary pause; true/missing → resume */
        bool on = enabled == NULL || cJSON_IsTrue(enabled);
        ESP_LOGI(TAG, "CMD stream: %s (id=%s, fd=%d)", on ? "resume" : "pause", command_id, client_fd);
        http_stream_set_enabled(on);
        err = ESP_OK;
        success = true;
    } else if (strcmp(action_str, "set_mode") == 0) {
        cJSON *mode = payload != NULL ? cJSON_GetObjectItem(payload, "mode") : NULL;
        const char *mode_str = cJSON_IsString(mode) ? mode->valuestring : "?";
        ESP_LOGI(TAG, "CMD set_mode: %s (id=%s, fd=%d)", mode_str, command_id, client_fd);
        if (cJSON_IsString(mode) && strcmp(mode_str, "AUTO_DRIVE") == 0) {
            err = uart_gateway_send_adjust();
            success = (err == ESP_OK);
        }
    } else {
        ESP_LOGW(TAG, "CMD unknown: action=%s (id=%s, fd=%d)", action_str, command_id, client_fd);
        send_error(server, client_fd, command_id, "UNKNOWN_ACTION", action_str);
        cJSON_Delete(root);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (success) {
        send_ack(server, client_fd, command_id, true);
    }

    cJSON_Delete(root);
    return err;
}

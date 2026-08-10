#include "net_log.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "net_rx";

static const char *net_method_name(httpd_method_t method)
{
    switch (method) {
    case HTTP_GET:     return "GET";
    case HTTP_POST:    return "POST";
    case HTTP_PUT:     return "PUT";
    case HTTP_PATCH:   return "PATCH";
    case HTTP_DELETE:  return "DELETE";
    case HTTP_HEAD:    return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    default:           return "OTHER";
    }
}

static const char *ws_type_str(httpd_ws_type_t type)
{
    switch (type) {
    case HTTPD_WS_TYPE_CONTINUE: return "CONTINUE";
    case HTTPD_WS_TYPE_TEXT:   return "TEXT";
    case HTTPD_WS_TYPE_BINARY: return "BINARY";
    case HTTPD_WS_TYPE_CLOSE:  return "CLOSE";
    case HTTPD_WS_TYPE_PING:   return "PING";
    case HTTPD_WS_TYPE_PONG:   return "PONG";
    default:                   return "UNKNOWN";
    }
}

void net_log_http_rx(httpd_req_t *req)
{
    if (!req) {
        return;
    }

    char query[128] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1 && query_len <= sizeof(query)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            query[0] = '\0';
        }
    }

    char content_len[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Length", content_len, sizeof(content_len)) != ESP_OK) {
        strcpy(content_len, "-");
    }

    char user_agent[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent)) != ESP_OK) {
        strcpy(user_agent, "-");
    }

    /* Debug-only: every HTTP request (incl. repeated /stream polling) is too
     * noisy for the default INFO log. Raise this module's log level to see it. */
    if (query[0] != '\0') {
        ESP_LOGD(TAG, "HTTP rx fd=%d %s %s?%s Content-Length=%s User-Agent=%s",
                 httpd_req_to_sockfd(req),
                 net_method_name(req->method),
                 req->uri,
                 query,
                 content_len,
                 user_agent);
    } else {
        ESP_LOGD(TAG, "HTTP rx fd=%d %s %s Content-Length=%s User-Agent=%s",
                 httpd_req_to_sockfd(req),
                 net_method_name(req->method),
                 req->uri,
                 content_len,
                 user_agent);
    }
}

void net_log_ws_rx(int client_fd, httpd_ws_type_t type, const uint8_t *payload, size_t len)
{
    /* Debug-only: raw WS frame dump. cmd_handler logs the decoded command
     * (e.g. "CMD move: direction=forward") at INFO instead. */
    if (len == 0 || payload == NULL) {
        ESP_LOGD(TAG, "WS rx fd=%d type=%s len=0", client_fd, ws_type_str(type));
        return;
    }

    char preview[192];
    size_t copy_len = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
    memcpy(preview, payload, copy_len);
    preview[copy_len] = '\0';

    for (size_t i = 0; i < copy_len; i++) {
        if (preview[i] < 0x20 || preview[i] == 0x7f) {
            preview[i] = '.';
        }
    }

    ESP_LOGD(TAG, "WS rx fd=%d type=%s len=%u data=\"%s\"%s",
             client_fd,
             ws_type_str(type),
             (unsigned)len,
             preview,
             len > copy_len ? "..." : "");
}

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_http_server.h>
#include <stdint.h>

httpd_handle_t http_server_init();

esp_err_t httpd_register_uri_handler_with_basic_auth(void *settings, httpd_handle_t handle, httpd_uri_t *uri_handler);

// Registers a URI handler wrapped with request/outcome counting. A request
// counts as failed if the wrapped handler returns anything other than
// ESP_OK (the convention already used by every handler in this codebase:
// send an error response, then return the corresponding esp_err_t/ESP_FAIL).
esp_err_t httpd_register_uri_handler_instrumented(httpd_handle_t server, httpd_uri_t *uri_handler);

typedef struct {
    const char *uri;
    const char *method;
    uint32_t total_count;
    uint32_t failed_count;
} http_route_metric_snapshot_t;

// Number of distinct (method, uri) routes tracked so far.
int http_metrics_route_count(void);

// Fills *out with the counters for route `index` (0..http_metrics_route_count()-1).
void http_metrics_route_get(int index, http_route_metric_snapshot_t *out);

#endif // HTTP_SERVER_H

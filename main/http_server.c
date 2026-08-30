#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include "esp_check.h"
#include "esp_tls_crypto.h"
#include "esp_tls.h"
#include "settings.h"
#include "metrics.h"
#include "http_server.h"


// Shamelessly borrowed from https://github.com/espressif/esp-idf/blob/v5.5.1/examples/protocols/http_server/simple/main/main.c

static const char *TAG = "httpd";

typedef struct {
    settings_t *settings;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} basic_auth_wrap_t;

#define HTTPD_401      "401 UNAUTHORIZED"           /*!< HTTP Response 401 */

// Distinct from ESP_FAIL (and any other esp_err_t a wrapped handler might
// return) so http_metrics_handler_wrapper can count 401s separately from
// other handler failures. Never sent to httpd's core dispatch logic as
// anything but "non-ESP_OK" - the specific value only matters to us.
#define HTTPD_ERR_UNAUTHORIZED ((esp_err_t)0x8001)

static char *http_auth_basic(const char *username, const char *password)
{
    size_t out;
    char *user_info = NULL;
    char *digest = NULL;
    size_t n = 0;
    int rc = asprintf(&user_info, "%s:%s", username, password);
    if (rc < 0) {
        ESP_LOGE(TAG, "asprintf() returned: %d", rc);
        return NULL;
    }

    if (!user_info) {
        ESP_LOGE(TAG, "No enough memory for user information");
        return NULL;
    }
    atomic_fetch_add(&malloc_count_http_server, 1); //asprintf for user_info
    esp_crypto_base64_encode(NULL, 0, &n, (const unsigned char *)user_info, strlen(user_info));

    /* 6: The length of the "Basic " string
     * n: Number of bytes for a base64 encode format
     * 1: Number of bytes for a reserved which be used to fill zero
    */
    digest = calloc(1, 6 + n + 1);
    atomic_fetch_add(&malloc_count_http_server, 1);
    if (digest) {
        strcpy(digest, "Basic ");
        esp_crypto_base64_encode((unsigned char *)digest + 6, n, &out, (const unsigned char *)user_info, strlen(user_info));
    }
    free(user_info);
    atomic_fetch_add(&free_count_http_server, 1);
    return digest;
}

/* An HTTP GET handler */
static esp_err_t basic_auth_get_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = 0;
    basic_auth_wrap_t *wrapper = req->user_ctx;
    ESP_LOGI(TAG, "basic_auth_get_handler settings ptr %p", wrapper->settings);

    buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len > 1) {
        buf = calloc(1, buf_len);
        atomic_fetch_add(&malloc_count_http_server, 1);
        if (!buf) {
            ESP_LOGE(TAG, "No enough memory for basic authorization");
            return ESP_ERR_NO_MEM;
        }

        if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Authorization: %s", buf);
        } else {
            ESP_LOGE(TAG, "No auth value received");
        }

        char *auth_credentials = http_auth_basic("admin", wrapper->settings->password);
        ESP_LOGI(TAG, "Expected Authorization: %s", auth_credentials);
        ESP_LOGI(TAG, "Received Authorization: %s", buf);
        ESP_LOGI(TAG, "password: %s", wrapper->settings->password);
        if (!auth_credentials) {
            ESP_LOGE(TAG, "No enough memory for basic authorization credentials");
            free(buf);
            atomic_fetch_add(&free_count_http_server, 1);
            return ESP_ERR_NO_MEM;
        }

        if (strncmp(auth_credentials, buf, buf_len)) {
            ESP_LOGE(TAG, "Not authenticated");
            httpd_resp_set_status(req, HTTPD_401);
            httpd_resp_set_hdr(req, "Connection", "keep-alive");
            httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Weight\"");
            httpd_resp_send(req, NULL, 0);
            free(auth_credentials);
            atomic_fetch_add(&free_count_http_server, 1);
            free(buf);
            atomic_fetch_add(&free_count_http_server, 1);
            return HTTPD_ERR_UNAUTHORIZED;
        } else {
            ESP_LOGI(TAG, "Authenticated!");
            req->user_ctx = wrapper->user_ctx;
            free(auth_credentials);
            atomic_fetch_add(&free_count_http_server, 1);
            free(buf);
            atomic_fetch_add(&free_count_http_server, 1);
            return wrapper->handler(req);
        }
    } else {
        ESP_LOGE(TAG, "No auth header received");
        httpd_resp_set_status(req, HTTPD_401);
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Weight\"");
        httpd_resp_send(req, NULL, 0);
        return HTTPD_ERR_UNAUTHORIZED;
    }

    return ESP_OK;
}

esp_err_t httpd_register_uri_handler_with_basic_auth(void *settings_ptr, httpd_handle_t server, httpd_uri_t *uri_handler)
{
    settings_t *settings = (settings_t *)settings_ptr;
    ESP_LOGI(TAG, "httpd_register_uri_handler_with_basic_auth settings ptr %p", settings);
    basic_auth_wrap_t *wrapper = malloc(sizeof(basic_auth_wrap_t));
    atomic_fetch_add(&malloc_count_http_server, 1);
    if (!wrapper) {
        ESP_LOGE(TAG, "No enough memory for basic auth wrapper");
        return ESP_ERR_NO_MEM;
    }
    memset(wrapper, 0, sizeof(basic_auth_wrap_t));
    wrapper->handler = uri_handler->handler;
    wrapper->user_ctx = uri_handler->user_ctx;
    wrapper->settings = settings;

    httpd_uri_t *wrapped_uri_handler = malloc(sizeof(httpd_uri_t));
    atomic_fetch_add(&malloc_count_http_server, 1);
    if (!wrapped_uri_handler) {
        ESP_LOGE(TAG, "No enough memory for wrapped URI handler");
        free(wrapper);
        atomic_fetch_add(&free_count_http_server, 1);
        return ESP_ERR_NO_MEM;
    }
    memcpy(wrapped_uri_handler, uri_handler, sizeof(httpd_uri_t));
    wrapped_uri_handler->user_ctx = wrapper;
    wrapped_uri_handler->handler = basic_auth_get_handler;

    // Instrument the auth-wrapped handler rather than registering it
    // directly, so a 401 (returned as ESP_FAIL by basic_auth_get_handler)
    // is counted as a failed request just like any other handler error.
    return httpd_register_uri_handler_instrumented(server, wrapped_uri_handler);
}

// --- Per-route request/outcome metrics -------------------------------------
//
// Route slots are only ever created while handlers are being registered at
// boot, before the httpd is serving requests concurrently from other tasks,
// so http_route_metric_count and the slot array need no locking. The counts
// within a slot are atomic since requests are served from httpd's worker
// tasks after boot.

#define HTTP_METRICS_MAX_ROUTES 24

typedef struct {
    char uri[40];
    char method[8];
    atomic_uint_fast32_t total_count;
    atomic_uint_fast32_t failed_count;
    atomic_uint_fast32_t unauthorized_count;
} http_route_metric_t;

typedef struct {
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
    http_route_metric_t *metric;
} http_metrics_wrap_t;

static http_route_metric_t http_route_metrics[HTTP_METRICS_MAX_ROUTES];
static int http_route_metric_count = 0;

static http_route_metric_t *http_route_metric_get_or_create(const char *uri, const char *method) {
    for (int i = 0; i < http_route_metric_count; i++) {
        if (strcmp(http_route_metrics[i].uri, uri) == 0 &&
            strcmp(http_route_metrics[i].method, method) == 0) {
            return &http_route_metrics[i];
        }
    }
    if (http_route_metric_count >= HTTP_METRICS_MAX_ROUTES) {
        ESP_LOGW(TAG, "HTTP metrics route table full, not tracking %s %s", method, uri);
        return NULL;
    }
    http_route_metric_t *slot = &http_route_metrics[http_route_metric_count++];
    strncpy(slot->uri, uri, sizeof(slot->uri) - 1);
    slot->uri[sizeof(slot->uri) - 1] = '\0';
    strncpy(slot->method, method, sizeof(slot->method) - 1);
    slot->method[sizeof(slot->method) - 1] = '\0';
    atomic_init(&slot->total_count, 0);
    atomic_init(&slot->failed_count, 0);
    atomic_init(&slot->unauthorized_count, 0);
    return slot;
}

static esp_err_t http_metrics_handler_wrapper(httpd_req_t *req) {
    http_metrics_wrap_t *wrap = (http_metrics_wrap_t *)req->user_ctx;

    atomic_fetch_add(&wrap->metric->total_count, 1);

    req->user_ctx = wrap->user_ctx;
    esp_err_t result = wrap->handler(req);

    if (result == HTTPD_ERR_UNAUTHORIZED) {
        atomic_fetch_add(&wrap->metric->unauthorized_count, 1);
    } else if (result != ESP_OK) {
        atomic_fetch_add(&wrap->metric->failed_count, 1);
    }
    return result;
}

esp_err_t httpd_register_uri_handler_instrumented(httpd_handle_t server, httpd_uri_t *uri_handler) {
    const char *method_str = http_method_str(uri_handler->method);
    http_route_metric_t *metric = http_route_metric_get_or_create(uri_handler->uri, method_str);
    if (metric == NULL) {
        // Route table is full; register the handler unmodified rather than
        // dropping the route entirely.
        return httpd_register_uri_handler(server, uri_handler);
    }

    http_metrics_wrap_t *wrap = malloc(sizeof(http_metrics_wrap_t));
    atomic_fetch_add(&malloc_count_http_server, 1);
    if (!wrap) {
        ESP_LOGE(TAG, "No enough memory for http metrics wrapper");
        return httpd_register_uri_handler(server, uri_handler);
    }
    wrap->handler = uri_handler->handler;
    wrap->user_ctx = uri_handler->user_ctx;
    wrap->metric = metric;

    httpd_uri_t *wrapped_uri_handler = malloc(sizeof(httpd_uri_t));
    atomic_fetch_add(&malloc_count_http_server, 1);
    if (!wrapped_uri_handler) {
        ESP_LOGE(TAG, "No enough memory for wrapped URI handler");
        free(wrap);
        atomic_fetch_add(&free_count_http_server, 1);
        return httpd_register_uri_handler(server, uri_handler);
    }
    memcpy(wrapped_uri_handler, uri_handler, sizeof(httpd_uri_t));
    wrapped_uri_handler->user_ctx = wrap;
    wrapped_uri_handler->handler = http_metrics_handler_wrapper;

    return httpd_register_uri_handler(server, wrapped_uri_handler);
}

int http_metrics_route_count(void) {
    return http_route_metric_count;
}

void http_metrics_route_get(int index, http_route_metric_snapshot_t *out) {
    if (out == NULL || index < 0 || index >= http_route_metric_count) {
        return;
    }
    out->uri = http_route_metrics[index].uri;
    out->method = http_route_metrics[index].method;
    out->total_count = atomic_load(&http_route_metrics[index].total_count);
    out->failed_count = atomic_load(&http_route_metrics[index].failed_count);
    out->unauthorized_count = atomic_load(&http_route_metrics[index].unauthorized_count);
}

httpd_handle_t http_server_init(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    // Keep in step with the number of routes registered across all modules
    // (settings/sensors/pump/ezo_ph/ota + "/"/metrics/bthome). Registration
    // fails silently past this limit, dropping whichever handlers register last.
    config.max_uri_handlers = 26;
    config.max_open_sockets = 10;
    
    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}


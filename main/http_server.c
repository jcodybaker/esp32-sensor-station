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


// Shamelessly borrowed from https://github.com/espressif/esp-idf/blob/v5.5.1/examples/protocols/http_server/simple/main/main.c

static const char *TAG = "httpd";

typedef struct {
    settings_t *settings;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} basic_auth_wrap_t;

#define HTTPD_401      "401 UNAUTHORIZED"           /*!< HTTP Response 401 */

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
        } else {
            ESP_LOGI(TAG, "Authenticated!");
            req->user_ctx = wrapper->user_ctx;
            free(auth_credentials);
            atomic_fetch_add(&free_count_http_server, 1);
            free(buf);
            atomic_fetch_add(&free_count_http_server, 1);
            return wrapper->handler(req);
        }
        free(auth_credentials);
        atomic_fetch_add(&free_count_http_server, 1);
        free(buf);
        atomic_fetch_add(&free_count_http_server, 1);
    } else {
        ESP_LOGE(TAG, "No auth header received");
        httpd_resp_set_status(req, HTTPD_401);
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Weight\"");
        httpd_resp_send(req, NULL, 0);
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

    return httpd_register_uri_handler(server, wrapped_uri_handler);
}

httpd_handle_t http_server_init(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16;
    
    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}


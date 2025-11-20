#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_http_server.h>

httpd_handle_t http_server_init();

esp_err_t httpd_register_uri_handler_with_basic_auth(settings_t *settings, httpd_handle_t handle, httpd_uri_t *uri_handler);

#endif // HTTP_SERVER_H
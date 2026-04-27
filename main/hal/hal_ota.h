#ifndef HAL_OTA_H
#define HAL_OTA_H

#include "esp_http_server.h"

/** Register GET/POST /ota on the SoftAP HTTP server (POST = raw firmware image). */
esp_err_t hal_ota_register_handlers(httpd_handle_t server);

#endif

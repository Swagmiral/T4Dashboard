/**
 * OTA over SoftAP HTTP — POST /ota with raw .bin body and Content-Length.
 */

#include "hal_ota.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota";

#define OTA_RECV_CHUNK 4096

static esp_err_t handle_ota_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const char *msg =
        "OTA firmware update\n"
        "-------------------\n"
        "POST this URL with the raw application .bin as the body.\n"
        "Header Content-Length is required (must match body size).\n"
        "\n"
        "Example (from PC on same WiFi as the dashboard AP):\n"
        "  curl -T build/t4dashboard.bin http://192.168.4.1/ota\n"
        "\n"
        "The device responds then reboots into the new image.\n";
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_ota_post(httpd_req_t *req)
{
    char len_buf[24];
    if (httpd_req_get_hdr_value_str(req, "Content-Length", len_buf, sizeof(len_buf)) != ESP_OK) {
        httpd_resp_set_status(req, "411 Length Required");
        return httpd_resp_send(req, "Content-Length header required", HTTPD_RESP_USE_STRLEN);
    }

    long content_len = strtol(len_buf, NULL, 10);
    if (content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Invalid Content-Length", HTTPD_RESP_USE_STRLEN);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "No OTA app partition", HTTPD_RESP_USE_STRLEN);
    }

    if ((size_t)content_len > update->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Image larger than OTA partition", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "OTA begin: %ld bytes -> partition '%s' (running '%s')",
             content_len, update->label, running ? running->label : "?");

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update, (size_t)content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "esp_ota_begin failed", HTTPD_RESP_USE_STRLEN);
    }

    char buf[OTA_RECV_CHUNK];
    long received = 0;
    unsigned chunk_ix = 0;

    while (received < content_len) {
        int want = (int)((content_len - received) > OTA_RECV_CHUNK ? OTA_RECV_CHUNK : (content_len - received));
        int r = httpd_req_recv(req, buf, want);
        if (r < 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "recv error %d", r);
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "Socket read error", HTTPD_RESP_USE_STRLEN);
        }
        if (r == 0) {
            break;
        }
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "esp_ota_write failed", HTTPD_RESP_USE_STRLEN);
        }
        received += r;
        if ((++chunk_ix & 0x3Fu) == 0) {
            vTaskDelay(1);
        }
    }

    if (received != content_len) {
        ESP_LOGE(TAG, "short read: %ld / %ld", received, content_len);
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Body shorter than Content-Length", HTTPD_RESP_USE_STRLEN);
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Invalid firmware image", HTTPD_RESP_USE_STRLEN);
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Could not set boot partition", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, "OK, rebooting into new firmware.\n", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t hal_ota_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uri_ota_get = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = handle_ota_get,
    };
    static const httpd_uri_t uri_ota_post = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = handle_ota_post,
    };

    esp_err_t e = httpd_register_uri_handler(server, &uri_ota_get);
    if (e != ESP_OK) {
        return e;
    }
    return httpd_register_uri_handler(server, &uri_ota_post);
}

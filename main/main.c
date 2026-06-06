#include <stdio.h>
#include <string.h>

#include "ca_manifest_updater.h"
#include "ca_manager.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_WIFI_RETRY 8
#define CA_VERSION_BUFFER_SIZE 32
#define CA_HTTP_TX_BUFFER_SIZE 4096
#define CA_MAX_HTTP_REDIRECTS 5
#define CA_BOOT_HTTPS_TEST_URLS_BUFFER_SIZE 768

static const char *TAG = "ca_updater";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;

static bool is_http_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < MAX_WIFI_RETRY) {
            s_wifi_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/%d)", s_wifi_retry_count, MAX_WIFI_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    if (strlen(CONFIG_CA_UPDATER_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; skipping bundle download");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
                        TAG, "Wi-Fi handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
                        TAG, "IP handler register failed");

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_CA_UPDATER_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_CA_UPDATER_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connection failed");
    return ESP_FAIL;
}

static void log_active_bundle_state(const char *prefix)
{
    char active_version[CA_VERSION_BUFFER_SIZE];
    esp_err_t err = ca_manager_get_active_version(active_version, sizeof(active_version));
    if (err != ESP_OK) {
        strlcpy(active_version, "(unknown)", sizeof(active_version));
    }

    ESP_LOGI(TAG, "%s CA bundle version=%s size=%u",
             prefix,
             active_version,
             (unsigned)ca_manager_get_active_bundle_size());
}

static esp_err_t update_bundle_if_needed(void)
{
    if (strlen(CONFIG_CA_UPDATER_MANIFEST_URL) == 0) {
        ESP_LOGW(TAG, "Manifest URL is empty; skipping bundle update");
        return ESP_ERR_INVALID_STATE;
    }

    const ca_manifest_updater_config_t config = {
        .manifest_url = CONFIG_CA_UPDATER_MANIFEST_URL,
        .channel = NULL,
        .restart_on_update = true,
    };
    return ca_manifest_updater_run(&config, NULL);
}

static esp_err_t probe_https_url(const char *url, int *out_status, int64_t *out_content_length)
{
    if (url == NULL || url[0] == '\0' || out_status == NULL || out_content_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "https://", strlen("https://")) != 0) {
        ESP_LOGE(TAG, "URL must use https");
        return ESP_ERR_INVALID_ARG;
    }

    *out_status = 0;
    *out_content_length = 0;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CONFIG_CA_UPDATER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CA_MAX_HTTP_REDIRECTS,
        .buffer_size_tx = CA_HTTP_TX_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    int status = 0;
    int64_t content_length = 0;
    for (int redirect_count = 0; redirect_count <= CA_MAX_HTTP_REDIRECTS; redirect_count++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed: %lld", content_length);
            err = ESP_FAIL;
            goto cleanup;
        }

        status = esp_http_client_get_status_code(client);
        if (!is_http_redirect_status(status)) {
            break;
        }

        ESP_LOGI(TAG, "Following HTTP redirect: status=%d", status);
        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP redirect failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    *out_status = status;
    *out_content_length = content_length;
    err = status >= 200 && status < 300 ? ESP_OK : ESP_FAIL;

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void trim_in_place(char *text)
{
    if (text == NULL) {
        return;
    }

    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        text[--len] = '\0';
    }
}

static void run_boot_https_diagnostics(void)
{
    char urls[CA_BOOT_HTTPS_TEST_URLS_BUFFER_SIZE];
    if (strlcpy(urls, CONFIG_CA_UPDATER_BOOT_HTTPS_TEST_URLS, sizeof(urls)) >= sizeof(urls)) {
        ESP_LOGE(TAG, "Boot HTTPS diagnostic URL list is too long");
        return;
    }

    trim_in_place(urls);
    if (urls[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "Running boot HTTPS diagnostics");

    char *saveptr = NULL;
    for (char *url = strtok_r(urls, ",", &saveptr);
         url != NULL;
         url = strtok_r(NULL, ",", &saveptr)) {
        trim_in_place(url);
        if (url[0] == '\0') {
            continue;
        }

        int status = 0;
        int64_t content_length = 0;
        esp_err_t err = probe_https_url(url, &status, &content_length);
        ESP_LOGI(TAG, "HTTPS diagnostic url=%s status=%d content_length=%lld result=%s",
                 url, status, content_length, esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ca_manager_init());
    log_active_bundle_state("Boot");

    if (connect_wifi() == ESP_OK) {
        run_boot_https_diagnostics();
        err = update_bundle_if_needed();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bundle update failed: %s", esp_err_to_name(err));
        }
    }

    log_active_bundle_state("Ready.");
}

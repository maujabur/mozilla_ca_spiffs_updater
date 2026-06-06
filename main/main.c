#include <stdio.h>
#include <string.h>

#include "ca_manifest_updater.h"
#include "ca_manager.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "manifest_file_updater.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_WIFI_RETRY 8
#define CA_VERSION_BUFFER_SIZE 32
#define CA_BOOT_HTTPS_TEST_URLS_BUFFER_SIZE 768

static const char *TAG = "ca_updater";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;

// ----- Example Wi-Fi connection -----

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

// ----- End example Wi-Fi connection -----

// ----- CA bundle update flow -----

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

static void log_component_versions(void)
{
    ESP_LOGI(TAG, "Component versions: ca_manager=%s manifest_file_updater=%s ca_manifest_updater=%s",
             ca_manager_get_component_version(),
             manifest_file_updater_get_component_version(),
             ca_manifest_updater_get_component_version());
}

// ----- End CA bundle update flow -----

// ----- Example HTTPS diagnostics -----

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
    // Example-app only: boot diagnostics demonstrate the active CA bundle
    // without adding debug policy to the reusable updater components.
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
        esp_err_t err = manifest_file_updater_probe_https(url, &status, &content_length);
        ESP_LOGI(TAG, "HTTPS diagnostic url=%s status=%d content_length=%lld result=%s",
                 url, status, content_length, esp_err_to_name(err));
    }
}

// ----- End example HTTPS diagnostics -----

// ----- Application entry point -----

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ca_manager_init());
    log_component_versions();
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

// ----- End application entry point -----

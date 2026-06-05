#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_WIFI_RETRY 8
#define CA_VERSION_BUFFER_SIZE 32
#define CA_SHA256_HEX_SIZE 64
#define CA_SHA256_BUFFER_SIZE 80
#define CA_URL_BUFFER_SIZE 256
#define CA_MANIFEST_BUFFER_SIZE 1024

typedef struct {
    char version[CA_VERSION_BUFFER_SIZE];
    char url[CA_URL_BUFFER_SIZE];
    char sha256[CA_SHA256_BUFFER_SIZE];
    size_t size;
} ca_bundle_manifest_t;

static const char *TAG = "ca_updater";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;

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

static void trim_text(char *text)
{
    if (text == NULL) {
        return;
    }

    text[strcspn(text, "\r\n\t ")] = '\0';
}

static const char *skip_json_ws(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }
    return cursor;
}

static const char *find_json_value(const char *json, const char *name)
{
    char key[48];
    int written = snprintf(key, sizeof(key), "\"%s\"", name);
    if (written < 0 || (size_t)written >= sizeof(key)) {
        return NULL;
    }

    const char *cursor = strstr(json, key);
    if (cursor == NULL) {
        return NULL;
    }

    cursor += strlen(key);
    cursor = skip_json_ws(cursor);
    if (*cursor != ':') {
        return NULL;
    }

    return skip_json_ws(cursor + 1);
}

static esp_err_t copy_json_string(const char *json, const char *name, char *out, size_t out_size)
{
    const char *value = find_json_value(json, name);
    if (value == NULL || *value != '"') {
        ESP_LOGE(TAG, "Manifest field is missing or invalid: %s", name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    value++;
    size_t len = 0;
    while (value[len] != '\0' && value[len] != '"') {
        if (value[len] == '\\') {
            ESP_LOGE(TAG, "Escaped strings are not supported in manifest field: %s", name);
            return ESP_ERR_INVALID_RESPONSE;
        }
        len++;
    }

    if (value[len] != '"') {
        ESP_LOGE(TAG, "Unterminated manifest field: %s", name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (len == 0 || len >= out_size) {
        ESP_LOGE(TAG, "Manifest field is too long: %s", name);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, value, len);
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t read_json_int(const char *json, const char *name, int *out_value)
{
    const char *value = find_json_value(json, name);
    if (value == NULL || *value < '0' || *value > '9') {
        ESP_LOGE(TAG, "Manifest number is missing or invalid: %s", name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > INT32_MAX) {
        ESP_LOGE(TAG, "Manifest number is out of range: %s", name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_value = (int)parsed;
    return ESP_OK;
}

static esp_err_t parse_manifest(const char *manifest_text, ca_bundle_manifest_t *manifest)
{
    if (manifest_text == NULL || manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manifest, 0, sizeof(*manifest));

    int schema = 0;
    ESP_RETURN_ON_ERROR(read_json_int(manifest_text, "schema", &schema),
                        TAG, "Invalid manifest schema");
    if (schema != 1) {
        ESP_LOGE(TAG, "Unsupported manifest schema: %d", schema);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char artifact_type[24];
    esp_err_t err = copy_json_string(manifest_text, "artifact_type", artifact_type, sizeof(artifact_type));
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(artifact_type, "ca_bundle") != 0) {
        ESP_LOGE(TAG, "Unexpected manifest artifact_type: %s", artifact_type);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(copy_json_string(manifest_text, "version", manifest->version, sizeof(manifest->version)),
                        TAG, "Invalid manifest version");
    ESP_RETURN_ON_ERROR(copy_json_string(manifest_text, "url", manifest->url, sizeof(manifest->url)),
                        TAG, "Invalid manifest URL");
    ESP_RETURN_ON_ERROR(copy_json_string(manifest_text, "sha256", manifest->sha256, sizeof(manifest->sha256)),
                        TAG, "Invalid manifest SHA-256");

    if (strlen(manifest->sha256) != CA_SHA256_HEX_SIZE) {
        ESP_LOGE(TAG, "Manifest SHA-256 is invalid: %s", manifest->sha256);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int size = 0;
    ESP_RETURN_ON_ERROR(read_json_int(manifest_text, "size", &size),
                        TAG, "Invalid manifest size");
    manifest->size = (size_t)size;
    return ESP_OK;
}

static esp_err_t load_manifest(ca_bundle_manifest_t *manifest)
{
    if (strlen(CONFIG_CA_UPDATER_VERSION_URL) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(manifest, 0, sizeof(*manifest));
    ESP_RETURN_ON_ERROR(ca_manager_download_text(CONFIG_CA_UPDATER_VERSION_URL,
                                                 manifest->version,
                                                 sizeof(manifest->version)),
                        TAG, "Failed to download CA bundle version");
    trim_text(manifest->version);
    if (manifest->version[0] == '\0') {
        ESP_LOGE(TAG, "Downloaded CA bundle version is empty");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strlen(CONFIG_CA_UPDATER_SHA256_URL) == 0) {
        ESP_LOGE(TAG, "SHA-256 URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ca_manager_download_text(CONFIG_CA_UPDATER_SHA256_URL,
                                                 manifest->sha256,
                                                 sizeof(manifest->sha256)),
                        TAG, "Failed to download CA bundle SHA-256");
    trim_text(manifest->sha256);
    if (strlen(manifest->sha256) != CA_SHA256_HEX_SIZE) {
        ESP_LOGE(TAG, "Downloaded CA bundle SHA-256 is invalid: %s", manifest->sha256);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strlcpy(manifest->url, CONFIG_CA_UPDATER_BUNDLE_URL, sizeof(manifest->url)) >= sizeof(manifest->url)) {
        ESP_LOGE(TAG, "Configured bundle URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t load_manifest_from_json(ca_bundle_manifest_t *manifest)
{
    char manifest_text[CA_MANIFEST_BUFFER_SIZE];
    ESP_RETURN_ON_ERROR(ca_manager_download_text(CONFIG_CA_UPDATER_MANIFEST_URL,
                                                 manifest_text,
                                                 sizeof(manifest_text)),
                        TAG, "Failed to download CA bundle manifest");
    return parse_manifest(manifest_text, manifest);
}

static esp_err_t apply_manifest_if_needed(const ca_bundle_manifest_t *manifest)
{
    char active_version[CA_VERSION_BUFFER_SIZE];
    esp_err_t err = ca_manager_get_active_version(active_version, sizeof(active_version));
    if (err == ESP_OK && strcmp(active_version, manifest->version) == 0) {
        ESP_LOGI(TAG, "CA bundle version %s is already active", active_version);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        active_version[0] = '\0';
    }

    ESP_LOGI(TAG, "Updating CA bundle version %s -> %s",
             active_version[0] != '\0' ? active_version : "(none)",
             manifest->version);

    bool active_bundle_matches = false;
    ESP_RETURN_ON_ERROR(ca_manager_active_bundle_matches_sha256(manifest->sha256,
                                                                &active_bundle_matches),
                        TAG, "Failed to compare active bundle SHA-256");
    if (active_bundle_matches) {
        ESP_LOGI(TAG, "Active CA bundle already matches remote SHA-256");
        ESP_RETURN_ON_ERROR(ca_manager_set_active_version(manifest->version),
                            TAG, "Failed to store CA bundle version");
        ESP_LOGI(TAG, "CA bundle version corrected to %s; restart not needed", manifest->version);
        return ESP_OK;
    }

    bool promoted = false;
    ESP_RETURN_ON_ERROR(ca_manager_update_from_http_client_verified(manifest->url,
                                                                    manifest->sha256,
                                                                    false,
                                                                    &promoted),
                        TAG, "Bundle download/apply failed");
    ESP_RETURN_ON_ERROR(ca_manager_set_active_version(manifest->version),
                        TAG, "Failed to store CA bundle version");

    if (!promoted) {
        ESP_LOGI(TAG, "CA bundle version corrected to %s; restart not needed", manifest->version);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "CA bundle updated to version %s; restarting", manifest->version);
    fflush(stdout);
    esp_restart();
    return ESP_OK;
}

static esp_err_t update_bundle_if_needed(void)
{
    ca_bundle_manifest_t manifest;

    if (strlen(CONFIG_CA_UPDATER_MANIFEST_URL) != 0) {
        ESP_RETURN_ON_ERROR(load_manifest_from_json(&manifest), TAG, "Manifest update metadata failed");
        return apply_manifest_if_needed(&manifest);
    }

    ESP_LOGW(TAG, "Manifest URL is empty; using legacy version/SHA-256 URLs");
    if (load_manifest(&manifest) == ESP_OK) {
        return apply_manifest_if_needed(&manifest);
    }

    ESP_LOGW(TAG, "Legacy version URL is empty; using direct bundle update");
    return ca_manager_update_from_http_client(CONFIG_CA_UPDATER_BUNDLE_URL, true);
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
    char active_version[CA_VERSION_BUFFER_SIZE];
    esp_err_t version_err = ca_manager_get_active_version(active_version, sizeof(active_version));
    if (version_err != ESP_OK) {
        strlcpy(active_version, "(unknown)", sizeof(active_version));
    }

    if (connect_wifi() == ESP_OK) {
        err = update_bundle_if_needed();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bundle update failed: %s", esp_err_to_name(err));
        }
    }

    version_err = ca_manager_get_active_version(active_version, sizeof(active_version));
    if (version_err != ESP_OK) {
        strlcpy(active_version, "(unknown)", sizeof(active_version));
    }

    ESP_LOGI(TAG, "Ready. Active CA bundle version=%s size=%u",
             active_version,
             (unsigned)ca_manager_get_active_bundle_size());
}

#include <stdio.h>
#include <string.h>

#include "ca_manager.h"
#include "cJSON.h"
#include "esp_console.h"
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
#define CA_SHA256_HEX_SIZE 64
#define CA_SHA256_BUFFER_SIZE 80
#define CA_URL_BUFFER_SIZE 256
#define CA_MANIFEST_BUFFER_SIZE 1024
#define CA_CONSOLE_STACK_SIZE 8192
#define CA_HTTP_TX_BUFFER_SIZE 4096
#define CA_MAX_HTTP_REDIRECTS 5

typedef struct {
    char version[CA_VERSION_BUFFER_SIZE];
    char url[CA_URL_BUFFER_SIZE];
    char sha256[CA_SHA256_BUFFER_SIZE];
    size_t size;
} ca_bundle_manifest_t;

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

static void print_active_bundle_state(void)
{
    char active_version[CA_VERSION_BUFFER_SIZE];
    esp_err_t err = ca_manager_get_active_version(active_version, sizeof(active_version));
    if (err != ESP_OK) {
        strlcpy(active_version, "(unknown)", sizeof(active_version));
    }

    printf("CA bundle version=%s size=%u\n",
           active_version,
           (unsigned)ca_manager_get_active_bundle_size());
}

static esp_err_t copy_json_string(cJSON *root, const char *name, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        ESP_LOGE(TAG, "Manifest field is missing or invalid: %s", name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strlcpy(out, item->valuestring, out_size) >= out_size) {
        ESP_LOGE(TAG, "Manifest field is too long: %s", name);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t parse_manifest(const char *manifest_text, ca_bundle_manifest_t *manifest)
{
    if (manifest_text == NULL || manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manifest, 0, sizeof(*manifest));

    cJSON *root = cJSON_Parse(manifest_text);
    if (root == NULL) {
        ESP_LOGE(TAG, "Manifest JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = ESP_OK;
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
        ESP_LOGE(TAG, "Unsupported manifest schema");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    char artifact_type[24];
    ret = copy_json_string(root, "artifact_type", artifact_type, sizeof(artifact_type));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (strcmp(artifact_type, "ca_bundle") != 0) {
        ESP_LOGE(TAG, "Unexpected manifest artifact_type: %s", artifact_type);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ESP_GOTO_ON_ERROR(copy_json_string(root, "version", manifest->version, sizeof(manifest->version)),
                      cleanup, TAG, "Invalid manifest version");
    ESP_GOTO_ON_ERROR(copy_json_string(root, "url", manifest->url, sizeof(manifest->url)),
                      cleanup, TAG, "Invalid manifest URL");
    ESP_GOTO_ON_ERROR(copy_json_string(root, "sha256", manifest->sha256, sizeof(manifest->sha256)),
                      cleanup, TAG, "Invalid manifest SHA-256");

    if (strncmp(manifest->url, "https://", strlen("https://")) != 0) {
        ESP_LOGE(TAG, "Manifest URL must use https");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    if (strlen(manifest->sha256) != CA_SHA256_HEX_SIZE) {
        ESP_LOGE(TAG, "Manifest SHA-256 is invalid: %s", manifest->sha256);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0) {
        ESP_LOGE(TAG, "Manifest size is missing or invalid");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    manifest->size = (size_t)size->valuedouble;

cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t load_manifest_from_url(const char *url, ca_bundle_manifest_t *manifest)
{
    char manifest_text[CA_MANIFEST_BUFFER_SIZE];
    ESP_RETURN_ON_ERROR(ca_manager_download_text(url,
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
                                                                    manifest->size,
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
    if (strlen(CONFIG_CA_UPDATER_MANIFEST_URL) == 0) {
        ESP_LOGW(TAG, "Manifest URL is empty; skipping bundle update");
        return ESP_ERR_INVALID_STATE;
    }

    ca_bundle_manifest_t manifest;
    ESP_RETURN_ON_ERROR(load_manifest_from_url(CONFIG_CA_UPDATER_MANIFEST_URL, &manifest),
                        TAG, "Manifest update metadata failed");
    return apply_manifest_if_needed(&manifest);
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

static void print_manifest(const ca_bundle_manifest_t *manifest)
{
    printf("version=%s\n", manifest->version);
    printf("url=%s\n", manifest->url);
    printf("sha256=%s\n", manifest->sha256);
    printf("size=%u\n", (unsigned)manifest->size);
}

static int ca_console_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        printf("Usage:\n");
        printf("  ca status\n");
        printf("  ca https-test <url>\n");
        printf("  ca fetch-manifest <url>\n");
        printf("  ca check [manifest_url]\n");
        printf("  ca update [manifest_url]\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        print_active_bundle_state();
        return 0;
    }

    if (strcmp(argv[1], "https-test") == 0) {
        if (argc < 3) {
            printf("Usage: ca https-test <url>\n");
            return 1;
        }

        int status = 0;
        int64_t content_length = 0;
        esp_err_t err = probe_https_url(argv[2], &status, &content_length);
        printf("https-test url=%s status=%d content_length=%lld result=%s\n",
               argv[2], status, content_length, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "fetch-manifest") == 0) {
        if (argc < 3) {
            printf("Usage: ca fetch-manifest <url>\n");
            return 1;
        }

        ca_bundle_manifest_t manifest;
        esp_err_t err = load_manifest_from_url(argv[2], &manifest);
        if (err != ESP_OK) {
            printf("fetch-manifest failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        print_manifest(&manifest);
        return 0;
    }

    if (strcmp(argv[1], "check") == 0) {
        const char *url = argc >= 3 ? argv[2] : CONFIG_CA_UPDATER_MANIFEST_URL;
        if (url[0] == '\0') {
            printf("Manifest URL is empty\n");
            return 1;
        }

        ca_bundle_manifest_t manifest;
        esp_err_t err = load_manifest_from_url(url, &manifest);
        if (err != ESP_OK) {
            printf("check failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        print_manifest(&manifest);

        char active_version[CA_VERSION_BUFFER_SIZE];
        err = ca_manager_get_active_version(active_version, sizeof(active_version));
        if (err != ESP_OK) {
            active_version[0] = '\0';
        }

        bool active_bundle_matches = false;
        err = ca_manager_active_bundle_matches_sha256(manifest.sha256, &active_bundle_matches);
        if (err != ESP_OK) {
            printf("active SHA-256 check failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        if (active_version[0] != '\0' && strcmp(active_version, manifest.version) == 0) {
            printf("state=already-active\n");
        } else if (active_bundle_matches) {
            printf("state=version-needs-correction active_version=%s remote_version=%s\n",
                   active_version[0] != '\0' ? active_version : "(none)",
                   manifest.version);
        } else {
            printf("state=update-available active_version=%s remote_version=%s\n",
                   active_version[0] != '\0' ? active_version : "(none)",
                   manifest.version);
        }
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        const char *url = argc >= 3 ? argv[2] : CONFIG_CA_UPDATER_MANIFEST_URL;
        if (url[0] == '\0') {
            printf("Manifest URL is empty\n");
            return 1;
        }

        ca_bundle_manifest_t manifest;
        esp_err_t err = load_manifest_from_url(url, &manifest);
        if (err != ESP_OK) {
            printf("update metadata failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        err = apply_manifest_if_needed(&manifest);
        if (err != ESP_OK) {
            printf("update failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("update finished\n");
        return 0;
    }

    printf("Unknown ca command: %s\n", argv[1]);
    printf("Run 'ca help'\n");
    return 1;
}

static esp_err_t start_diagnostic_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ca> ";
    repl_config.task_stack_size = CA_CONSOLE_STACK_SIZE;
    repl_config.max_cmdline_length = 384;

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "Failed to register help command");

    const esp_console_cmd_t ca_cmd = {
        .command = "ca",
        .help = "CA bundle diagnostic commands. Run 'ca help'.",
        .hint = NULL,
        .func = &ca_console_command,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ca_cmd), TAG, "Failed to register CA command");
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&repl_config, &repl), TAG, "Failed to create console REPL");
    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "Failed to start console REPL");
    ESP_LOGI(TAG, "Diagnostic console ready. Run 'ca help'.");
    return ESP_OK;
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
        err = update_bundle_if_needed();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bundle update failed: %s", esp_err_to_name(err));
        }
    }

    log_active_bundle_state("Ready.");

#if CONFIG_CA_UPDATER_ENABLE_CONSOLE
    err = start_diagnostic_console();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostic console failed: %s", esp_err_to_name(err));
    }
#endif
}

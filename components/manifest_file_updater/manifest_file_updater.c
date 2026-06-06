#include "manifest_file_updater.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/private/sha256.h"

#define MANIFEST_FILE_SHA256_BYTES 32
#define MANIFEST_FILE_SHA256_HEX_CHARS 64

static const char *TAG = "manifest_file";

const char *manifest_file_updater_get_component_version(void)
{
    return MANIFEST_FILE_UPDATER_COMPONENT_VERSION;
}

static bool is_http_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t sha256_hex_to_bytes(const char *hex, uint8_t out[MANIFEST_FILE_SHA256_BYTES])
{
    if (hex == NULL || out == NULL || strlen(hex) != MANIFEST_FILE_SHA256_HEX_CHARS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < MANIFEST_FILE_SHA256_BYTES; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    return ESP_OK;
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

static esp_err_t copy_optional_json_string(cJSON *root, const char *name, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    out[0] = '\0';
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        ESP_LOGE(TAG, "Manifest field is invalid: %s", name);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strlcpy(out, item->valuestring, out_size) >= out_size) {
        ESP_LOGE(TAG, "Manifest field is too long: %s", name);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t parse_manifest(const char *manifest_text,
                                const manifest_file_update_request_t *request,
                                manifest_file_artifact_t *artifact)
{
    if (manifest_text == NULL || request == NULL || artifact == NULL ||
        request->artifact_type == NULL || request->artifact_type[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(artifact, 0, sizeof(*artifact));

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
    artifact->schema = schema->valueint;

    ESP_GOTO_ON_ERROR(copy_json_string(root, "artifact_type",
                                       artifact->artifact_type,
                                       sizeof(artifact->artifact_type)),
                      cleanup, TAG, "Invalid manifest artifact_type");
    if (strcmp(artifact->artifact_type, request->artifact_type) != 0) {
        ESP_LOGE(TAG, "Unexpected manifest artifact_type: %s", artifact->artifact_type);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ESP_GOTO_ON_ERROR(copy_json_string(root, "version", artifact->version, sizeof(artifact->version)),
                      cleanup, TAG, "Invalid manifest version");
    ESP_GOTO_ON_ERROR(copy_json_string(root, "url", artifact->url, sizeof(artifact->url)),
                      cleanup, TAG, "Invalid manifest URL");
    ESP_GOTO_ON_ERROR(copy_json_string(root, "sha256", artifact->sha256_hex,
                                       sizeof(artifact->sha256_hex)),
                      cleanup, TAG, "Invalid manifest SHA-256");
    ESP_GOTO_ON_ERROR(copy_optional_json_string(root, "channel",
                                               artifact->channel,
                                               sizeof(artifact->channel)),
                      cleanup, TAG, "Invalid manifest channel");
    ESP_GOTO_ON_ERROR(copy_optional_json_string(root, "build_id",
                                               artifact->build_id,
                                               sizeof(artifact->build_id)),
                      cleanup, TAG, "Invalid manifest build_id");

    if (request->channel != NULL && request->channel[0] != '\0' &&
        artifact->channel[0] != '\0' && strcmp(artifact->channel, request->channel) != 0) {
        ESP_LOGE(TAG, "Unexpected manifest channel: %s", artifact->channel);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (strncmp(artifact->url, "https://", strlen("https://")) != 0) {
        ESP_LOGE(TAG, "Manifest URL must use https");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (strlen(artifact->sha256_hex) != MANIFEST_FILE_SHA256_HEX_CHARS) {
        ESP_LOGE(TAG, "Manifest SHA-256 is invalid: %s", artifact->sha256_hex);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    uint8_t unused_sha[MANIFEST_FILE_SHA256_BYTES];
    ESP_GOTO_ON_ERROR(sha256_hex_to_bytes(artifact->sha256_hex, unused_sha),
                      cleanup, TAG, "Manifest SHA-256 is invalid");

    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0) {
        ESP_LOGE(TAG, "Manifest size is missing or invalid");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    artifact->size = (size_t)size->valuedouble;
    if (request->max_artifact_size > 0 && artifact->size > request->max_artifact_size) {
        ESP_LOGE(TAG, "Manifest artifact exceeds max size: %u", (unsigned)artifact->size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    cJSON *critical = cJSON_GetObjectItemCaseSensitive(root, "critical");
    artifact->critical = cJSON_IsBool(critical) && cJSON_IsTrue(critical);

cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t download_text(const char *url, char *out_text, size_t out_size)
{
    if (url == NULL || url[0] == '\0' || out_text == NULL || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "https://", strlen("https://")) != 0) {
        ESP_LOGE(TAG, "Manifest URL must use https");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CONFIG_MANIFEST_FILE_UPDATER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CONFIG_MANIFEST_FILE_UPDATER_MAX_REDIRECTS,
        .buffer_size = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE,
        .buffer_size_tx = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    int64_t content_length = 0;
    int status = 0;
    for (int redirect_count = 0; redirect_count <= CONFIG_MANIFEST_FILE_UPDATER_MAX_REDIRECTS; redirect_count++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Manifest HTTP open failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Manifest HTTP fetch headers failed: %lld", content_length);
            err = ESP_FAIL;
            goto cleanup;
        }

        status = esp_http_client_get_status_code(client);
        if (!is_http_redirect_status(status)) {
            break;
        }

        ESP_LOGI(TAG, "Following manifest HTTP redirect: status=%d", status);
        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Manifest HTTP redirect failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected manifest HTTP status: %d", status);
        err = ESP_FAIL;
        goto cleanup;
    }
    if (content_length >= (int64_t)out_size) {
        ESP_LOGE(TAG, "Manifest is too large: %lld", content_length);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    int read_len = esp_http_client_read_response(client, out_text, out_size - 1);
    if (read_len < 0) {
        ESP_LOGE(TAG, "Manifest HTTP read failed");
        err = ESP_FAIL;
        goto cleanup;
    }
    out_text[read_len] = '\0';

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t manifest_file_updater_probe_https(const char *url,
                                            int *out_status,
                                            int64_t *out_content_length)
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
        .timeout_ms = CONFIG_MANIFEST_FILE_UPDATER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CONFIG_MANIFEST_FILE_UPDATER_MAX_REDIRECTS,
        .buffer_size = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE,
        .buffer_size_tx = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    int64_t content_length = 0;
    int status = 0;
    for (int redirect_count = 0; redirect_count <= CONFIG_MANIFEST_FILE_UPDATER_MAX_REDIRECTS; redirect_count++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Probe HTTP open failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Probe HTTP fetch headers failed: %lld", content_length);
            err = ESP_FAIL;
            goto cleanup;
        }

        status = esp_http_client_get_status_code(client);
        if (!is_http_redirect_status(status)) {
            break;
        }

        ESP_LOGI(TAG, "Following probe HTTP redirect: status=%d", status);
        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Probe HTTP redirect failed: %s", esp_err_to_name(err));
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

static void build_temp_path(const manifest_file_update_request_t *request,
                            char *path,
                            size_t path_size)
{
    snprintf(path, path_size, "%s/%s", request->work_dir,
             MANIFEST_FILE_UPDATER_TEMP_FILENAME);
}

static esp_err_t download_artifact_to_file_verified(const manifest_file_artifact_t *artifact,
                                                    const manifest_file_update_request_t *request,
                                                    const char *dest_path)
{
    if (artifact == NULL || request == NULL || dest_path == NULL || dest_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (artifact->size == 0 ||
        (request->max_artifact_size > 0 && artifact->size > request->max_artifact_size)) {
        ESP_LOGE(TAG, "Invalid artifact size: %u", (unsigned)artifact->size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t expected_sha256[MANIFEST_FILE_SHA256_BYTES];
    ESP_RETURN_ON_ERROR(sha256_hex_to_bytes(artifact->sha256_hex, expected_sha256),
                        TAG, "Invalid artifact SHA-256");

    esp_http_client_config_t config = {
        .url = artifact->url,
        .timeout_ms = CONFIG_MANIFEST_FILE_UPDATER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CONFIG_MANIFEST_FILE_UPDATER_MAX_REDIRECTS,
        .buffer_size = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE,
        .buffer_size_tx = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    FILE *file = NULL;
    uint8_t *buffer = NULL;
    esp_err_t err = ESP_OK;
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);

    if (mbedtls_sha256_starts(&sha_ctx, 0) != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    int64_t content_length = 0;
    int status = 0;
    for (int redirect_count = 0; redirect_count <= CONFIG_MANIFEST_FILE_UPDATER_MAX_REDIRECTS; redirect_count++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Artifact HTTP open failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Artifact HTTP fetch headers failed: %lld", content_length);
            err = ESP_FAIL;
            goto cleanup;
        }

        status = esp_http_client_get_status_code(client);
        if (!is_http_redirect_status(status)) {
            break;
        }

        ESP_LOGI(TAG, "Following artifact HTTP redirect: status=%d", status);
        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Artifact HTTP redirect failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected artifact HTTP status: %d", status);
        err = ESP_FAIL;
        goto cleanup;
    }
    if (content_length > 0 && (size_t)content_length != artifact->size) {
        ESP_LOGE(TAG, "Artifact content length mismatch: expected=%u actual=%lld",
                 (unsigned)artifact->size, content_length);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    unlink(dest_path);
    file = fopen(dest_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open artifact temp file %s: errno=%d", dest_path, errno);
        err = ESP_FAIL;
        goto cleanup;
    }

    buffer = malloc(CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE);
    if (buffer == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t written_size = 0;
    while (content_length <= 0 || written_size < (size_t)content_length) {
        int bytes_to_read = CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE;
        if (content_length > 0) {
            size_t remaining = (size_t)content_length - written_size;
            bytes_to_read = remaining < (size_t)CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE ?
                            (int)remaining : CONFIG_MANIFEST_FILE_UPDATER_DOWNLOAD_BUFFER_SIZE;
        }

        int read_len = esp_http_client_read(client, (char *)buffer, bytes_to_read);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Artifact HTTP read failed");
            err = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (written_size > artifact->size || (size_t)read_len > artifact->size - written_size) {
            ESP_LOGE(TAG, "Artifact exceeds manifest size");
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }

        if (fwrite(buffer, 1, (size_t)read_len, file) != (size_t)read_len) {
            ESP_LOGE(TAG, "Failed to write artifact temp file: errno=%d", errno);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (mbedtls_sha256_update(&sha_ctx, buffer, (size_t)read_len) != 0) {
            err = ESP_FAIL;
            goto cleanup;
        }
        written_size += (size_t)read_len;
    }

    if (written_size != artifact->size) {
        ESP_LOGE(TAG, "Artifact size mismatch: expected=%u actual=%u",
                 (unsigned)artifact->size, (unsigned)written_size);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    uint8_t actual_sha256[MANIFEST_FILE_SHA256_BYTES];
    if (mbedtls_sha256_finish(&sha_ctx, actual_sha256) != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }
    if (memcmp(actual_sha256, expected_sha256, sizeof(actual_sha256)) != 0) {
        ESP_LOGE(TAG, "Artifact SHA-256 mismatch");
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "Failed to flush artifact temp file: errno=%d", errno);
        err = ESP_FAIL;
        goto cleanup;
    }
    if (fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG, "Failed to sync artifact temp file: errno=%d", errno);
        err = ESP_FAIL;
        goto cleanup;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed to close artifact temp file: errno=%d", errno);
        file = NULL;
        err = ESP_FAIL;
        goto cleanup;
    }
    file = NULL;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if (err != ESP_OK) {
        unlink(dest_path);
    }
    mbedtls_sha256_free(&sha_ctx);
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t manifest_file_updater_fetch_manifest(const char *manifest_url,
                                               const manifest_file_update_request_t *request,
                                               manifest_file_artifact_t *out_artifact)
{
    if (manifest_url == NULL || manifest_url[0] == '\0' || request == NULL ||
        out_artifact == NULL || request->max_manifest_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    char *manifest_text = calloc(1, request->max_manifest_size);
    if (manifest_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = download_text(manifest_url, manifest_text, request->max_manifest_size);
    if (err == ESP_OK) {
        err = parse_manifest(manifest_text, request, out_artifact);
    }

    free(manifest_text);
    return err;
}

esp_err_t manifest_file_updater_apply_artifact(const manifest_file_artifact_t *artifact,
                                               const manifest_file_update_request_t *request,
                                               manifest_file_apply_fn apply,
                                               void *user_ctx)
{
    if (artifact == NULL || request == NULL || apply == NULL ||
        request->work_dir == NULL || request->work_dir[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char temp_path[128];
    build_temp_path(request, temp_path, sizeof(temp_path));
    esp_err_t err = download_artifact_to_file_verified(artifact, request, temp_path);
    if (err == ESP_OK) {
        err = apply(temp_path, artifact, user_ctx);
    }
    unlink(temp_path);
    return err;
}

esp_err_t manifest_file_updater_run(const char *manifest_url,
                                    const manifest_file_update_request_t *request,
                                    manifest_file_apply_fn apply,
                                    void *user_ctx,
                                    manifest_file_updater_result_t *out_result)
{
    if (request == NULL || apply == NULL || request->work_dir == NULL ||
        request->work_dir[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_result != NULL) {
        *out_result = MANIFEST_FILE_UPDATER_RESULT_UNKNOWN;
    }

    manifest_file_artifact_t artifact;
    ESP_RETURN_ON_ERROR(manifest_file_updater_fetch_manifest(manifest_url, request, &artifact),
                        TAG, "Manifest fetch/validation failed");

    if (!request->allow_same_version &&
        request->current_version != NULL &&
        request->current_version[0] != '\0' &&
        strcmp(request->current_version, artifact.version) == 0) {
        ESP_LOGI(TAG, "Artifact version %s is already current", artifact.version);
        if (out_result != NULL) {
            *out_result = MANIFEST_FILE_UPDATER_RESULT_SKIPPED_CURRENT;
        }
        return ESP_OK;
    }

    esp_err_t err = manifest_file_updater_apply_artifact(&artifact, request, apply, user_ctx);
    if (err == ESP_OK && out_result != NULL) {
        *out_result = MANIFEST_FILE_UPDATER_RESULT_APPLIED;
    }
    return err;
}

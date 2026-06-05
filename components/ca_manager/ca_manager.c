#include "ca_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/private/sha256.h"

#define CA_MANAGER_DOWNLOAD_BUFFER_SIZE 4096
#define CA_MANAGER_HTTP_TX_BUFFER_SIZE 4096
#define CA_MANAGER_MAX_HTTP_REDIRECTS 5
#define CA_MANAGER_SHA256_SIZE 32
#define CA_MANAGER_SHA256_HEX_SIZE 65

struct ca_manager_update_ctx {
    FILE *file;
    char active_path[128];
    char temp_path[128];
    size_t expected_size;
    size_t written_size;
    bool promoted;
    bool active;
};

static const char *TAG = "ca_manager";
static uint8_t *s_active_bundle;
static size_t s_active_bundle_size;
static bool s_spiffs_mounted;

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

static esp_err_t sha256_hex_to_bytes(const char *hex, uint8_t out[CA_MANAGER_SHA256_SIZE])
{
    if (hex == NULL || strlen(hex) != CA_MANAGER_SHA256_HEX_SIZE - 1) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < CA_MANAGER_SHA256_SIZE; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    return ESP_OK;
}

static void sha256_bytes_to_hex(const uint8_t bytes[CA_MANAGER_SHA256_SIZE],
                                char out[CA_MANAGER_SHA256_HEX_SIZE])
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < CA_MANAGER_SHA256_SIZE; i++) {
        out[i * 2] = hex_chars[bytes[i] >> 4];
        out[i * 2 + 1] = hex_chars[bytes[i] & 0x0f];
    }
    out[CA_MANAGER_SHA256_HEX_SIZE - 1] = '\0';
}

static void build_path(char *out, size_t out_size, const char *filename)
{
    int written = snprintf(out, out_size, "%s/%s", CONFIG_CA_UPDATER_SPIFFS_BASE_PATH, filename);
    if (written < 0 || (size_t)written >= out_size) {
        ESP_LOGE(TAG, "Path truncated for %s", filename);
    }
}

static void build_default_paths(char *active_path, size_t active_size,
                                char *temp_path, size_t temp_size,
                                char *backup_path, size_t backup_size)
{
    build_path(active_path, active_size, CONFIG_CA_UPDATER_BUNDLE_FILENAME);
    snprintf(temp_path, temp_size, "%s.tmp", active_path);
    snprintf(backup_path, backup_size, "%s.bak", active_path);
}

static void build_version_path(char *version_path, size_t version_size)
{
    build_path(version_path, version_size, CONFIG_CA_UPDATER_VERSION_FILENAME);
}

static void build_version_temp_path(char *version_temp_path, size_t version_temp_size)
{
    build_path(version_temp_path, version_temp_size, CONFIG_CA_UPDATER_VERSION_FILENAME ".tmp");
}

static void build_version_backup_path(char *version_backup_path, size_t version_backup_size)
{
    build_path(version_backup_path, version_backup_size, CONFIG_CA_UPDATER_VERSION_FILENAME ".bak");
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static esp_err_t mount_spiffs(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_CA_UPDATER_SPIFFS_BASE_PATH,
        .partition_label = CONFIG_CA_UPDATER_STORAGE_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "Failed to mount SPIFFS");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(CONFIG_CA_UPDATER_STORAGE_LABEL, &total, &used),
                        TAG, "Failed to read SPIFFS info");

    s_spiffs_mounted = true;
    ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

static esp_err_t read_file_to_memory(const char *path, uint8_t **out_data, size_t *out_size)
{
    if (path == NULL || out_data == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_size = 0;

    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (st.st_size <= 0 || st.st_size > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE) {
        ESP_LOGE(TAG, "Invalid bundle size at %s: %ld", path, (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    uint8_t *data = malloc((size_t)st.st_size);
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = fread(data, 1, (size_t)st.st_size, file);
    int close_err = fclose(file);

    if (read_len != (size_t)st.st_size || close_err != 0) {
        free(data);
        ESP_LOGE(TAG, "Failed to read %s: read=%u expected=%ld errno=%d",
                 path, (unsigned)read_len, (long)st.st_size, errno);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_size = read_len;
    return ESP_OK;
}

static esp_err_t set_active_bundle(uint8_t *bundle, size_t bundle_size, const char *path)
{
    esp_err_t err = esp_crt_bundle_set(bundle, bundle_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rejected CA bundle from %s: %s", path, esp_err_to_name(err));
        return err;
    }

    free(s_active_bundle);
    s_active_bundle = bundle;
    s_active_bundle_size = bundle_size;
    ESP_LOGI(TAG, "Activated CA bundle from %s (%u bytes)", path, (unsigned)bundle_size);
    return ESP_OK;
}

static esp_err_t activate_bundle_file(const char *path)
{
    uint8_t *bundle = NULL;
    size_t bundle_size = 0;
    esp_err_t err = read_file_to_memory(path, &bundle, &bundle_size);
    if (err != ESP_OK) {
        return err;
    }

    err = set_active_bundle(bundle, bundle_size, path);
    if (err != ESP_OK) {
        free(bundle);
    }

    return err;
}

static esp_err_t recover_interrupted_promotion(void)
{
    char active_path[128];
    char temp_path[128];
    char backup_path[128];
    build_default_paths(active_path, sizeof(active_path),
                        temp_path, sizeof(temp_path),
                        backup_path, sizeof(backup_path));

    if (!file_exists(active_path) && file_exists(backup_path)) {
        if (rename(backup_path, active_path) != 0) {
            ESP_LOGE(TAG, "Failed to recover backup bundle: errno=%d", errno);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "Recovered CA bundle from backup");
    }

    if (file_exists(active_path) && file_exists(backup_path)) {
        unlink(backup_path);
    }

    if (file_exists(temp_path)) {
        unlink(temp_path);
    }

    char version_path[128];
    build_version_path(version_path, sizeof(version_path));
    char version_backup_path[128];
    build_version_backup_path(version_backup_path, sizeof(version_backup_path));
    if (!file_exists(version_path) && file_exists(version_backup_path)) {
        if (rename(version_backup_path, version_path) != 0) {
            ESP_LOGE(TAG, "Failed to recover backup version: errno=%d", errno);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "Recovered CA bundle version from backup");
    }

    if (file_exists(version_path) && file_exists(version_backup_path)) {
        unlink(version_backup_path);
    }

    char version_temp_path[128];
    build_version_temp_path(version_temp_path, sizeof(version_temp_path));
    if (file_exists(version_temp_path)) {
        unlink(version_temp_path);
    }

    return ESP_OK;
}

static esp_err_t promote_temp_bundle(const char *temp_path, const char *active_path)
{
    char backup_path[128];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", active_path);
    unlink(backup_path);

    bool has_active = file_exists(active_path);
    if (has_active && rename(active_path, backup_path) != 0) {
        ESP_LOGE(TAG, "Failed to stage old bundle as backup: errno=%d", errno);
        return ESP_FAIL;
    }

    if (rename(temp_path, active_path) != 0) {
        ESP_LOGE(TAG, "Failed to promote bundle %s -> %s: errno=%d", temp_path, active_path, errno);
        if (has_active && rename(backup_path, active_path) != 0) {
            ESP_LOGE(TAG, "Failed to restore previous CA bundle: errno=%d", errno);
        }
        return ESP_FAIL;
    }

    if (has_active) {
        unlink(backup_path);
    }

    ESP_LOGI(TAG, "Bundle update promoted to %s", active_path);
    return ESP_OK;
}

static esp_err_t promote_temp_version_file(const char *temp_path, const char *version_path)
{
    char backup_path[128];
    build_version_backup_path(backup_path, sizeof(backup_path));
    unlink(backup_path);

    bool has_version = file_exists(version_path);
    if (has_version && rename(version_path, backup_path) != 0) {
        ESP_LOGE(TAG, "Failed to stage old version as backup: errno=%d", errno);
        return ESP_FAIL;
    }

    if (rename(temp_path, version_path) != 0) {
        ESP_LOGE(TAG, "Failed to promote version file %s -> %s: errno=%d",
                 temp_path, version_path, errno);
        if (has_version && rename(backup_path, version_path) != 0) {
            ESP_LOGE(TAG, "Failed to restore previous version file: errno=%d", errno);
        }
        return ESP_FAIL;
    }

    if (has_version) {
        unlink(backup_path);
    }

    return ESP_OK;
}

static esp_err_t copy_file(const char *source_path, const char *dest_path, size_t *out_size)
{
    if (source_path == NULL || dest_path == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = 0;

    FILE *source = fopen(source_path, "rb");
    if (source == NULL) {
        ESP_LOGE(TAG, "Failed to open source file %s: errno=%d", source_path, errno);
        return ESP_FAIL;
    }

    FILE *dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        ESP_LOGE(TAG, "Failed to open destination file %s: errno=%d", dest_path, errno);
        fclose(source);
        return ESP_FAIL;
    }

    uint8_t buffer[1024];
    esp_err_t err = ESP_OK;

    while (true) {
        size_t read_len = fread(buffer, 1, sizeof(buffer), source);
        if (read_len > 0) {
            if (*out_size > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE ||
                read_len > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE - *out_size) {
                ESP_LOGE(TAG, "Source bundle exceeds maximum size");
                err = ESP_ERR_INVALID_SIZE;
                break;
            }

            size_t written = fwrite(buffer, 1, read_len, dest);
            if (written != read_len) {
                ESP_LOGE(TAG, "Failed to copy bundle file: errno=%d", errno);
                err = ESP_FAIL;
                break;
            }

            *out_size += written;
        }

        if (read_len < sizeof(buffer)) {
            if (ferror(source)) {
                ESP_LOGE(TAG, "Failed to read source file %s: errno=%d", source_path, errno);
                err = ESP_FAIL;
            }
            break;
        }
    }

    if (fflush(dest) != 0 && err == ESP_OK) {
        ESP_LOGE(TAG, "Failed to flush copied bundle: errno=%d", errno);
        err = ESP_FAIL;
    }

    if (err == ESP_OK && fsync(fileno(dest)) != 0) {
        ESP_LOGE(TAG, "Failed to sync copied bundle: errno=%d", errno);
        err = ESP_FAIL;
    }

    if (fclose(dest) != 0 && err == ESP_OK) {
        ESP_LOGE(TAG, "Failed to close copied bundle: errno=%d", errno);
        err = ESP_FAIL;
    }

    fclose(source);
    return err;
}

static esp_err_t validate_and_promote(ca_manager_update_ctx_t *ctx,
                                      bool restart_on_success,
                                      bool *out_promoted)
{
    if (ctx == NULL || !ctx->active) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->promoted = false;
    if (out_promoted != NULL) {
        *out_promoted = false;
    }

    if (ctx->expected_size > 0 && ctx->written_size != ctx->expected_size) {
        ESP_LOGE(TAG, "Incomplete bundle stream: %u/%u",
                 (unsigned)ctx->written_size, (unsigned)ctx->expected_size);
        return ESP_FAIL;
    }

    if (ctx->written_size == 0 || ctx->written_size > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE) {
        ESP_LOGE(TAG, "Invalid downloaded bundle size: %u", (unsigned)ctx->written_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *new_bundle = NULL;
    size_t new_bundle_size = 0;
    esp_err_t err = read_file_to_memory(ctx->temp_path, &new_bundle, &new_bundle_size);
    if (err != ESP_OK) {
        return err;
    }

    if (s_active_bundle != NULL &&
        s_active_bundle_size == new_bundle_size &&
        memcmp(s_active_bundle, new_bundle, new_bundle_size) == 0) {
        ESP_LOGI(TAG, "Downloaded CA bundle is already active; skipping promotion");
        free(new_bundle);
        unlink(ctx->temp_path);
        return ESP_OK;
    }

    uint8_t *old_bundle = s_active_bundle;
    size_t old_bundle_size = s_active_bundle_size;
    s_active_bundle = NULL;
    s_active_bundle_size = 0;

    err = set_active_bundle(new_bundle, new_bundle_size, ctx->temp_path);
    if (err != ESP_OK) {
        s_active_bundle = old_bundle;
        s_active_bundle_size = old_bundle_size;
        free(new_bundle);
        return err;
    }

    err = promote_temp_bundle(ctx->temp_path, ctx->active_path);
    if (err != ESP_OK) {
        if (old_bundle != NULL) {
            esp_err_t restore_err = esp_crt_bundle_set(old_bundle, old_bundle_size);
            if (restore_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore previous in-memory CA bundle: %s",
                         esp_err_to_name(restore_err));
            }
            free(s_active_bundle);
            s_active_bundle = old_bundle;
            s_active_bundle_size = old_bundle_size;
        }
        return err;
    }

    free(old_bundle);
    ctx->promoted = true;
    if (out_promoted != NULL) {
        *out_promoted = true;
    }

    if (restart_on_success) {
        ESP_LOGI(TAG, "CA bundle updated successfully; restarting");
        fflush(stdout);
        esp_restart();
    }

    return ESP_OK;
}

esp_err_t ca_manager_init(void)
{
    ESP_RETURN_ON_ERROR(mount_spiffs(), TAG, "SPIFFS init failed");
    ESP_RETURN_ON_ERROR(recover_interrupted_promotion(), TAG, "Bundle promotion recovery failed");

    char active_path[128];
    build_path(active_path, sizeof(active_path), CONFIG_CA_UPDATER_BUNDLE_FILENAME);

    esp_err_t err = activate_bundle_file(active_path);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No SPIFFS CA bundle found yet; using embedded ESP-IDF bundle");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stored CA bundle is invalid; using embedded ESP-IDF bundle");
        return ESP_OK;
    }

    return ESP_OK;
}

size_t ca_manager_get_active_bundle_size(void)
{
    return s_active_bundle_size;
}

esp_err_t ca_manager_get_active_version(char *out_version, size_t out_size)
{
    if (out_version == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_version[0] = '\0';

    char version_path[128];
    build_version_path(version_path, sizeof(version_path));

    FILE *file = fopen(version_path, "r");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fgets(out_version, out_size, file) == NULL) {
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);
    out_version[strcspn(out_version, "\r\n")] = '\0';
    return ESP_OK;
}

esp_err_t ca_manager_set_active_version(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char version_path[128];
    build_version_path(version_path, sizeof(version_path));
    char temp_path[128];
    build_version_temp_path(temp_path, sizeof(temp_path));

    FILE *file = fopen(temp_path, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open version file %s: errno=%d", temp_path, errno);
        return ESP_FAIL;
    }

    if (fprintf(file, "%s\n", version) < 0) {
        ESP_LOGE(TAG, "Failed to write version file %s: errno=%d", temp_path, errno);
        fclose(file);
        unlink(temp_path);
        return ESP_FAIL;
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG, "Failed to sync version file %s: errno=%d", temp_path, errno);
        fclose(file);
        unlink(temp_path);
        return ESP_FAIL;
    }

    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed to close version file %s: errno=%d", temp_path, errno);
        unlink(temp_path);
        return ESP_FAIL;
    }

    if (promote_temp_version_file(temp_path, version_path) != ESP_OK) {
        unlink(temp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Stored CA bundle version %s", version);
    return ESP_OK;
}

esp_err_t ca_manager_active_bundle_matches_sha256(const char *expected_sha256_hex, bool *out_matches)
{
    if (expected_sha256_hex == NULL || out_matches == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_matches = false;

    uint8_t expected_sha256[CA_MANAGER_SHA256_SIZE];
    ESP_RETURN_ON_ERROR(sha256_hex_to_bytes(expected_sha256_hex, expected_sha256),
                        TAG, "Invalid expected SHA-256");

    if (s_active_bundle == NULL || s_active_bundle_size == 0) {
        return ESP_OK;
    }

    uint8_t actual_sha256[CA_MANAGER_SHA256_SIZE];
    if (mbedtls_sha256(s_active_bundle, s_active_bundle_size, actual_sha256, 0) != 0) {
        return ESP_FAIL;
    }

    *out_matches = memcmp(actual_sha256, expected_sha256, sizeof(actual_sha256)) == 0;
    return ESP_OK;
}

esp_err_t ca_manager_update_begin(ca_manager_update_ctx_t **out_ctx, size_t expected_size)
{
    if (out_ctx == NULL || expected_size > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_ctx = NULL;

    ca_manager_update_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char backup_path[128];
    build_default_paths(ctx->active_path, sizeof(ctx->active_path),
                        ctx->temp_path, sizeof(ctx->temp_path),
                        backup_path, sizeof(backup_path));

    unlink(ctx->temp_path);

    ctx->file = fopen(ctx->temp_path, "wb");
    if (ctx->file == NULL) {
        ESP_LOGE(TAG, "Failed to open temp file %s: errno=%d", ctx->temp_path, errno);
        free(ctx);
        return ESP_FAIL;
    }

    ctx->expected_size = expected_size;
    ctx->active = true;
    *out_ctx = ctx;
    return ESP_OK;
}

esp_err_t ca_manager_update_write(ca_manager_update_ctx_t *ctx, const uint8_t *data, size_t data_len)
{
    if (ctx == NULL || !ctx->active || ctx->file == NULL || (data == NULL && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len == 0) {
        return ESP_OK;
    }

    if (ctx->written_size > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE ||
        data_len > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE - ctx->written_size) {
        ESP_LOGE(TAG, "Bundle stream exceeds maximum size");
        return ESP_ERR_INVALID_SIZE;
    }

    size_t written = fwrite(data, 1, data_len, ctx->file);
    if (written != data_len) {
        ESP_LOGE(TAG, "SPIFFS write failed: errno=%d", errno);
        return ESP_FAIL;
    }

    ctx->written_size += written;
    return ESP_OK;
}

static esp_err_t finish_update(ca_manager_update_ctx_t *ctx,
                               bool restart_on_success,
                               bool *out_promoted)
{
    if (ctx == NULL || !ctx->active || ctx->file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_promoted != NULL) {
        *out_promoted = false;
    }

    esp_err_t err = ESP_OK;
    if (fflush(ctx->file) != 0) {
        ESP_LOGE(TAG, "Failed to flush temp bundle: errno=%d", errno);
        err = ESP_FAIL;
    }

    if (err == ESP_OK && fsync(fileno(ctx->file)) != 0) {
        ESP_LOGE(TAG, "Failed to sync temp bundle: errno=%d", errno);
        err = ESP_FAIL;
    }

    if (fclose(ctx->file) != 0 && err == ESP_OK) {
        ESP_LOGE(TAG, "Failed to close temp bundle: errno=%d", errno);
        err = ESP_FAIL;
    }

    ctx->file = NULL;

    if (err == ESP_OK) {
        err = validate_and_promote(ctx, restart_on_success, out_promoted);
    }

    if (err != ESP_OK) {
        unlink(ctx->temp_path);
    }

    ctx->active = false;
    free(ctx);
    return err;
}

esp_err_t ca_manager_update_finish(ca_manager_update_ctx_t *ctx, bool restart_on_success)
{
    return finish_update(ctx, restart_on_success, NULL);
}

void ca_manager_update_abort(ca_manager_update_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->file != NULL) {
        fclose(ctx->file);
        ctx->file = NULL;
    }

    if (ctx->active) {
        unlink(ctx->temp_path);
    }

    ctx->active = false;
    free(ctx);
}

esp_err_t ca_manager_update_from_stream(const uint8_t *data, size_t data_len, bool restart_on_success)
{
    ca_manager_update_ctx_t *ctx = NULL;
    ESP_RETURN_ON_ERROR(ca_manager_update_begin(&ctx, data_len), TAG, "Failed to begin stream update");

    esp_err_t err = ca_manager_update_write(ctx, data, data_len);
    if (err == ESP_OK) {
        err = ca_manager_update_finish(ctx, restart_on_success);
    } else {
        ca_manager_update_abort(ctx);
    }

    return err;
}

esp_err_t ca_manager_apply_file(const char *path, bool restart_on_success)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ca_manager_update_ctx_t *ctx = NULL;
    ESP_RETURN_ON_ERROR(ca_manager_update_begin(&ctx, 0), TAG, "Failed to begin file apply");

    FILE *temp_file = ctx->file;
    ctx->file = NULL;
    if (fclose(temp_file) != 0) {
        ESP_LOGE(TAG, "Failed to close temp file before apply copy: errno=%d", errno);
        ca_manager_update_abort(ctx);
        return ESP_FAIL;
    }

    esp_err_t err = copy_file(path, ctx->temp_path, &ctx->written_size);
    if (err == ESP_OK) {
        err = validate_and_promote(ctx, restart_on_success, NULL);
    }

    if (err != ESP_OK) {
        unlink(ctx->temp_path);
    }

    ctx->active = false;
    free(ctx);
    return err;
}

esp_err_t ca_manager_update_from_http_client_verified(const char *url,
                                                      const char *expected_sha256_hex,
                                                      size_t expected_size,
                                                      bool restart_on_success,
                                                      bool *out_promoted)
{
    if (url == NULL || url[0] == '\0') {
        ESP_LOGW(TAG, "Bundle URL is empty; nothing to update");
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_size > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE) {
        ESP_LOGE(TAG, "Invalid expected bundle size: %u", (unsigned)expected_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (out_promoted != NULL) {
        *out_promoted = false;
    }

    uint8_t expected_sha256[CA_MANAGER_SHA256_SIZE];
    bool verify_sha256 = expected_sha256_hex != NULL && expected_sha256_hex[0] != '\0';
    if (verify_sha256) {
        esp_err_t parse_err = sha256_hex_to_bytes(expected_sha256_hex, expected_sha256);
        if (parse_err != ESP_OK) {
            ESP_LOGE(TAG, "Invalid expected SHA-256: %s", expected_sha256_hex);
            return parse_err;
        }
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CONFIG_CA_UPDATER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CA_MANAGER_MAX_HTTP_REDIRECTS,
        .buffer_size_tx = CA_MANAGER_HTTP_TX_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    ca_manager_update_ctx_t *ctx = NULL;
    uint8_t *buffer = NULL;
    esp_err_t err = ESP_OK;
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    if (verify_sha256 && mbedtls_sha256_starts(&sha_ctx, 0) != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    int64_t content_length = 0;
    int status = 0;
    for (int redirect_count = 0; redirect_count <= CA_MANAGER_MAX_HTTP_REDIRECTS; redirect_count++) {
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

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (content_length > CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE) {
        ESP_LOGE(TAG, "Invalid content length: %lld", content_length);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (expected_size > 0 && content_length > 0 && (size_t)content_length != expected_size) {
        ESP_LOGE(TAG, "Content length mismatch: expected=%u actual=%lld",
                 (unsigned)expected_size, content_length);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    size_t update_expected_size = expected_size;
    if (update_expected_size == 0 && content_length > 0) {
        update_expected_size = (size_t)content_length;
    }

    err = ca_manager_update_begin(&ctx, update_expected_size);
    if (err != ESP_OK) {
        goto cleanup;
    }

    buffer = malloc(CA_MANAGER_DOWNLOAD_BUFFER_SIZE);
    if (buffer == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (content_length <= 0 || ctx->written_size < (size_t)content_length) {
        int bytes_to_read = CA_MANAGER_DOWNLOAD_BUFFER_SIZE;
        if (content_length > 0) {
            size_t remaining = (size_t)content_length - ctx->written_size;
            bytes_to_read = remaining < CA_MANAGER_DOWNLOAD_BUFFER_SIZE ?
                            (int)remaining : CA_MANAGER_DOWNLOAD_BUFFER_SIZE;
        }

        int read_len = esp_http_client_read(client, (char *)buffer, bytes_to_read);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read failed");
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

        err = ca_manager_update_write(ctx, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            goto cleanup;
        }
        if (verify_sha256 && mbedtls_sha256_update(&sha_ctx, buffer, (size_t)read_len) != 0) {
            err = ESP_FAIL;
            goto cleanup;
        }
    }

    if (expected_size > 0 && ctx->written_size != expected_size) {
        ESP_LOGE(TAG, "Downloaded size mismatch: expected=%u actual=%u",
                 (unsigned)expected_size, (unsigned)ctx->written_size);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (verify_sha256) {
        uint8_t actual_sha256[CA_MANAGER_SHA256_SIZE];
        if (mbedtls_sha256_finish(&sha_ctx, actual_sha256) != 0) {
            err = ESP_FAIL;
            goto cleanup;
        }

        if (memcmp(actual_sha256, expected_sha256, sizeof(actual_sha256)) != 0) {
            char actual_hex[CA_MANAGER_SHA256_HEX_SIZE];
            sha256_bytes_to_hex(actual_sha256, actual_hex);
            ESP_LOGE(TAG, "SHA-256 mismatch: expected=%s actual=%s",
                     expected_sha256_hex, actual_hex);
            err = ESP_ERR_INVALID_CRC;
            goto cleanup;
        }
    }

    err = finish_update(ctx, restart_on_success, out_promoted);
    ctx = NULL;

cleanup:
    mbedtls_sha256_free(&sha_ctx);
    free(buffer);
    if (ctx != NULL) {
        ca_manager_update_abort(ctx);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ca_manager_update_from_http_client(const char *url, bool restart_on_success)
{
    return ca_manager_update_from_http_client_verified(url, NULL, 0, restart_on_success, NULL);
}

esp_err_t ca_manager_download_text(const char *url, char *out_text, size_t out_size)
{
    if (url == NULL || url[0] == '\0' || out_text == NULL || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    out_text[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CONFIG_CA_UPDATER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CA_MANAGER_MAX_HTTP_REDIRECTS,
        .buffer_size_tx = CA_MANAGER_HTTP_TX_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    int64_t content_length = 0;
    int status = 0;
    for (int redirect_count = 0; redirect_count <= CA_MANAGER_MAX_HTTP_REDIRECTS; redirect_count++) {
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

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (content_length >= (int64_t)out_size) {
        ESP_LOGE(TAG, "Text response too large: %lld", content_length);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    size_t written = 0;
    while (content_length <= 0 || written < (size_t)content_length) {
        int bytes_to_read = (int)(out_size - 1 - written);
        if (bytes_to_read <= 0) {
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if (content_length > 0) {
            size_t remaining = (size_t)content_length - written;
            if (remaining < (size_t)bytes_to_read) {
                bytes_to_read = (int)remaining;
            }
        }

        int read_len = esp_http_client_read(client, out_text + written, bytes_to_read);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read failed");
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

        written += (size_t)read_len;
    }

    out_text[written] = '\0';

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

#include "ca_manifest_updater.h"

#include <stdio.h>
#include <string.h>

#include "ca_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"

#define CA_MANIFEST_ARTIFACT_TYPE "ca_bundle"
#define CA_MANIFEST_VERSION_BUFFER_SIZE 32

static const char *TAG = "ca_manifest";

const char *ca_manifest_updater_get_component_version(void)
{
    return CA_MANIFEST_UPDATER_COMPONENT_VERSION;
}

typedef struct {
    bool restart_on_update;
} ca_manifest_apply_ctx_t;

static void make_request(const char *current_version,
                         const char *channel,
                         manifest_file_update_request_t *request)
{
    *request = (manifest_file_update_request_t) {
        .artifact_type = CA_MANIFEST_ARTIFACT_TYPE,
        .current_version = current_version,
        .channel = channel,
        .work_dir = CONFIG_CA_UPDATER_SPIFFS_BASE_PATH,
        .max_manifest_size = CONFIG_CA_MANIFEST_UPDATER_MAX_MANIFEST_SIZE,
        .max_artifact_size = CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE,
        .allow_same_version = false,
    };
}

static esp_err_t apply_ca_bundle(const char *verified_path,
                                 const manifest_file_artifact_t *artifact,
                                 void *user_ctx)
{
    if (artifact == NULL || user_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ca_manifest_apply_ctx_t *ctx = (ca_manifest_apply_ctx_t *)user_ctx;
    ESP_RETURN_ON_ERROR(ca_manager_apply_file(verified_path, false),
                        TAG, "CA bundle apply failed");
    ESP_RETURN_ON_ERROR(ca_manager_set_active_version(artifact->version),
                        TAG, "Failed to store CA bundle version");

    if (ctx->restart_on_update) {
        ESP_LOGI(TAG, "CA bundle updated to version %s; restarting", artifact->version);
        fflush(stdout);
        esp_restart();
    }

    ESP_LOGI(TAG, "CA bundle updated to version %s", artifact->version);
    return ESP_OK;
}

esp_err_t ca_manifest_updater_fetch(const char *manifest_url,
                                    const char *channel,
                                    manifest_file_artifact_t *out_artifact)
{
    manifest_file_update_request_t request;
    make_request(NULL, channel, &request);
    return manifest_file_updater_fetch_manifest(manifest_url, &request, out_artifact);
}

esp_err_t ca_manifest_updater_run(const ca_manifest_updater_config_t *config,
                                  ca_manifest_updater_result_t *out_result)
{
    if (config == NULL || config->manifest_url == NULL || config->manifest_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_result != NULL) {
        *out_result = CA_MANIFEST_UPDATER_RESULT_UNKNOWN;
    }

    char active_version[CA_MANIFEST_VERSION_BUFFER_SIZE];
    esp_err_t err = ca_manager_get_active_version(active_version, sizeof(active_version));
    if (err != ESP_OK) {
        active_version[0] = '\0';
    }

    manifest_file_update_request_t request;
    make_request(active_version, config->channel, &request);

    manifest_file_artifact_t artifact;
    ESP_RETURN_ON_ERROR(manifest_file_updater_fetch_manifest(config->manifest_url, &request, &artifact),
                        TAG, "Manifest update metadata failed");

    if (active_version[0] != '\0' && strcmp(active_version, artifact.version) == 0) {
        ESP_LOGI(TAG, "CA bundle version %s is already active", active_version);
        if (out_result != NULL) {
            *out_result = CA_MANIFEST_UPDATER_RESULT_SKIPPED_CURRENT;
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Updating CA bundle version %s -> %s",
             active_version[0] != '\0' ? active_version : "(none)",
             artifact.version);

    bool active_bundle_matches = false;
    ESP_RETURN_ON_ERROR(ca_manager_active_bundle_matches_sha256(artifact.sha256_hex,
                                                                &active_bundle_matches),
                        TAG, "Failed to compare active bundle SHA-256");
    if (active_bundle_matches) {
        ESP_LOGI(TAG, "Active CA bundle already matches remote SHA-256");
        ESP_RETURN_ON_ERROR(ca_manager_set_active_version(artifact.version),
                            TAG, "Failed to store CA bundle version");
        if (out_result != NULL) {
            *out_result = CA_MANIFEST_UPDATER_RESULT_VERSION_CORRECTED;
        }
        ESP_LOGI(TAG, "CA bundle version corrected to %s; restart not needed", artifact.version);
        return ESP_OK;
    }

    ca_manifest_apply_ctx_t apply_ctx = {
        .restart_on_update = config->restart_on_update,
    };
    err = manifest_file_updater_apply_artifact(&artifact,
                                               &request,
                                               apply_ca_bundle,
                                               &apply_ctx);
    if (err == ESP_OK && out_result != NULL) {
        *out_result = CA_MANIFEST_UPDATER_RESULT_APPLIED;
    }
    return err;
}

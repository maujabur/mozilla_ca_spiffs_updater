#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MANIFEST_FILE_UPDATER_ARTIFACT_TYPE_SIZE 32
#define MANIFEST_FILE_UPDATER_CHANNEL_SIZE 32
#define MANIFEST_FILE_UPDATER_VERSION_SIZE 32
#define MANIFEST_FILE_UPDATER_BUILD_ID_SIZE 64
#define MANIFEST_FILE_UPDATER_URL_SIZE 256
#define MANIFEST_FILE_UPDATER_SHA256_HEX_SIZE 65
#define MANIFEST_FILE_UPDATER_TEMP_FILENAME "manifest_artifact.tmp"
#define MANIFEST_FILE_UPDATER_COMPONENT_VERSION "0.1.0"

typedef struct {
    const char *artifact_type;
    const char *current_version;
    const char *channel;
    const char *work_dir;
    size_t max_manifest_size;
    size_t max_artifact_size;
    bool allow_same_version;
} manifest_file_update_request_t;

typedef struct {
    int schema;
    char artifact_type[MANIFEST_FILE_UPDATER_ARTIFACT_TYPE_SIZE];
    char channel[MANIFEST_FILE_UPDATER_CHANNEL_SIZE];
    char version[MANIFEST_FILE_UPDATER_VERSION_SIZE];
    char build_id[MANIFEST_FILE_UPDATER_BUILD_ID_SIZE];
    char url[MANIFEST_FILE_UPDATER_URL_SIZE];
    char sha256_hex[MANIFEST_FILE_UPDATER_SHA256_HEX_SIZE];
    size_t size;
    bool critical;
} manifest_file_artifact_t;

typedef enum {
    MANIFEST_FILE_UPDATER_RESULT_UNKNOWN = 0,
    MANIFEST_FILE_UPDATER_RESULT_SKIPPED_CURRENT,
    MANIFEST_FILE_UPDATER_RESULT_APPLIED,
} manifest_file_updater_result_t;

typedef esp_err_t (*manifest_file_apply_fn)(const char *verified_path,
                                            const manifest_file_artifact_t *artifact,
                                            void *user_ctx);

const char *manifest_file_updater_get_component_version(void);

esp_err_t manifest_file_updater_probe_https(const char *url,
                                            int *out_status,
                                            int64_t *out_content_length);

esp_err_t manifest_file_updater_fetch_manifest(const char *manifest_url,
                                               const manifest_file_update_request_t *request,
                                               manifest_file_artifact_t *out_artifact);

esp_err_t manifest_file_updater_apply_artifact(const manifest_file_artifact_t *artifact,
                                               const manifest_file_update_request_t *request,
                                               manifest_file_apply_fn apply,
                                               void *user_ctx);

esp_err_t manifest_file_updater_run(const char *manifest_url,
                                    const manifest_file_update_request_t *request,
                                    manifest_file_apply_fn apply,
                                    void *user_ctx,
                                    manifest_file_updater_result_t *out_result);

#ifdef __cplusplus
}
#endif

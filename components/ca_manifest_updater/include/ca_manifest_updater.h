#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "manifest_file_updater.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CA_MANIFEST_UPDATER_COMPONENT_VERSION "0.1.0"

typedef struct {
    const char *manifest_url;
    const char *channel;
    bool restart_on_update;
} ca_manifest_updater_config_t;

typedef enum {
    CA_MANIFEST_UPDATER_RESULT_UNKNOWN = 0,
    CA_MANIFEST_UPDATER_RESULT_SKIPPED_CURRENT,
    CA_MANIFEST_UPDATER_RESULT_VERSION_CORRECTED,
    CA_MANIFEST_UPDATER_RESULT_APPLIED,
} ca_manifest_updater_result_t;

const char *ca_manifest_updater_get_component_version(void);

esp_err_t ca_manifest_updater_fetch(const char *manifest_url,
                                    const char *channel,
                                    manifest_file_artifact_t *out_artifact);

esp_err_t ca_manifest_updater_run(const ca_manifest_updater_config_t *config,
                                  ca_manifest_updater_result_t *out_result);

#ifdef __cplusplus
}
#endif

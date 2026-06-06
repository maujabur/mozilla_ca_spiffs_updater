#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_manager_update_ctx ca_manager_update_ctx_t;

#define CA_MANAGER_COMPONENT_VERSION "0.1.0"

const char *ca_manager_get_component_version(void);

esp_err_t ca_manager_init(void);
size_t ca_manager_get_active_bundle_size(void);
esp_err_t ca_manager_get_active_version(char *out_version, size_t out_size);
esp_err_t ca_manager_set_active_version(const char *version);
esp_err_t ca_manager_active_bundle_matches_sha256(const char *expected_sha256_hex, bool *out_matches);

/*
 * Update the active bundle from caller-provided bytes.
 *
 * The input must already be obtained by a trusted caller. ca_manager validates
 * that the bytes are an ESP-IDF x509_crt_bundle before promotion.
 */
esp_err_t ca_manager_update_from_stream(const uint8_t *data, size_t data_len, bool restart_on_success);

/*
 * Apply a local file that was already downloaded and verified by an external
 * updater. ca_manager does not perform HTTP, manifest parsing, size checks
 * against remote metadata, or SHA-256 verification. It copies the file into
 * managed storage, validates it with esp_crt_bundle_set(), and promotes it
 * only after validation succeeds.
 */
esp_err_t ca_manager_apply_file(const char *path, bool restart_on_success);

/*
 * Streaming API for callers that download or generate the bundle incrementally.
 * The final bundle is validated and promoted by ca_manager_update_finish().
 */
esp_err_t ca_manager_update_begin(ca_manager_update_ctx_t **out_ctx, size_t expected_size);
esp_err_t ca_manager_update_write(ca_manager_update_ctx_t *ctx, const uint8_t *data, size_t data_len);
esp_err_t ca_manager_update_finish(ca_manager_update_ctx_t *ctx, bool restart_on_success);
void ca_manager_update_abort(ca_manager_update_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

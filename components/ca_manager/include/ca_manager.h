#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_manager_update_ctx ca_manager_update_ctx_t;

esp_err_t ca_manager_init(void);
size_t ca_manager_get_active_bundle_size(void);
esp_err_t ca_manager_get_active_version(char *out_version, size_t out_size);
esp_err_t ca_manager_set_active_version(const char *version);

esp_err_t ca_manager_update_from_stream(const uint8_t *data, size_t data_len, bool restart_on_success);
esp_err_t ca_manager_apply_file(const char *path, bool restart_on_success);

esp_err_t ca_manager_update_begin(ca_manager_update_ctx_t **out_ctx, size_t expected_size);
esp_err_t ca_manager_update_write(ca_manager_update_ctx_t *ctx, const uint8_t *data, size_t data_len);
esp_err_t ca_manager_update_finish(ca_manager_update_ctx_t *ctx, bool restart_on_success);
void ca_manager_update_abort(ca_manager_update_ctx_t *ctx);

esp_err_t ca_manager_update_from_http_client(const char *url, bool restart_on_success);
esp_err_t ca_manager_update_from_http_client_verified(const char *url,
                                                      const char *expected_sha256_hex,
                                                      bool restart_on_success,
                                                      bool *out_promoted);
esp_err_t ca_manager_download_text(const char *url, char *out_text, size_t out_size);

#ifdef __cplusplus
}
#endif

# CA Manager Component Apply Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `ca_manager` into a reusable ESP-IDF component and add an apply-style file API that future manifest code can call after artifact verification.

**Architecture:** `components/ca_manager` owns CA bundle storage, validation, activation, and promotion. `main/` remains an example application that owns Wi-Fi and example update behavior. The first increment preserves existing behavior while creating the boundary needed for `manifest_file_updater`.

**Tech Stack:** ESP-IDF C component layout, SPIFFS, `esp_crt_bundle`, `esp_http_client`, CMake, Kconfig.

---

## File Structure

- Create: `components/ca_manager/CMakeLists.txt`
- Create: `components/ca_manager/Kconfig`
- Create: `components/ca_manager/ca_manager.c`
- Create: `components/ca_manager/include/ca_manager.h`
- Modify: `main/CMakeLists.txt`
- Modify: `main/Kconfig.projbuild`
- Modify: `main/main.c`
- Modify: `README.md`

The reusable code moves out of `main/`. `main/` keeps only the example app and Wi-Fi configuration.

## Task 1: Move CA Manager Into A Component

**Files:**
- Create: `components/ca_manager/CMakeLists.txt`
- Create: `components/ca_manager/Kconfig`
- Move: `main/ca_manager.c` to `components/ca_manager/ca_manager.c`
- Move: `main/ca_manager.h` to `components/ca_manager/include/ca_manager.h`
- Modify: `main/CMakeLists.txt`
- Modify: `main/Kconfig.projbuild`

- [ ] **Step 1: Move the files**

Run:

```bash
mkdir -p components/ca_manager/include
git mv main/ca_manager.c components/ca_manager/ca_manager.c
git mv main/ca_manager.h components/ca_manager/include/ca_manager.h
```

Expected: files are moved with git history preserved.

- [ ] **Step 2: Create the component CMake file**

Create `components/ca_manager/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS
        "ca_manager.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_http_client
        mbedtls
        spiffs
)
```

- [ ] **Step 3: Create the component Kconfig**

Create `components/ca_manager/Kconfig`:

```text
menu "CA manager"

    config CA_UPDATER_STORAGE_LABEL
        string "SPIFFS partition label"
        default "ca_store"

    config CA_UPDATER_SPIFFS_BASE_PATH
        string "SPIFFS mount path"
        default "/spiffs"

    config CA_UPDATER_BUNDLE_FILENAME
        string "Active bundle file"
        default "bundle_ca.bin"

    config CA_UPDATER_MAX_BUNDLE_SIZE
        int "Maximum bundle size"
        default 262144

    config CA_UPDATER_HTTP_TIMEOUT_MS
        int "HTTP timeout in milliseconds"
        default 15000

endmenu
```

- [ ] **Step 4: Update the example app CMake**

Replace `main/CMakeLists.txt` with:

```cmake
idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        ca_manager
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
)
```

- [ ] **Step 5: Remove CA manager config from the example Kconfig**

Update `main/Kconfig.projbuild` so it keeps only example-app configuration:

```text
menu "Mozilla CA SPIFFS updater example"

    config CA_UPDATER_WIFI_SSID
        string "Wi-Fi SSID"
        default ""

    config CA_UPDATER_WIFI_PASSWORD
        string "Wi-Fi password"
        default ""

    config CA_UPDATER_BUNDLE_URL
        string "Mozilla CA bundle binary URL"
        default ""
        help
            HTTPS URL that serves an ESP-IDF x509_crt_bundle binary generated
            from Mozilla public CA data.

endmenu
```

- [ ] **Step 6: Build to verify component wiring**

Run:

```bash
idf.py build
```

Expected: build passes and `main/main.c` still finds `ca_manager.h`.

- [ ] **Step 7: Commit**

Run:

```bash
git add components/ca_manager main/CMakeLists.txt main/Kconfig.projbuild
git commit -m "refactor: move ca manager into component"
```

## Task 2: Add Apply-Style File API

**Files:**
- Modify: `components/ca_manager/include/ca_manager.h`
- Modify: `components/ca_manager/ca_manager.c`

- [ ] **Step 1: Add the public function declaration**

Add to `components/ca_manager/include/ca_manager.h` after `ca_manager_update_from_stream()`:

```c
esp_err_t ca_manager_apply_file(const char *path, bool restart_on_success);
```

- [ ] **Step 2: Add a private helper to copy a verified external file into the managed temp path**

Add this helper in `components/ca_manager/ca_manager.c` after `promote_temp_bundle()`:

```c
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
```

- [ ] **Step 3: Implement `ca_manager_apply_file()`**

Add this function before `ca_manager_update_from_http_client()`:

```c
esp_err_t ca_manager_apply_file(const char *path, bool restart_on_success)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ca_manager_update_ctx_t *ctx = NULL;
    ESP_RETURN_ON_ERROR(ca_manager_update_begin(&ctx, 0), TAG, "Failed to begin file apply");

    if (fclose(ctx->file) != 0) {
        ESP_LOGE(TAG, "Failed to close temp file before apply copy: errno=%d", errno);
        ca_manager_update_abort(ctx);
        return ESP_FAIL;
    }
    ctx->file = NULL;

    esp_err_t err = copy_file(path, ctx->temp_path, &ctx->written_size);
    if (err == ESP_OK) {
        err = validate_and_promote(ctx, restart_on_success);
    }

    if (err != ESP_OK) {
        unlink(ctx->temp_path);
    }

    ctx->active = false;
    free(ctx);
    return err;
}
```

- [ ] **Step 4: Build to verify the API compiles**

Run:

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 5: Commit**

Run:

```bash
git add components/ca_manager
git commit -m "feat: add ca manager apply file API"
```

## Task 3: Update Example App And Documentation

**Files:**
- Modify: `main/main.c`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-06-05-manifest-file-updater-design.md`

- [ ] **Step 1: Keep the example app behavior unchanged**

Inspect `main/main.c` and keep this call for now:

```c
err = ca_manager_update_from_http_client(CONFIG_CA_UPDATER_BUNDLE_URL, true);
```

Expected: no code change is required in `main/main.c` for this increment. The direct HTTP helper remains only as a compatibility/example path until the manifest updater exists.

- [ ] **Step 2: Update the root README component description**

In `README.md`, update the introduction to explain the new layout:

```markdown
Projeto ESP-IDF independente para gerenciar, em tempo de execucao, um bundle publico de CAs da Mozilla armazenado em uma particao SPIFFS.

O codigo reutilizavel fica em `components/ca_manager`. O diretorio `main/` contem um app de exemplo para ESP32/ESP32-S3 que configura Wi-Fi e demonstra o fluxo de atualizacao.
```

- [ ] **Step 3: Add apply API note to README**

Add this note near the integration observations:

```markdown
- Para integracao com atualizadores baseados em manifesto, prefira `ca_manager_apply_file(path, restart_on_success)`: a camada externa baixa e verifica o artefato, e o `ca_manager` valida semanticamente o bundle antes de promove-lo.
```

- [ ] **Step 4: Mark the first implementation increment in the design spec**

Add this sentence under `## Recommended First Cut` in `docs/superpowers/specs/2026-06-05-manifest-file-updater-design.md`:

```markdown
The first implementation increment is `components/ca_manager` plus `ca_manager_apply_file()`. The generic manifest updater and terminal test mode follow in later increments.
```

- [ ] **Step 5: Build after docs/code changes**

Run:

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 6: Commit**

Run:

```bash
git add README.md docs/superpowers/specs/2026-06-05-manifest-file-updater-design.md
git commit -m "docs: describe ca manager component boundary"
```

## Self-Review Checklist

- The plan implements only the first increment of the accepted design.
- `ca_manager` becomes portable as an ESP-IDF component.
- `main/` remains an example app.
- The apply-style API exists before the manifest updater is introduced.
- Manifest parsing, periodic scheduling, terminal commands, and OTA are intentionally left for later plans.

## Follow-Up: Manifest Trust Bootstrap

The current CA updater has an unavoidable trust-bootstrap risk: the device may
need a newer CA bundle to reach the HTTPS host that serves the newer CA bundle.
Treat this as follow-up architecture work after the manifest flow is stable.

**Goal:** Improve update availability with mirrors, then make the manifest the
real trust boundary with a firmware-verified signature.

**Files likely involved:**
- Modify: `main/main.c`
- Modify: `tools/certificate_prepare/create_release.sh`
- Modify: `tools/certificate_prepare/prepare.py`
- Modify: `tools/certificate_prepare/README.md`
- Modify or create: manifest updater component files when split from `main/`

- [ ] **Step 1: Add mirrored artifact URLs to the manifest schema**

Accept either a legacy single `url` or a new `urls` array. Normalize both into
an internal URL list and reject an empty list.

Validation requirements:
- Reject empty URLs.
- Reject malformed URLs.
- Reject unsupported schemes.
- Reject URLs longer than the configured limit.
- Prefer HTTPS-only URLs until signed manifests are mandatory.

- [ ] **Step 2: Implement mirror fallback**

Try artifact URLs in manifest order. For each mirror:
- Download to the temporary artifact path.
- Verify downloaded size against manifest `size`.
- Verify SHA-256 against manifest `sha256`.
- Accept the first mirror whose file verifies.
- Log failed mirrors with enough context to diagnose host, redirect, size, or
  hash problems.

Expected: a primary mirror outage or certificate-chain failure can be bypassed
when another trusted HTTPS mirror still works.

- [ ] **Step 3: Extend release tooling for mirrors**

Allow the release script to generate a manifest with optional extra artifact
URLs. The GitHub Release asset URL can remain the default first URL.

Example shape:

```json
{
  "schema": 1,
  "artifact_type": "ca_bundle",
  "channel": "stable",
  "version": "1.0.8",
  "build_id": "2026-06-05T00:00:00Z-ca",
  "urls": [
    "https://github.com/maujabur/mozilla_ca_spiffs_updater/releases/download/v1.0.8/bundle_ca.bin",
    "https://updates.example.net/ca/stable/bundle_ca.bin"
  ],
  "sha256": "hexadecimal_sha256_do_arquivo",
  "size": 56244
}
```

- [ ] **Step 4: Add signed manifest support**

Embed a public signing key in firmware and verify the manifest signature before
trusting any field in the manifest.

Recommended fields:
- `signature_algorithm`
- `signing_key_id`
- `signature`

Signing requirements:
- Define manifest canonicalization rules before signing.
- Sign the manifest metadata, including version, URLs, size, and SHA-256.
- Reject invalid signatures before attempting artifact download.
- Keep support for at least current and next signing keys for rotation.

- [ ] **Step 5: Revisit non-HTTPS mirrors only after signing**

Do not use plain HTTP artifact mirrors while the manifest is only protected by
HTTPS plus SHA-256. If signed manifests become mandatory, an HTTP artifact
mirror may be considered for emergency recovery because the artifact is still
bound to signed metadata and verified by SHA-256.

- [ ] **Step 6: Add integration tests**

Cover:
- Primary mirror fails, secondary mirror succeeds.
- All mirrors fail.
- Mirror returns wrong size.
- Mirror returns wrong SHA-256.
- Manifest signature is valid.
- Manifest signature is invalid.
- Device has an older active CA bundle and can still update through a mirror
  with a different certificate chain.

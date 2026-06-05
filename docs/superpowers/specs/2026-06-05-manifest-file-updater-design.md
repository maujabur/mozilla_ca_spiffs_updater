# Manifest File Updater Design

## Goal

Evolve this project from a standalone Mozilla CA SPIFFS updater into a reusable ESP-IDF system with two clear layers:

- A small CA bundle manager that remains focused on storing, validating, and activating ESP-IDF CA bundles.
- A reusable manifest-based file updater that can later support OTA firmware and other online artifacts without contaminating the CA core.

The first implementation target remains CA bundle updates for ESP32/ESP32-S3. The design should leave a direct path toward a more generic manifest update framework.

## Current Context

The current project already has useful boundaries:

- `main/ca_manager.c` and `main/ca_manager.h` manage SPIFFS storage, active bundle loading, streaming writes, validation through `esp_crt_bundle_set()`, safe promotion, and fallback to the embedded ESP-IDF bundle.
- `main/main.c` is an example application that initializes NVS, connects Wi-Fi, and calls the CA update flow.
- `tools/certificate_prepare/prepare.py` generates the `bundle_ca.bin` consumed by the device.

The main issue is portability. Reusable code currently lives beside the example app in `main/`, and `ca_manager` includes an HTTP helper. A future manifest layer should not add more policy and network behavior to the CA core.

## Recommended Structure

Use ESP-IDF components for reusable code and keep `main/` as an example application.

```text
components/
  ca_manager/
    include/ca_manager.h
    ca_manager.c
    Kconfig
    CMakeLists.txt

  manifest_file_updater/
    include/manifest_file_updater.h
    manifest_file_updater.c
    manifest_http.c
    manifest_json.c
    Kconfig
    CMakeLists.txt

  ca_manifest_updater/
    include/ca_manifest_updater.h
    ca_manifest_updater.c
    Kconfig
    CMakeLists.txt

main/
  main.c
  Kconfig.projbuild
  CMakeLists.txt

tools/
  certificate_prepare/
```

This is better than subfolders inside `main/` because ESP-IDF components can be copied, vendored, or added as dependencies in other projects with less clutter. `main/` should demonstrate integration, not own reusable modules.

## Component Responsibilities

### `ca_manager`

Owns only CA bundle lifecycle:

- Mount configured storage.
- Load an existing CA bundle.
- Activate a bundle with `esp_crt_bundle_set()`.
- Write/update a candidate bundle.
- Validate before promotion.
- Promote atomically enough for SPIFFS constraints.
- Recover interrupted promotion.
- Preserve fallback behavior when no valid stored bundle exists.

It should not parse manifests, compare versions, know channels, or decide when to contact a server.

The public API can keep the existing streaming functions, but the manifest flow should prefer an apply-style entry point:

```c
esp_err_t ca_manager_apply_file(const char *path, bool restart_on_success);
```

That allows an external updater to download and verify an artifact first, then hand a stable local file to `ca_manager`.

### `manifest_file_updater`

Owns generic update mechanics:

- Download a small HTTPS manifest.
- Parse and validate known JSON fields.
- Compare manifest metadata against caller-provided current state.
- Decide whether an artifact should be downloaded.
- Download the artifact to a temporary file.
- Verify `size` and `sha256`.
- Call a domain-specific `apply()` callback with the verified local path.
- Clean up temporary files after success or failure.

It should be generic enough to support CA and OTA later, but conservative enough that the first version does not become a full framework.

Suggested initial manifest fields:

```json
{
  "schema": 1,
  "artifact_type": "ca_bundle",
  "channel": "pilot",
  "version": "2026.06.05",
  "build_id": "2026-06-05T00:00:00Z-ca",
  "url": "https://updates.example.com/ca/bundle_ca.bin",
  "sha256": "hexadecimal_sha256_do_arquivo",
  "size": 131072,
  "min_version": "",
  "critical": false,
  "notes": "Atualiza CAs Mozilla"
}
```

`artifact_type` is the key that keeps the generic updater extensible without making every domain share one meaning for `version`.

### `ca_manifest_updater`

Connects the generic manifest updater to the CA manager:

- Provides current CA bundle metadata if available.
- Configures expected `artifact_type = "ca_bundle"`.
- Supplies an `apply()` callback that calls `ca_manager_apply_file()`.
- Owns CA-specific policy that does not belong in `ca_manager`, such as whether to restart after a CA bundle update.

This layer is intentionally thin. It exists so application code can call one CA-focused function without pushing manifest concepts into the CA core.

## Apply-Style Flow

The preferred update flow is:

1. App initializes networking.
2. App calls `ca_manager_init()`.
3. App calls `ca_manifest_updater_run(manifest_url, options)`.
4. `ca_manifest_updater` builds a `manifest_file_updater` request for `artifact_type = "ca_bundle"`.
5. `manifest_file_updater` downloads the manifest.
6. `manifest_file_updater` validates schema, type, URL, size, and SHA-256.
7. `manifest_file_updater` decides whether to download.
8. `manifest_file_updater` downloads the artifact to its own temporary path.
9. `manifest_file_updater` verifies downloaded size and SHA-256.
10. `manifest_file_updater` calls `apply(downloaded_path, user_ctx)`.
11. `ca_manifest_updater` applies the file through `ca_manager_apply_file()`.
12. Temporary updater files are removed.

The important boundary is that `manifest_file_updater` guarantees the file matches the manifest before calling `apply()`. The domain-specific component remains responsible for validating the file semantically.

For CA bundles, SHA-256 and size prove transport integrity, while `esp_crt_bundle_set()` proves the file is actually usable as an ESP-IDF CA bundle.

## Initial API Sketch

```c
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
    const char *artifact_type;
    const char *channel;
    const char *version;
    const char *build_id;
    const char *url;
    const char *sha256_hex;
    size_t size;
    const char *min_version;
    bool critical;
} manifest_file_artifact_t;

typedef esp_err_t (*manifest_file_apply_fn)(const char *verified_path,
                                            const manifest_file_artifact_t *artifact,
                                            void *user_ctx);

esp_err_t manifest_file_updater_run(const char *manifest_url,
                                    const manifest_file_update_request_t *request,
                                    manifest_file_apply_fn apply,
                                    void *user_ctx);
```

This API intentionally exposes one artifact per manifest in the first version. Multi-artifact manifests can be added later without changing the `apply()` idea.

## Version Policy

Version comparison should start simple:

- If `current_version` is empty or unknown, allow update.
- If manifest `version` equals `current_version`, skip unless `allow_same_version` is true.
- If manifest `min_version` is present and the current version is below it, skip or return a policy error.

The first version should not assume every artifact uses semantic versioning. CA bundles may use date-like versions, while firmware may use semver. To avoid premature complexity, version comparison can initially be callback-based or strict string equality with optional domain policy.

## Error Handling

The generic updater should distinguish:

- No update needed.
- Manifest download failure.
- Manifest JSON invalid.
- Unsupported schema.
- Wrong artifact type.
- Version policy rejected.
- Artifact download failure.
- Size mismatch.
- SHA-256 mismatch.
- Apply callback failure.

CA-specific apply failure must preserve the previous active CA bundle, matching the current `ca_manager` behavior.

## Migration Plan

### Phase 1: Component Layout

- Move `ca_manager.c/.h` into `components/ca_manager`.
- Move CA-related Kconfig entries into `components/ca_manager/Kconfig`.
- Keep Wi-Fi and example-only config in `main/Kconfig.projbuild`.
- Keep `main/main.c` as an integration example.
- Remove or de-emphasize direct HTTP update from `ca_manager`; keep streaming/file apply APIs as the stable core.

### Phase 2: CA Apply API

- Add `ca_manager_apply_file(const char *path, bool restart_on_success)`.
- Reuse existing validation and promotion logic internally.
- Keep existing streaming API if useful for callers that already download themselves.
- Update README to describe `ca_manager` as reusable component code.

### Phase 3: Generic Manifest Updater

- Add `components/manifest_file_updater`.
- Implement manifest HTTPS download with bounded manifest size.
- Parse JSON with ESP-IDF `cJSON`.
- Download artifact to a temp file.
- Verify SHA-256 and size.
- Call the provided `apply()` callback.

### Phase 4: CA Manifest Adapter

- Add `components/ca_manifest_updater`.
- Add a simple CA manifest config: manifest URL, channel, current CA version, restart policy.
- Wire the adapter to `manifest_file_updater_run()`.
- Update `main/main.c` to demonstrate manifest-driven CA update.

### Phase 5: Tooling And Publishing

- Extend `tools/certificate_prepare` or add a sibling tool to emit a manifest for the generated `bundle_ca.bin`.
- Include `version`, `build_id`, `size`, and `sha256`.
- Document how to host both manifest and binary.

## Expansion Toward A Generic Framework

The path from option 2 to option 3 is additive:

- Add `ota_manifest_updater` that uses the same manifest updater but applies via ESP-IDF OTA.
- Add support for domain-specific version comparators.
- Add signed manifests after the HTTPS-only version is working.
- Add ETag or `If-None-Match` support for bandwidth reduction.
- Add persistent update state for last check, last applied build, and failed build suppression.
- Add multi-artifact manifests when one device needs coordinated updates.
- Add storage backends beyond SPIFFS, such as LittleFS, FATFS, NVS blobs, or OTA partitions.
- Add progress callbacks and retry/backoff policy.

The key rule is that option 3 should emerge by adding new adapters and policies around `manifest_file_updater`, not by making `ca_manager` more general.

## Open Decisions

- Whether to remove `ca_manager_update_from_http_client()` immediately or keep it as a compatibility helper for one release.
- Whether the first version comparator is string equality only or accepts a callback.
- Whether manifest signing is required in the first implementation or postponed until HTTPS plus SHA-256 is validated in practice.
- Whether `version` for CA bundles should be date-based, Mozilla certdata revision based, or generated build timestamp based.

## Recommended First Cut

Implement option 2 with component layout now:

- `components/ca_manager` as the clean CA core.
- `components/manifest_file_updater` as the reusable apply-style updater.
- `components/ca_manifest_updater` as the small adapter.
- `main/` as an ESP32-S3 example.

This keeps the current project useful immediately and makes the future option 3 a controlled expansion instead of a rewrite.

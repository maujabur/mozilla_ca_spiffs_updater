# Manifest File Updater Design

## Goal

Evolve this project from a standalone Mozilla CA SPIFFS updater into a reusable ESP-IDF system with two clear layers:

- A small CA bundle manager that remains focused on storing, validating, and activating ESP-IDF CA bundles.
- A reusable manifest-based file updater that can later support OTA firmware and other online artifacts without contaminating the CA core.

The first implementation target remains CA bundle updates for ESP32/ESP32-S3. The design should leave a direct path toward a more generic manifest update framework.

## Current Context

The project now has the intended component boundaries:

- `components/ca_manager` manages SPIFFS storage, active bundle loading, streaming writes, file apply, validation through `esp_crt_bundle_set()`, safe promotion, and fallback to the embedded ESP-IDF bundle.
- `components/manifest_file_updater` owns HTTPS manifest/artifact download, redirects, bounded reads, size checks, and SHA-256 verification.
- `components/ca_manifest_updater` adapts the generic manifest updater to CA bundles and calls `ca_manager_apply_file()` with a verified local file.
- `main/main.c` is an example application that initializes NVS, connects Wi-Fi, runs boot HTTPS diagnostics, and calls the manifest-driven CA update flow.
- `tools/certificate_prepare/prepare.py` generates the `bundle_ca.bin` consumed by the device.

The main architectural rule is that network policy stays outside the CA core.
`ca_manager` does not perform HTTP and should remain focused on local storage,
semantic CA bundle validation, activation, and promotion.

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

The first cut may keep a single `url` field because it is simple and already
matches the current release tooling. The schema should reserve a direct
migration path to `urls`, where the artifact can be attempted from more than
one HTTPS mirror.

Suggested future manifest fields for mirrored and signed CA bundle delivery:

```json
{
  "schema": 1,
  "artifact_type": "ca_bundle",
  "channel": "stable",
  "version": "1.0.8",
  "build_id": "2026-06-05T00:00:00Z-ca",
  "urls": [
    "https://maujabur.github.io/mozilla_ca_spiffs_artifacts/ca/stable/bundle_ca.bin",
    "https://raw.githubusercontent.com/maujabur/mozilla_ca_spiffs_artifacts/main/ca/stable/bundle_ca.bin"
  ],
  "sha256": "hexadecimal_sha256_do_arquivo",
  "size": 56244,
  "signature_algorithm": "ed25519",
  "signing_key_id": "ca-bundle-2026",
  "signature": "base64_assinatura_do_manifest_canonico"
}
```

`url` and `urls` should not both be required. A compatibility parser can accept
either one, normalize both into an internal URL list, and reject an empty list.
URL validation should reject empty strings, malformed URLs, unsupported schemes,
and strings longer than the configured manifest URL limit.

## Trust Bootstrap And Mirrors

CA bundle update has a bootstrapping paradox: the device may need a newer CA
bundle to reach the HTTPS server that hosts the newer CA bundle. This project
should treat that as an architecture concern, not only as an operational edge
case.

Multiple HTTPS mirrors are useful and should be added as an availability
feature. Hosting the same artifact on providers with different certificate
chains, such as GitHub Releases plus an object-storage CDN, reduces the chance
that one expired or distrusted chain blocks every update. The device should try
each mirror in order, applying the same size and SHA-256 checks to whichever
download succeeds.

Mirrors do not fully solve trust bootstrap. If the local trust store is too old
for every HTTPS mirror, the device still cannot fetch the update. The long-term
security boundary should be a signed manifest verified by a public key embedded
in firmware. With a valid manifest signature, the manifest becomes the authority
for artifact metadata, and the artifact can be accepted only if its size and
SHA-256 match the signed metadata.

The recommended policy is:

- Keep HTTPS for the manifest and artifacts whenever possible.
- Add `urls[]` mirror fallback for resilience.
- Add manifest signatures before supporting any non-HTTPS artifact mirror.
- Never trust a downloaded artifact only because it came from a mirror; always
  verify size and SHA-256 from the manifest.
- Plan signing key rotation with `signing_key_id` and firmware support for at
  least the current and next public keys.

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
6. `manifest_file_updater` validates schema, type, URL list, size, and SHA-256.
7. `manifest_file_updater` decides whether to download.
8. `manifest_file_updater` downloads the artifact to its own temporary path,
   trying mirrors in order when more than one URL is available.
9. `manifest_file_updater` verifies downloaded size and SHA-256.
10. `manifest_file_updater` calls `apply(downloaded_path, user_ctx)`.
11. `ca_manifest_updater` applies the file through `ca_manager_apply_file()`.
12. Temporary updater files are removed.

The important boundary is that `manifest_file_updater` guarantees the file matches the manifest before calling `apply()`. The domain-specific component remains responsible for validating the file semantically.

For CA bundles, SHA-256 and size prove transport integrity, while `esp_crt_bundle_set()` proves the file is actually usable as an ESP-IDF CA bundle.

After manifest signing is implemented, signature verification must happen before
the updater trusts any manifest metadata, including `url`, `urls`, `size`,
`sha256`, `version`, and `critical`.

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
    const char *const *urls;
    size_t url_count;
    const char *sha256_hex;
    size_t size;
    const char *min_version;
    bool critical;
    const char *signature_algorithm;
    const char *signing_key_id;
    const char *signature_b64;
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
- Manifest signature invalid.
- Unsupported schema.
- Wrong artifact type.
- Version policy rejected.
- Invalid or empty artifact URL list.
- Artifact download failure on one mirror.
- Artifact download failure on all mirrors.
- Size mismatch.
- SHA-256 mismatch.
- Apply callback failure.

CA-specific apply failure must preserve the previous active CA bundle, matching the current `ca_manager` behavior.

## ESP32 Test Strategy

Testing on real ESP32/ESP32-S3 hardware should be planned as an explicit integration mode, not only as boot-time behavior. The example app should expose a small terminal-driven control surface so a developer can exercise manifest fetch, artifact download, apply, and failure cases without reflashing for each scenario.

Recommended test interface:

- A console command to fetch and print manifest metadata from a configured or typed URL.
- A console command to run the full manifest update flow.
- A console command to download and verify an artifact without applying it.
- A console command to apply a previously downloaded local file.
- A console command to print current CA bundle state, stored metadata, and last update result.

This can be implemented with the ESP-IDF console component later, or with a simpler UART command loop if console setup feels too heavy for the first cut. The important design point is that update operations should be callable independently from boot.

Useful HTTPS test cases:

- Valid manifest and valid CA bundle.
- Valid manifest pointing to an unchanged version.
- Valid manifest with wrong `artifact_type`.
- Valid manifest with wrong `sha256`.
- Valid manifest with wrong `size`.
- Valid manifest where primary artifact mirror fails and secondary succeeds.
- Valid manifest where all artifact mirrors fail.
- Valid signed manifest.
- Manifest with invalid signature.
- Manifest URL returning 404.
- Artifact URL returning 404.
- HTTPS endpoint with an expired or untrusted certificate.
- Oversized manifest.
- Oversized artifact.
- Corrupted CA bundle with correct transport hash, proving that `ca_manager` semantic validation still rejects it.

The device-side logs should make these cases distinguishable. A test run should be able to answer whether the failure happened during manifest fetch, manifest validation, artifact download, hash/size verification, or domain apply.

## Periodic Manifest Checks

Periodic checks should be a scheduling concern above the manifest updater, not part of the CA manager.

Recommended first design:

- `manifest_file_updater_run()` performs one update attempt and returns a precise result.
- `ca_manifest_updater` may expose a convenience function for one CA update attempt.
- The example app owns periodic scheduling with a FreeRTOS task or timer.
- Backoff, jitter, and minimum interval are configured in the scheduling layer.

This keeps the reusable updater deterministic and easy to test. It also lets different products choose different policies: check once at boot, check once after Wi-Fi connects, check every 24 hours, check only after SNTP time sync, or check when commanded from a console.

Suggested periodic policy for the example app:

- Run one check after Wi-Fi is connected and system time is valid.
- If no update is needed, wait a long interval such as 24 hours.
- If a transient network error occurs, retry with bounded exponential backoff.
- If manifest or artifact validation fails, do not retry aggressively; wait for the normal interval or manual command.
- If apply succeeds and restart is enabled, restart immediately.
- If apply succeeds and restart is disabled, record success and continue.

The updater should eventually persist last-check and last-result metadata, but this can be postponed until the manifest flow is working manually.

## Accepted Architecture Decisions

These decisions are accepted for the first implementation increment:

- Use `components/` for reusable code and keep `main/` as the example app.
- Use an apply-style callback as the primary manifest updater contract.
- Keep `ca_manager` free of manifest, version, channel, and scheduling policy.
- Keep example-app diagnostics in `main/`, currently as boot HTTPS probes.
- Keep periodic scheduling outside `manifest_file_updater`.
- Remove HTTP helpers from `ca_manager`; external flows download and verify
  files before calling `ca_manager_apply_file()`.

Not every remaining open decision must be closed before implementation starts. Policy details can stay configurable or be deferred.

Can be decided during implementation:

- Whether the first version comparator is string equality only or callback-based.
- Exact ESP-IDF error code mapping for each updater result.

Can be postponed:

- Multi-artifact manifests.
- Persistent last-check metadata.
- ETag or `If-None-Match`.
- OTA adapter.
- Extra storage backends.

## Migration Plan

### Phase 1: Component Layout

- Move `ca_manager.c/.h` into `components/ca_manager`.
- Move CA-related Kconfig entries into `components/ca_manager/Kconfig`.
- Keep Wi-Fi and example-only config in `main/Kconfig.projbuild`.
- Keep `main/main.c` as an integration example.
- Remove direct HTTP update from `ca_manager`; keep streaming/file apply APIs as the stable core.

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

### Phase 3.5: Mirror Fallback

- Extend manifest parsing to accept either `url` or `urls`.
- Normalize artifact locations into an internal URL list.
- Reject empty, malformed, non-HTTPS, or overlong URLs.
- Try mirrors in manifest order until one download verifies successfully.
- Log which mirror failed and which mirror, if any, produced the accepted file.

### Phase 4: CA Manifest Adapter

- Add `components/ca_manifest_updater`.
- Add a simple CA manifest config: manifest URL, channel, current CA version, restart policy.
- Wire the adapter to `manifest_file_updater_run()`.
- Update `main/main.c` to demonstrate manifest-driven CA update.

### Phase 5: ESP32 Integration Diagnostics

- Keep boot HTTPS diagnostics in the example app for proving that the active CA bundle works against real endpoints.
- Document a set of local or hosted HTTPS URLs for success and failure scenarios.
- Keep boot-time automatic update and diagnostics configured by existing example-app Kconfig strings.

### Phase 6: Periodic Scheduling

- Add an example FreeRTOS task or timer that runs one-shot CA manifest checks.
- Gate checks on Wi-Fi readiness and, if needed, valid system time.
- Add bounded retry/backoff for transient failures.
- Keep scheduling policy out of `manifest_file_updater`.

### Phase 7: Tooling And Publishing

- Extend `tools/certificate_prepare` or add a sibling tool to emit a manifest for the generated `bundle_ca.bin`.
- Include `version`, `build_id`, `size`, and `sha256`.
- Document how to host both manifest and binary in a separate artifact repository.
- Add release-script support for optional mirror URLs.

### Phase 8: Signed Manifest Trust Boundary

- Add firmware-embedded public signing key configuration.
- Add manifest canonicalization rules for exactly what bytes are signed.
- Add manifest signature verification before using artifact metadata.
- Add `signature_algorithm`, `signing_key_id`, and `signature` fields.
- Extend release tooling to sign `bundle_ca.manifest.json`.
- Keep support for HTTPS mirrors; only consider non-HTTPS artifact URLs after
  signed manifests are mandatory.

## Roadmap Toward A Generic Framework

The current `manifest_file_updater` is already generic at the most important
boundary: it downloads and verifies a file, then hands a verified local path to a
domain-specific `apply()` callback. The roadmap below keeps that boundary intact
and grows the system around adapters and policy modules, not by making
`ca_manager` more general.

### R0: Current Baseline

Status: implemented.

- `ca_manager` owns only CA bundle storage, validation, activation, and promotion.
- `manifest_file_updater` owns HTTPS manifest download, artifact download,
  redirects, size validation, SHA-256 validation, and temp-file cleanup.
- `ca_manifest_updater` adapts the generic updater to `ca_manager_apply_file()`.
- `main/` remains an ESP-IDF example app with Wi-Fi, boot diagnostics, and the
  CA update flow.

### R1: Harden The Existing Single-Artifact Flow

Goal: make the current CA updater boringly reliable before broadening the
framework.

- Exercise hardware cases for current version, new version, SHA mismatch,
  invalid bundle, interrupted download, unavailable Wi-Fi, unavailable manifest,
  and SPIFFS-full behavior.
- Tighten error mapping so callers can distinguish no-update, network failure,
  manifest validation failure, artifact validation failure, and apply failure.
- Add small comments to public headers where ownership, temp-file lifetime, and
  callback guarantees matter.
- Keep diagnostics in `main/`; do not move debug policy into reusable components.

### R2: Mirror Fallback

Goal: improve availability without changing the trust model.

- Extend the manifest schema to accept either `url` or `urls`.
- Normalize artifact locations into an internal URL list.
- Reject empty, malformed, non-HTTPS, or overlong URLs.
- Try mirrors in manifest order until one artifact downloads and verifies.
- Log each failed mirror and the accepted mirror.
- Extend release tooling to publish optional mirror URLs.

### R3: Version And Result Policy Hooks

Goal: keep the updater generic for domains where string equality is not enough.

- Add an optional version comparator callback to replace fixed string equality.
- Preserve simple string equality as the default.
- Add optional caller-facing result details for skipped, downloaded, applied,
  corrected, failed-before-apply, and failed-during-apply cases.
- Keep scheduling and retry policy outside `manifest_file_updater`.

### R4: Signed Manifests

Goal: move trust from HTTPS-only transport metadata to firmware-verifiable
metadata.

- Add firmware-embedded public signing key configuration.
- Define canonicalization rules for exactly which manifest bytes are signed.
- Add `signature_algorithm`, `signing_key_id`, and `signature` fields.
- Verify the signature before using artifact metadata.
- Extend release tooling to sign `bundle_ca.manifest.json`.
- Keep non-HTTPS artifact URLs forbidden until signed manifests are mandatory
  and tested.

### R5: OTA Adapter

Goal: prove that `manifest_file_updater` is reusable beyond CA bundles.

- Add `ota_manifest_updater` as a separate adapter component.
- Reuse manifest fetch, artifact download, size checks, SHA-256 verification,
  and the `apply()` callback.
- Apply the verified file through ESP-IDF OTA APIs.
- Keep OTA partition selection, reboot policy, and rollback policy in the OTA
  adapter, not in `manifest_file_updater`.

### R6: Operational Efficiency

Goal: reduce bandwidth and make unattended operation cleaner.

- Add ETag or `If-None-Match` support.
- Add persistent update state for last check, last applied build, and failed
  build suppression.
- Add progress callbacks for UI/logging.
- Add example scheduling with bounded retry/backoff, gated on Wi-Fi readiness
  and valid system time.
- Keep periodic scheduling as example-app or adapter policy.

### R7: Multi-Artifact And Storage Backends

Goal: support richer products only when a real use case appears.

- Add multi-artifact manifests when one device needs coordinated updates.
- Add storage backends beyond SPIFFS, such as LittleFS, FATFS, NVS blobs, or OTA
  partitions.
- Keep the single-artifact API stable for simple devices.

## Open Decisions

- Whether the first version comparator is string equality only or accepts a callback.
- Whether manifest signing is required before public use, or postponed until HTTPS plus SHA-256 and mirror fallback are validated in practice.
- Whether non-HTTPS artifact mirrors are ever allowed, and if so only after signed manifests are mandatory.
- How signing keys are generated, stored, rotated, and revoked.
- Whether `version` for CA bundles should be date-based, Mozilla certdata revision based, or generated build timestamp based.

## Recommended First Cut

Implement option 2 with component layout now:

The first implementation increment is `components/ca_manager` plus `ca_manager_apply_file()`. The generic manifest updater and terminal test mode follow in later increments.

- `components/ca_manager` as the clean CA core.
- `components/manifest_file_updater` as the reusable apply-style updater.
- `components/ca_manifest_updater` as the small adapter.
- `main/` as an ESP32-S3 example.

This keeps the current project useful immediately and makes the future option 3 a controlled expansion instead of a rewrite.

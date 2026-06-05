# Certificate Prepare Design

## Goal

Build a Python tool in `tools/certificate_prepare` that prepares the `bundle_ca.bin` file consumed by the ESP-IDF updater.

## Scope

The tool supports two input modes:

- Default mode downloads Mozilla `certdata.txt`.
- Override mode reads a local `certdata.txt` passed by `--certdata`.

The output is an ESP-IDF `x509_crt_bundle` binary named `bundle_ca.bin` by default. It is not a concatenated PEM file.

## Architecture

The Python CLI owns orchestration: input resolution, Mozilla certdata conversion, invoking the official ESP-IDF bundle generator, output placement, and size validation.

The ESP-IDF binary format remains delegated to `${IDF_PATH}/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py`. This keeps the generated file aligned with ESP-IDF 6.2 behavior and avoids duplicating the sorted bundle format.

## Data Flow

1. Resolve `certdata.txt` from `--certdata` or download URL.
2. Convert trusted server-auth CA records from NSS `certdata.txt` into a concatenated PEM.
3. Run `gen_crt_bundle.py --input <pem> --max-certs <n>`.
4. Move the generated `x509_crt_bundle` file to the requested output path.
5. Validate that the output exists, is non-empty, and is not larger than `--max-size`.

## CLI

Default:

```bash
python tools/certificate_prepare/prepare.py --output dist/bundle_ca.bin
```

Local override:

```bash
python tools/certificate_prepare/prepare.py --certdata path/to/certdata.txt --output dist/bundle_ca.bin
```

Useful options:

- `--work-dir`: preserve intermediates for inspection.
- `--certdata-url`: override the Mozilla URL.
- `--gen-crt-bundle`: override the ESP-IDF generator path.
- `--max-size`: default `262144`, matching `CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE`.
- `--max-certs`: default `200`, matching ESP-IDF bundle generator default.

## Error Handling

The tool exits non-zero when the input file is missing, download fails, no trusted certificates are found, the ESP-IDF generator is unavailable, the generator fails, or the output exceeds the configured size.

## Testing

Unit tests cover conversion from a minimal NSS `certdata.txt` fixture, local override behavior, command construction for the ESP-IDF generator, and output size validation. Network download is kept out of unit tests.

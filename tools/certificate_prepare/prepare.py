#!/usr/bin/env python3
import argparse
import base64
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path


DEFAULT_CERTDATA_URL = (
    "https://hg.mozilla.org/mozilla-central/raw-file/default/"
    "security/nss/lib/ckfw/builtins/certdata.txt"
)
DEFAULT_MAX_SIZE = 262144
DEFAULT_MAX_CERTS = 200
GENERATED_BUNDLE_NAME = "x509_crt_bundle"
PEM_NAME = "mozilla_ca.pem"


class PrepareError(RuntimeError):
    pass


def _parse_octal_bytes(lines, start_index):
    data = bytearray()
    index = start_index

    while index < len(lines):
        line = lines[index].strip()
        index += 1
        if line == "END":
            return bytes(data), index

        cursor = 0
        while cursor < len(line):
            if line[cursor] == "\\":
                data.append(int(line[cursor + 1:cursor + 4], 8))
                cursor += 4
            else:
                cursor += 1

    raise PrepareError("Unterminated CKA_VALUE MULTILINE_OCTAL block")


def _parse_label(line):
    marker = 'UTF8 "'
    start = line.find(marker)
    if start < 0:
        return None
    start += len(marker)
    end = line.find('"', start)
    if end < 0:
        return None
    return line[start:end]


def _split_certdata_objects(certdata_text):
    objects = []
    current = []

    for line in certdata_text.splitlines():
        if line.startswith("CKA_CLASS ") and current:
            objects.append(current)
            current = [line]
        elif current or line.startswith("CKA_CLASS "):
            current.append(line)

    if current:
        objects.append(current)

    return objects


def _parse_cert_object(lines):
    label = None
    value = None
    index = 0

    while index < len(lines):
        line = lines[index].strip()
        if line.startswith("CKA_LABEL "):
            label = _parse_label(line)
        elif line == "CKA_VALUE MULTILINE_OCTAL":
            value, index = _parse_octal_bytes(lines, index + 1)
            continue
        index += 1

    if label is None or value is None:
        return None
    return label, value


def _parse_trust_object(lines):
    label = None
    trusted_server_auth = False

    for raw_line in lines:
        line = raw_line.strip()
        if line.startswith("CKA_LABEL "):
            label = _parse_label(line)
        elif line.startswith("CKA_TRUST_SERVER_AUTH "):
            trusted_server_auth = "CKT_NSS_TRUSTED_DELEGATOR" in line

    if label is None:
        return None
    return label, trusted_server_auth


def convert_certdata_to_pem(certdata_text):
    certificates = {}
    trusted_labels = set()

    for object_lines in _split_certdata_objects(certdata_text):
        header = object_lines[0] if object_lines else ""
        if "CKO_CERTIFICATE" in header:
            parsed = _parse_cert_object(object_lines)
            if parsed is not None:
                label, der_bytes = parsed
                certificates[label] = der_bytes
        elif "CKO_NSS_TRUST" in header:
            parsed = _parse_trust_object(object_lines)
            if parsed is not None:
                label, trusted = parsed
                if trusted:
                    trusted_labels.add(label)

    pem_blocks = []
    for label in sorted(trusted_labels):
        der_bytes = certificates.get(label)
        if der_bytes is None:
            continue

        body = base64.encodebytes(der_bytes).decode("ascii").strip()
        pem_blocks.append(
            f"{label}\n"
            "-----BEGIN CERTIFICATE-----\n"
            f"{body}\n"
            "-----END CERTIFICATE-----\n"
        )

    if not pem_blocks:
        raise PrepareError("No trusted server-auth CA certificates found in certdata")

    return "\n".join(pem_blocks)


def resolve_gen_crt_bundle(explicit_path=None):
    if explicit_path:
        path = Path(explicit_path)
    else:
        idf_path = Path(os.environ["IDF_PATH"]) if "IDF_PATH" in os.environ else Path(sys.prefix)
        path = idf_path / "components" / "mbedtls" / "esp_crt_bundle" / "gen_crt_bundle.py"

    if not path.is_file():
        raise PrepareError(f"ESP-IDF gen_crt_bundle.py not found: {path}")
    return path


def run_gen_crt_bundle(gen_script, pem_path, work_dir, max_certs):
    command = [
        sys.executable,
        str(gen_script),
        "--quiet",
        "--input",
        str(pem_path),
        "--max-certs",
        str(max_certs),
    ]
    subprocess.run(command, cwd=work_dir, check=True)

    generated = Path(work_dir) / GENERATED_BUNDLE_NAME
    if not generated.is_file():
        raise PrepareError(f"ESP-IDF generator did not produce {generated}")
    return generated


def validate_output(path, max_size):
    path = Path(path)
    if not path.is_file():
        raise PrepareError(f"Output file does not exist: {path}")

    size = path.stat().st_size
    if size == 0:
        raise PrepareError(f"Output file is empty: {path}")
    if size > max_size:
        raise PrepareError(f"Output file {path} exceeds max size {max_size}: {size}")


def _download_certdata(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        return response.read().decode("utf-8")


def _read_certdata(path):
    return Path(path).read_text(encoding="utf-8")


def _build_parser():
    parser = argparse.ArgumentParser(description="Prepare ESP-IDF Mozilla CA bundle for SPIFFS updater")
    parser.add_argument("--certdata", help="Local Mozilla certdata.txt override")
    parser.add_argument("--certdata-url", default=DEFAULT_CERTDATA_URL, help="Mozilla certdata.txt URL")
    parser.add_argument("--output", default="bundle_ca.bin", help="Output bundle path")
    parser.add_argument("--work-dir", help="Directory for intermediate files")
    parser.add_argument("--gen-crt-bundle", help="Path to ESP-IDF gen_crt_bundle.py")
    parser.add_argument("--max-size", type=int, default=DEFAULT_MAX_SIZE, help="Maximum output size in bytes")
    parser.add_argument("--max-certs", type=int, default=DEFAULT_MAX_CERTS, help="Maximum certificate count")
    return parser


def _run(args):
    gen_script = resolve_gen_crt_bundle(args.gen_crt_bundle)
    output = Path(args.output)

    if args.work_dir:
        work_dir = Path(args.work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)
        cleanup = None
    else:
        cleanup = tempfile.TemporaryDirectory(prefix="certificate_prepare_")
        work_dir = Path(cleanup.name)

    try:
        certdata_text = _read_certdata(args.certdata) if args.certdata else _download_certdata(args.certdata_url)
        pem_text = convert_certdata_to_pem(certdata_text)
        pem_path = work_dir / PEM_NAME
        pem_path.write_text(pem_text, encoding="utf-8")

        generated = run_gen_crt_bundle(gen_script, pem_path, work_dir, args.max_certs)
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(generated), output)
        validate_output(output, args.max_size)

        print(f"Generated {output} ({output.stat().st_size} bytes)")
        print(f"PEM intermediate: {pem_path}")
        return 0
    finally:
        if cleanup is not None:
            cleanup.cleanup()


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)

    try:
        return _run(args)
    except subprocess.CalledProcessError as exc:
        print(f"ESP-IDF bundle generator failed with exit code {exc.returncode}", file=sys.stderr)
        return 1
    except PrepareError as exc:
        print(f"certificate_prepare: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

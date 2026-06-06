#!/usr/bin/env python3
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urljoin


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_BUNDLE = SCRIPT_DIR / "bundle_ca.bin"
DEFAULT_OUT_DIR = SCRIPT_DIR / "dist"


class PackageError(RuntimeError):
    pass


def _normalize_base_url(url):
    if not url:
        raise PackageError("--base-url is required")
    if not url.startswith("https://"):
        raise PackageError("--base-url must use https")
    return url.rstrip("/") + "/"


def _sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _copy_bundle(source, dest):
    source = Path(source)
    if not source.is_file():
        raise PackageError(f"bundle file not found: {source}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() != dest.resolve():
        shutil.copy2(source, dest)


def _run_prepare(output, extra_args):
    command = [
        sys.executable,
        str(SCRIPT_DIR / "prepare.py"),
        "--output",
        str(output),
    ]
    command.extend(extra_args)
    subprocess.run(command, check=True)


def _write_text(path, text):
    path.write_text(text + "\n", encoding="utf-8")


def _write_manifest(path, artifact):
    path.write_text(json.dumps(artifact, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def _find_git_root(path):
    path = Path(path).resolve()
    for candidate in [path, *path.parents]:
        if (candidate / ".git").exists():
            return candidate
    return None


def _print_publish_hint(out_dir):
    git_root = _find_git_root(out_dir)
    print("")
    if git_root is None:
        print("Warning: output directory is not inside a Git repository.")
        print("Files were generated locally only. To publish them, write --out-dir")
        print("inside a clone of the artifact repository, then commit and push.")
        return

    rel_out = Path(out_dir).resolve().relative_to(git_root)
    print("Publish with:")
    print(f"  cd {git_root}")
    print(f"  git add {rel_out}/bundle_ca.bin {rel_out}/bundle_ca.manifest.json \\")
    print(f"          {rel_out}/bundle_ca.version {rel_out}/bundle_ca.sha256")
    print("  git commit -m \"Publish CA bundle <version>\"")
    print("  git push origin main")


def _build_parser():
    parser = argparse.ArgumentParser(
        description="Build CA bundle artifact files for publication in an artifact repository"
    )
    parser.add_argument("version", help="CA bundle version, for example 1.0.9")
    parser.add_argument("--channel", default="stable", help="Manifest channel")
    parser.add_argument("--base-url", required=True, help="HTTPS base URL where generated files will be hosted")
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR, help="Directory to write artifact files")
    parser.add_argument("--bundle", default=DEFAULT_BUNDLE, help="Existing bundle_ca.bin to package")
    parser.add_argument("--prepare", action="store_true", help="Generate bundle_ca.bin before packaging")
    parser.add_argument(
        "--prepare-arg",
        action="append",
        default=[],
        help="Extra argument forwarded to prepare.py. Repeat for multiple arguments.",
    )
    parser.add_argument(
        "--extra-artifact-url",
        action="append",
        default=[],
        help="Additional HTTPS artifact URL for future mirror fallback. Repeat for multiple mirrors.",
    )
    parser.add_argument("--notes", help="Optional manifest notes")
    return parser


def _run(args):
    base_url = _normalize_base_url(args.base_url)
    out_dir = Path(args.out_dir)
    bundle_path = out_dir / "bundle_ca.bin"
    version_path = out_dir / "bundle_ca.version"
    sha256_path = out_dir / "bundle_ca.sha256"
    manifest_path = out_dir / "bundle_ca.manifest.json"

    if args.prepare:
        _run_prepare(bundle_path, args.prepare_arg)
    else:
        _copy_bundle(args.bundle, bundle_path)

    sha256 = _sha256_file(bundle_path)
    size = bundle_path.stat().st_size
    primary_url = urljoin(base_url, "bundle_ca.bin")
    artifact_urls = [primary_url]
    for url in args.extra_artifact_url:
        if not url.startswith("https://"):
            raise PackageError(f"extra artifact URL must use https: {url}")
        if url not in artifact_urls:
            artifact_urls.append(url)

    build_id = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    notes = args.notes if args.notes is not None else f"CA bundle {args.version}"
    manifest = {
        "schema": 1,
        "artifact_type": "ca_bundle",
        "channel": args.channel,
        "version": args.version,
        "build_id": build_id,
        "url": primary_url,
        "sha256": sha256,
        "size": size,
        "notes": notes,
    }
    if len(artifact_urls) > 1:
        manifest["urls"] = artifact_urls

    _write_text(version_path, args.version)
    _write_text(sha256_path, sha256)
    _write_manifest(manifest_path, manifest)

    print(f"Wrote {bundle_path} ({size} bytes)")
    print(f"Wrote {version_path}")
    print(f"Wrote {sha256_path}")
    print(f"Wrote {manifest_path}")
    print(f"Manifest URL: {urljoin(base_url, 'bundle_ca.manifest.json')}")
    print(f"Artifact URL: {primary_url}")
    if len(artifact_urls) > 1:
        print("Additional artifact URLs:")
        for url in artifact_urls[1:]:
            print(f"  {url}")
    _print_publish_hint(out_dir)
    return 0


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        return _run(args)
    except subprocess.CalledProcessError as exc:
        print(f"prepare.py failed with exit code {exc.returncode}", file=sys.stderr)
        return 1
    except PackageError as exc:
        print(f"package_artifacts: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

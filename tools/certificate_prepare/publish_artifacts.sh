#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORKSPACE_DIR="$(cd "${PROJECT_DIR}/.." && pwd)"

ARTIFACT_REPO_URL="${ARTIFACT_REPO_URL:-https://github.com/maujabur/mozilla_ca_spiffs_artifacts.git}"
ARTIFACT_REPO_DIR="${ARTIFACT_REPO_DIR:-${WORKSPACE_DIR}/mozilla_ca_spiffs_artifacts}"
ARTIFACT_BASE_URL="${ARTIFACT_BASE_URL:-https://maujabur.github.io/mozilla_ca_spiffs_artifacts/ca/stable}"
ARTIFACT_EXTRA_URL="${ARTIFACT_EXTRA_URL:-https://raw.githubusercontent.com/maujabur/mozilla_ca_spiffs_artifacts/main/ca/stable/bundle_ca.bin}"
ARTIFACT_BRANCH="${ARTIFACT_BRANCH:-main}"

usage() {
  cat <<EOF
Usage: $0 <version>

Publishes CA bundle artifacts to the artifact repository.

Environment overrides:
  ARTIFACT_REPO_URL   ${ARTIFACT_REPO_URL}
  ARTIFACT_REPO_DIR   ${ARTIFACT_REPO_DIR}
  ARTIFACT_BASE_URL   ${ARTIFACT_BASE_URL}
  ARTIFACT_EXTRA_URL  ${ARTIFACT_EXTRA_URL}
  ARTIFACT_BRANCH     ${ARTIFACT_BRANCH}
EOF
}

if [[ $# -ne 1 ]]; then
  usage
  exit 1
fi

VERSION="$1"
if [[ ! "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Error: version must look like 1.0.3" >&2
  exit 1
fi

ensure_artifact_repo() {
  if [[ -e "${ARTIFACT_REPO_DIR}" && ! -d "${ARTIFACT_REPO_DIR}/.git" ]]; then
    local backup="${ARTIFACT_REPO_DIR}.generated.$(date -u +%Y%m%dT%H%M%SZ)"
    echo "Found non-Git artifact directory: ${ARTIFACT_REPO_DIR}"
    echo "Moving it aside to: ${backup}"
    mv "${ARTIFACT_REPO_DIR}" "${backup}"
  fi

  if [[ ! -d "${ARTIFACT_REPO_DIR}/.git" ]]; then
    echo "Cloning artifact repository into ${ARTIFACT_REPO_DIR}"
    git clone "${ARTIFACT_REPO_URL}" "${ARTIFACT_REPO_DIR}"
  fi

  git -C "${ARTIFACT_REPO_DIR}" fetch origin "${ARTIFACT_BRANCH}"
  git -C "${ARTIFACT_REPO_DIR}" checkout "${ARTIFACT_BRANCH}"
  git -C "${ARTIFACT_REPO_DIR}" pull --ff-only origin "${ARTIFACT_BRANCH}"

  if [[ -n "$(git -C "${ARTIFACT_REPO_DIR}" status --porcelain)" ]]; then
    echo "Error: artifact repository has local changes:" >&2
    git -C "${ARTIFACT_REPO_DIR}" status --short >&2
    echo "Commit, push, or clean them before publishing a new bundle." >&2
    exit 1
  fi
}

ensure_git_identity() {
  if ! git -C "${ARTIFACT_REPO_DIR}" config user.name >/dev/null; then
    local login=""
    if command -v gh >/dev/null 2>&1; then
      login="$(gh api user --jq .login 2>/dev/null || true)"
    fi
    if [[ -z "${login}" ]]; then
      login="maujabur"
    fi
    git -C "${ARTIFACT_REPO_DIR}" config user.name "${login}"
    git -C "${ARTIFACT_REPO_DIR}" config user.email "${login}@users.noreply.github.com"
  fi
}

publish_artifacts() {
  local out_dir="${ARTIFACT_REPO_DIR}/ca/stable"

  python3 "${SCRIPT_DIR}/package_artifacts.py" "${VERSION}" \
    --prepare \
    --out-dir "${out_dir}" \
    --base-url "${ARTIFACT_BASE_URL}" \
    --extra-artifact-url "${ARTIFACT_EXTRA_URL}" \
    --notes "CA bundle ${VERSION}"

  git -C "${ARTIFACT_REPO_DIR}" add \
    ca/stable/bundle_ca.bin \
    ca/stable/bundle_ca.manifest.json \
    ca/stable/bundle_ca.version \
    ca/stable/bundle_ca.sha256

  if git -C "${ARTIFACT_REPO_DIR}" diff --cached --quiet; then
    echo "No artifact changes to publish."
    return 0
  fi

  git -C "${ARTIFACT_REPO_DIR}" commit -m "Publish CA bundle ${VERSION}"
  git -C "${ARTIFACT_REPO_DIR}" push origin "${ARTIFACT_BRANCH}"
}

ensure_artifact_repo
ensure_git_identity
publish_artifacts

echo
echo "Published CA bundle ${VERSION}"
echo "Manifest URL: ${ARTIFACT_BASE_URL}/bundle_ca.manifest.json"
echo "GitHub Pages may cache the previous manifest for a few minutes."

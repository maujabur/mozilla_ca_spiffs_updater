#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 1 ]]; then
  echo "Uso: $0 <versao>"
  echo "Exemplo: $0 1.0.1"
  exit 1
fi

VERSION="$1"

if [[ ! "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Erro: versao deve estar no formato 1.0.1, sem o prefixo v."
  exit 1
fi

TAG="v${VERSION}"
BIN_FILE="${SCRIPT_DIR}/bundle_ca.bin"
VERSION_FILE="${SCRIPT_DIR}/bundle_ca.version"
SHA256_FILE="${SCRIPT_DIR}/bundle_ca.sha256"
TITLE="Mozilla CA bundle ${TAG}"
NOTES="Release ${TAG}"

if ! command -v gh >/dev/null 2>&1; then
  echo "Erro: GitHub CLI (gh) nao encontrado."
  echo "Instale o gh e autentique com: gh auth login"
  exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
  echo "Erro: GitHub CLI nao esta autenticado."
  echo "Execute: gh auth login"
  exit 1
fi

if [[ ! -f "${BIN_FILE}" ]]; then
  echo "Erro: arquivo .bin nao encontrado: ${BIN_FILE}"
  exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
  SHA256="$(sha256sum "${BIN_FILE}" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
  SHA256="$(shasum -a 256 "${BIN_FILE}" | awk '{print $1}')"
else
  echo "Erro: sha256sum ou shasum nao encontrado."
  exit 1
fi

printf "%s\n" "${VERSION}" > "${VERSION_FILE}"
printf "%s\n" "${SHA256}" > "${SHA256_FILE}"

echo "Criando release ${TAG}"
echo "Arquivo: ${BIN_FILE}"
echo "Versao: ${VERSION_FILE}"
echo "SHA-256: ${SHA256_FILE}"
echo "Titulo: ${TITLE}"
echo "Notas: ${NOTES}"

gh release create "${TAG}" "${BIN_FILE}" "${VERSION_FILE}" "${SHA256_FILE}" --title "${TITLE}" --notes "${NOTES}"

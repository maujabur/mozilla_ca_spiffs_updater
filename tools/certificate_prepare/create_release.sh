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
TITLE="Firmware ${TAG}"
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

echo "Criando release ${TAG}"
echo "Arquivo: ${BIN_FILE}"
echo "Titulo: ${TITLE}"
echo "Notas: ${NOTES}"

gh release create "${TAG}" "${BIN_FILE}" --title "${TITLE}" --notes "${NOTES}"

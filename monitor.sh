#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PORT="${ESPPORT:-/dev/ttyACM0}"

if [[ $# -gt 0 ]]; then
  case "$1" in
    -h|--help)
      echo "Uso: $0 [porta] [opcoes extras do monitor]"
      echo "Exemplo: $0"
      echo "Exemplo: $0 /dev/ttyACM1"
      echo "Exemplo: ESPPORT=/dev/ttyACM1 $0"
      exit 0
      ;;
    /dev/*)
      PORT="$1"
      shift
      ;;
  esac
fi

if ! command -v idf.py >/dev/null 2>&1; then
  echo "Erro: idf.py nao encontrado no PATH."
  echo "Abra o Dev Container do ESP-IDF ou carregue o ambiente antes de rodar."
  exit 1
fi

if [[ ! -e "${PORT}" ]]; then
  echo "Erro: porta serial nao encontrada: ${PORT}"
  echo "Conecte a placa ou informe outra porta, por exemplo: $0 /dev/ttyACM1"
  exit 1
fi

exec idf.py -p "${PORT}" monitor --no-reset "$@"

#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT="${PORT:-/dev/ttyACM0}"
MAC=""

if [ -n "${1:-}" ]; then
    if [[ "$1" =~ ^/dev/ ]]; then
        PORT="$1"
    else
        MAC="$(printf '%s' "$1" | tr '[:upper:]:-' '[:lower:]..')"
        if [ -n "${2:-}" ]; then
            PORT="$2"
        fi
    fi
fi

if [ -z "$MAC" ]; then
    echo "Reading device MAC from $PORT..."
    MAC=$(esptool.py --port "$PORT" read_mac 2>/dev/null | awk -F': ' '/^BASE MAC:/ {m=$2} /^MAC:/ && !m {m=$2} END {gsub(/:/, ".", m); print tolower(m)}')
    if [ -z "$MAC" ]; then
        echo "Error: Failed to read MAC from $PORT. Is the device plugged in?" >&2
        exit 1
    fi
    echo "Detected device MAC: $MAC"
fi

CSV_FILE="device-secrets/${MAC}.device_secrets.csv"
BIN_FILE="nvs-partitions/${MAC}.device_nvs.bin"

if [ ! -f "$CSV_FILE" ]; then
    echo "Error: Secrets file '$CSV_FILE' not found for device $MAC." >&2
    echo "Create it using device-secrets/aa.bb.cc.dd.ee.ff.device_secrets.csv.example as a template." >&2
    exit 1
fi

mkdir -p nvs-partitions

echo "Generating NVS binary from $CSV_FILE..."
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate "$CSV_FILE" "$BIN_FILE" 0x6000

echo "Flashing $BIN_FILE to NVS partition via $PORT..."
parttool.py --partition-table-offset 0x10000 --port "$PORT" write_partition --partition-name nvs --input "$BIN_FILE"

echo "Successfully flashed NVS for $MAC"

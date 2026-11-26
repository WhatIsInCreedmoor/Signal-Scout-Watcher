#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT="${1:-${PORT:-/dev/ttyACM0}}"

if [[ "$PORT" == "-h" || "$PORT" == "--help" ]]; then
    echo "Usage: $0 [serial_port]"
    echo "Reads the connected ESP32 chip type and MAC address, then generates:"
    echo "  keys/<mac>.flash-encryption.bin  (skipped if chip already has flash encryption burned)"
    echo "  keys/<mac>.nvs_keys.bin"
    echo "Default port: /dev/ttyACM0 (or \$PORT)"
    exit 0
fi

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "Error: IDF_PATH is not set. Source your ESP-IDF export.sh first." >&2
    exit 1
fi

for cmd in esptool.py espsecure.py espefuse.py python; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Error: '$cmd' not found in PATH. Source your ESP-IDF export.sh first." >&2
        exit 1
    fi
done

echo "Reading chip info and MAC from $PORT..."
READ_OUT="$(esptool.py --port "$PORT" read_mac 2>&1)" || {
    echo "$READ_OUT" >&2
    echo "Error: Failed to read MAC from $PORT. Is the device plugged in and accessible?" >&2
    exit 1
}

CHIP="$(printf '%s\n' "$READ_OUT" | awk -F'Detecting chip type... ' '/Detecting chip type\.\.\./ {print $2; exit}' | tr -d '\r')"
MAC_COLON="$(printf '%s\n' "$READ_OUT" | awk -F': ' '
    /^BASE MAC:/ { base=$2 }
    /^MAC:/      { mac=$2 }
    END {
        val = (base != "" ? base : mac)
        gsub(/\r/, "", val)
        print tolower(val)
    }
')"

if [[ -z "$MAC_COLON" ]]; then
    echo "$READ_OUT" >&2
    echo "Error: Could not parse MAC address from esptool.py output." >&2
    exit 1
fi

MAC_DOT="${MAC_COLON//:/\.}"
CHIP_NORM="$(printf '%s' "${CHIP:-unknown}" | tr '[:upper:]' '[:lower:]' | tr -d ' -')"

echo "Detected chip : ${CHIP:-unknown}"
echo "Device MAC    : $MAC_COLON ($MAC_DOT)"

# Check eFuse status so we never generate a mismatched flash encryption key for an already-encrypted chip
EFUSE_OUT="$(espefuse.py --port "$PORT" summary 2>&1 || true)"
ALREADY_ENCRYPTED=0
if printf '%s\n' "$EFUSE_OUT" | grep -qE '^(SPI_BOOT_CRYPT_CNT|FLASH_CRYPT_CNT).*=[[:space:]]*(Enable|[1-9])|^KEY_PURPOSE_0.*=[[:space:]]*XTS_AES'; then
    ALREADY_ENCRYPTED=1
fi

mkdir -p "$REPO_ROOT/keys"

FLASH_KEY="$REPO_ROOT/keys/${MAC_DOT}.flash-encryption.bin"
NVS_KEY="$REPO_ROOT/keys/${MAC_DOT}.nvs_keys.bin"

if [[ "$ALREADY_ENCRYPTED" -eq 1 ]]; then
    echo ""
    echo "WARNING: This chip already has flash encryption eFuses burned!"
    echo "         Skipping flash encryption key generation (a new random key would NOT match BLOCK_KEY0)."
    if [[ -f "$FLASH_KEY" ]]; then
        echo "         Existing key file found: keys/${MAC_DOT}.flash-encryption.bin"
    else
        echo "         No existing keys/${MAC_DOT}.flash-encryption.bin found in keys/."
    fi
elif [[ -f "$FLASH_KEY" ]]; then
    echo "Flash key already exists (keeping): keys/${MAC_DOT}.flash-encryption.bin"
else
    espsecure.py generate_flash_encryption_key "$FLASH_KEY"
    chmod 600 "$FLASH_KEY"
    echo "Created flash key : keys/${MAC_DOT}.flash-encryption.bin"
fi

if [[ -f "$NVS_KEY" ]]; then
    echo "NVS key already exists (keeping)  : keys/${MAC_DOT}.nvs_keys.bin"
else
    python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
        generate-key --keyfile "${MAC_DOT}.nvs_keys.bin" --outdir "$REPO_ROOT"
    chmod 600 "$NVS_KEY"
    echo "Created NVS key   : keys/${MAC_DOT}.nvs_keys.bin"
fi

echo ""
if [[ "$ALREADY_ENCRYPTED" -eq 0 ]]; then
    echo "Next step (one-time per unencrypted chip — burns flash key into eFuse):"
    if [[ "$CHIP_NORM" == "esp32" ]]; then
        echo "  espefuse.py --port $PORT burn_key flash_encryption keys/${MAC_DOT}.flash-encryption.bin"
    else
        echo "  espefuse.py --port $PORT burn_key BLOCK_KEY0 keys/${MAC_DOT}.flash-encryption.bin XTS_AES_128_KEY"
    fi
fi

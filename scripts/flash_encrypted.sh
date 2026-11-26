#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT="${1:-${PORT:-/dev/ttyACM0}}"

if [[ ! -f build/project_description.json ]]; then
    echo "Error: build/project_description.json not found. Run 'idf.py build' first." >&2
    exit 1
fi

TARGET="$(python -c 'import json; print(json.load(open("build/project_description.json"))["target"])')"
case "$TARGET" in
    esp32c3|esp32c6|esp32s3) BOOT_ADDR="0x0" ;;
    esp32c5)                 BOOT_ADDR="0x2000" ;;
    *)
        echo "Error: Unsupported target '$TARGET' (expected esp32c3, esp32c5, esp32c6, or esp32s3)." >&2
        exit 1
        ;;
esac

# 1. Read 6-byte device MAC
MAC="$(esptool.py --port "$PORT" read_mac 2>/dev/null | awk -F': ' '
    /^BASE MAC:/ { base=$2 }
    /^MAC:/      { mac=$2 }
    END {
        val = (base != "" ? base : mac)
        gsub(/\r/, "", val)
        gsub(/:/, ".", val)
        print tolower(val)
    }
')"
if [[ -z "$MAC" ]]; then
    echo "Error: Failed to read MAC from $PORT." >&2
    exit 1
fi

# 2. Verify device is currently encrypted
EFUSE_OUT="$(espefuse.py --port "$PORT" summary 2>&1)"
if ! printf '%s\n' "$EFUSE_OUT" | grep -qE '^SPI_BOOT_CRYPT_CNT.*=[[:space:]]*Enable'; then
    echo "Error: Device $MAC on $PORT does not have flash encryption enabled (SPI_BOOT_CRYPT_CNT != Enable)." >&2
    exit 1
fi

FLASH_KEY="keys/${MAC}.flash-encryption.bin"
NVS_KEY="keys/${MAC}.nvs_keys.bin"
if [[ ! -f "$FLASH_KEY" ]]; then
    echo "Error: Flash encryption key not found: $FLASH_KEY" >&2
    exit 1
fi
if [[ ! -f "$NVS_KEY" ]]; then
    echo "Error: NVS encryption key not found: $NVS_KEY" >&2
    exit 1
fi

echo "Erasing and flashing encrypted $TARGET ($MAC) on $PORT..."

# 3. Encrypt build artifacts and NVS key partition
espsecure.py encrypt_flash_data --aes_xts --keyfile "$FLASH_KEY" --address "$BOOT_ADDR" --output build/bootloader/bootloader-enc.bin          build/bootloader/bootloader.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "$FLASH_KEY" --address 0x10000      --output build/partition_table/partition-table-enc.bin build/partition_table/partition-table.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "$FLASH_KEY" --address 0x11000      --output build/encrypted_nvs_keys.bin                  "$NVS_KEY"
espsecure.py encrypt_flash_data --aes_xts --keyfile "$FLASH_KEY" --address 0x19000      --output build/ota_data_initial-enc.bin                build/ota_data_initial.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "$FLASH_KEY" --address 0x20000      --output build/app-enc.bin                             build/signal-scout-watcher.bin

# 4. Erase flash (resets NVS and storage) and flash encrypted images
esptool.py --chip "$TARGET" --port "$PORT" erase_flash --force
esptool.py --chip "$TARGET" --port "$PORT" --baud 460800 write_flash --force \
    "$BOOT_ADDR" build/bootloader/bootloader-enc.bin \
    0x10000      build/partition_table/partition-table-enc.bin \
    0x11000      build/encrypted_nvs_keys.bin \
    0x19000      build/ota_data_initial-enc.bin \
    0x20000      build/app-enc.bin

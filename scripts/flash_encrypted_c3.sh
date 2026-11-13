#!/usr/bin/env bash
set -euo pipefail

MAC="${1:?Usage: $0 <mac_address> [port]}"
MAC="${MAC//:/\.}"
MAC="${MAC,,}"
PORT="${2:-/dev/ttyACM0}"

grep -q '"target"[[:space:]]*:[[:space:]]*"esp32c3"' build/project_description.json || { echo "Error: build/ is not esp32c3" >&2; exit 1; }

# 1. Generate encrypted NVS partition (NVS uses its own keys in keys/${MAC}.nvs_keys.bin)
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" encrypt \
    "device-secrets/${MAC}.device_secrets.csv" build/device_nvs.bin 0x6000 \
    --inputkey "keys/${MAC}.nvs_keys.bin"

# 2. Encrypt flash partitions with the proven offsets
espsecure.py encrypt_flash_data --aes_xts --keyfile "keys/${MAC}.flash-encryption.bin" --address 0x0     --output build/bootloader/bootloader-enc.bin      build/bootloader/bootloader.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "keys/${MAC}.flash-encryption.bin" --address 0x10000 --output build/partition_table/partition-table-enc.bin build/partition_table/partition-table.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "keys/${MAC}.flash-encryption.bin" --address 0x11000 --output build/encrypted_nvs_keys.bin     "keys/${MAC}.nvs_keys.bin"
espsecure.py encrypt_flash_data --aes_xts --keyfile "keys/${MAC}.flash-encryption.bin" --address 0x19000 --output build/ota_data_initial-enc.bin build/ota_data_initial.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "keys/${MAC}.flash-encryption.bin" --address 0x20000 --output build/app-enc.bin              build/signal-scout-watcher.bin

# 3. Flash all partitions, then monitor
esptool.py --chip esp32c3 --port "$PORT" --baud 460800 write_flash --force \
    0x0     build/bootloader/bootloader-enc.bin \
    0x10000 build/partition_table/partition-table-enc.bin \
    0x11000 build/encrypted_nvs_keys.bin \
    0x12000 build/device_nvs.bin \
    0x19000 build/ota_data_initial-enc.bin \
    0x20000 build/app-enc.bin

idf.py monitor -p "$PORT"

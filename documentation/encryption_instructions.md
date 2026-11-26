# Encrypted Firmware Instructions

Keys are named by each device's 6-byte MAC in dot format (`aa.bb.cc.dd.ee.ff`). Secrets (Wi-Fi, URLs, API key) are provisioned after flashing via the SoftAP Wi-Fi portal and stored encrypted in NVS (`nvs_keys` at `0x11000`), keeping them out of OTA firmware images.

## Prerequisites (`sdkconfig.defaults`)
Uncomment these before building for encrypted devices (keep `CONFIG_SECURE_INSECURE_ALLOW_DL_MODE=y` enabled so USB flashing stays open; comment them back out for unencrypted builds):
```ini
CONFIG_SECURE_INSECURE_ALLOW_DL_MODE=y
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
```

Check if a plugged-in board is already encrypted:
```bash
espefuse.py --port /dev/ttyACM0 summary | grep -E "Detecting chip|SPI_BOOT_CRYPT_CNT|DIS_DOWNLOAD_MANUAL_ENCRYPT|KEY_PURPOSE_0|DIS_DOWNLOAD_MODE|SECURE_BOOT_EN"
```

---

## First-Time Setup (New C3, C5, C6, or S3)

**C3, C6, and S3** use `BOOT_ADDR=0x0`. **C5** uses `BOOT_ADDR=0x2000`.

### 1. Set Environment Variables
```bash
TARGET="esp32s3"   # esp32c3, esp32c5, esp32c6, or esp32s3
BOOT_ADDR="0x0"    # 0x2000 for esp32c5; 0x0 for esp32c3, esp32c6, esp32s3
PORT="/dev/ttyACM0"
```

### 2. Set Target, Erase Flash & Generate Keys
```bash
idf.py fullclean && idf.py set-target "$TARGET"
idf.py erase-flash -p "$PORT"
bash scripts/generate_device_keys.sh "$PORT"
```

### 3. Burn Flash Key to eFuse (One-Time Only)
Set `MAC` from the script output above (dot format):
```bash
MAC="<aa.bb.cc.dd.ee.ff>"
KEY="keys/${MAC}.flash-encryption.bin"

espefuse.py --port "$PORT" burn_key BLOCK_KEY0 "$KEY" XTS_AES_128_KEY
```

### 4. Build Firmware
```bash
idf.py build
```

### 5. Encrypt Build Artifacts Offline
```bash
espsecure.py encrypt_flash_data --aes_xts --keyfile "$KEY" --address "$BOOT_ADDR" --output build/bootloader/bootloader-enc.bin          build/bootloader/bootloader.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "$KEY" --address 0x10000      --output build/partition_table/partition-table-enc.bin build/partition_table/partition-table.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "$KEY" --address 0x11000      --output build/encrypted_nvs_keys.bin                  "keys/${MAC}.nvs_keys.bin"
espsecure.py encrypt_flash_data --aes_xts --keyfile "$KEY" --address 0x19000      --output build/ota_data_initial-enc.bin                build/ota_data_initial.bin
espsecure.py encrypt_flash_data --aes_xts --keyfile "$KEY" --address 0x20000      --output build/app-enc.bin                             build/signal-scout-watcher.bin
```

### 6. Flash Pre-Encrypted Images
*(Add `--no-stub` before `write_flash` on `esp32c5` if the stub fails.)*
```bash
esptool.py --chip "$TARGET" --port "$PORT" --baud 460800 --after no_reset write_flash --force \
    "$BOOT_ADDR" build/bootloader/bootloader-enc.bin \
    0x10000      build/partition_table/partition-table-enc.bin \
    0x11000      build/encrypted_nvs_keys.bin \
    0x19000      build/ota_data_initial-enc.bin \
    0x20000      build/app-enc.bin
```

### 7. Enable Encryption eFuses (One-Time — Only After Step 6) & Monitor
```bash
espefuse.py --port "$PORT" burn_efuse SPI_BOOT_CRYPT_CNT 1 DIS_DOWNLOAD_MANUAL_ENCRYPT 1
idf.py monitor -p "$PORT"
```

After first boot, connect to `signal-scout-<mac>` (password `signalscout`) to provision Wi-Fi, URLs, and auth key into encrypted NVS.

### 8. OTA Updates
Once encrypted, all future firmware updates can be done via OTA. Upload an unencrypted firmware to the dashboard, enable OTA updates, and then the running device encrypts the incoming app image automatically as it writes to the inactive OTA partition.

---

## Reflashing Over USB (Already-Encrypted Device)
- **Never** re-run `generate_device_keys.sh` for a flash key, Step 3 (`burn_key`), Step 7 (`burn_efuse`), or `idf.py flash` without pre-encrypting the firmware.
- **Keep existing NVS:** Set `TARGET`, `BOOT_ADDR`, `PORT`, `MAC`, and `KEY`, then run Steps 4, 5, and 6 (omit `0x11000` and `--after no_reset`, and do not `erase-flash`).
- **Full wipe / partition migration:** Set those variables, run `esptool.py --chip "$TARGET" --port "$PORT" erase_flash --force`, then run Steps 4, 5, and 6 (including `0x11000`, without `--after no_reset`).

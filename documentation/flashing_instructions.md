# Unencrypted Firmware Instructions

Secrets (Wi-Fi, URLs, API key) are provisioned after flashing via the SoftAP Wi-Fi portal and stored in NVS, keeping them out of OTA firmware images.

## Prerequisites (`sdkconfig.defaults`)
Make sure hardware flash encryption is **commented out** before building for unencrypted devices:
```ini
#CONFIG_SECURE_FLASH_ENC_ENABLED=y
#CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
```

Check that the plugged-in board is unencrypted (`SPI_BOOT_CRYPT_CNT = Disable (0b000)`):
```bash
espefuse.py --port /dev/ttyACM0 summary | grep -E "Detecting chip|SPI_BOOT_CRYPT_CNT|FLASH_CRYPT_CNT|DIS_DOWNLOAD_MANUAL_ENCRYPT|KEY_PURPOSE_0|DIS_DOWNLOAD_MODE|SECURE_BOOT_EN"
```

---

## First-Time Setup (New C3, C5, C6, S3, or ESP32)

### 1. Set Environment Variables
```bash
TARGET="esp32s3"   # esp32c3, esp32c5, esp32c6, esp32s3, or esp32
PORT="/dev/ttyACM0" # /dev/ttyUSB0 on most classic ESP32 boards
```

### 2. Set Target & Erase Flash
```bash
idf.py fullclean && idf.py set-target "$TARGET"
idf.py erase-flash -p "$PORT"
```

### 3. Build, Flash & Monitor
```bash
idf.py -p "$PORT" build flash monitor
```

After first boot, connect to `signal-scout-<mac>` (password `signalscout`) to provision Wi-Fi, URLs, and auth key (see [`ap_provisioning_instructions.md`](ap_provisioning_instructions.md)).

Once provisioned, all future firmware updates can be done via OTA.

---

## Reflashing Over USB
- **Keep existing NVS:** Set `TARGET` and `PORT`, skip `erase-flash`, and run Step 3 (`idf.py -p "$PORT" build flash monitor`).
- **Full wipe / factory reset:** Run Steps 1, 2, and 3, then re-provision via the SoftAP portal.

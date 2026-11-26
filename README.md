# Signal Scout Watcher

Edge sensor firmware for the [Signal Scout](https://github.com/WhatIsInCreedmoor/Signal-Scout) electronic intelligence (ELINT) platform. Monitors Wi-Fi and Bluetooth Low Energy (BLE) signals, updates defintions periodically from server, and checks in with heartbeat so that the server knows when a device is online, active, or needs an OTA update.

## Features

* **Dual-Radio Sniffing:** Promiscuous Wi-Fi monitoring with background channel hopping alongside continuous NimBLE discovery.
* **Remote Definition Sync:** Pulls target OUIs and names from `/api/definitions/` with local caching on an encrypted FAT filesystem.
* **Resilient Networking:** Multi-credential Wi-Fi failover with flash persistence of the last connected network.
* **Over-The-Air (OTA) Updates:** Automated firmware downloads and safe rollbacks via the `/api/heartbeat/` API.
* **Cross-Target Support:** Configurations tuned for `esp32c3`, `esp32c5`, `esp32c6`, and `esp32s3`.
* **Hardware Security:** Supports optional AES-XTS flash encryption and per-device NVS encryption.

## Prerequisites

* **ESP-IDF v5.5+** installed and sourced (`source /path/to/esp-idf/export.sh`).
* A running **Signal Scout** server instance.

## Quick Start

By default, devices are configured **over Wi-Fi after flashing** using the built-in SoftAP captive portal (`signal-scout-<mac>`, password `signalscout`)—no secrets files or build-time environment variables are required. See the **[Wireless SoftAP Provisioning Guide](documentation/ap_provisioning_instructions.md)** for portal setup details.

### 1. Unencrypted Flashing (Default)
* **[Unencrypted Firmware Instructions](documentation/flashing_instructions.md)** — Build, flash, and monitor an unencrypted board (`esp32c3`, `esp32c5`, `esp32c6`, `esp32s3`, or `esp32`), then configure Wi-Fi, server URL, and API key via the SoftAP portal.

### 2. Encrypted USB Flashing
* **[Encrypted Firmware Instructions](documentation/encryption_instructions.md)** — Generate per-device keys (`scripts/generate_device_keys.sh`), burn `BLOCK_KEY0`, pre-encrypt and flash firmware + `nvs_keys` over USB, and enable hardware flash encryption eFuses before provisioning via the SoftAP portal.

### 3. Optional: Manual NVS Flashing from a Secrets File
If you prefer pre-provisioning Wi-Fi credentials and API keys from a file over USB instead of using the Wi-Fi portal:
1. Copy `device-secrets/aa.bb.cc.dd.ee.ff.device_secrets.csv.example` to `device-secrets/<MAC>.device_secrets.csv` and fill in `api_key` and `wifi_creds`.
2. For unencrypted boards, generate and flash the NVS partition directly:
   ```bash
   bash scripts/flash_nvs.sh
   ```
   *(For encrypted boards, encrypt the CSV with `nvs_partition_gen.py encrypt ... --inputkey keys/<MAC>.nvs_keys.bin` and flash it to `0x12000`.)*

## Project Structure

```text
├── CMakeLists.txt              # Project definition (signal-scout-watcher)
├── partitions.csv              # Custom partition table (NVS, OTA dual-boot, encrypted FAT)
├── device-secrets/             # Optional per-device CSV secrets: <MAC>.device_secrets.csv
├── documentation/
│   ├── ap_provisioning_instructions.md # Wireless SoftAP captive portal guide (default config)
│   ├── flashing_instructions.md        # Unencrypted USB flashing guide
│   └── encryption_instructions.md      # Hardware encryption & eFuse flashing guide
├── keys/                       # Per-device flash & NVS encryption keys (git-ignored)
├── main/
│   ├── CMakeLists.txt          # Main component build config
│   └── main.c                  # Core sniffer, HTTP client, and state machine
├── nvs-partitions/             # Generated binary NVS partitions (git-ignored)
├── scripts/
│   ├── generate_device_keys.sh # Reads MAC and generates flash & NVS encryption keys
│   ├── flash_nvs.sh            # Optional: generates and flashes unencrypted NVS from CSV
│   └── flash_encrypted.sh      # Reads MAC, verifies encryption, encrypts & flashes C3/C5/C6/S3
└── sdkconfig.defaults*         # Optimized configs per chip target (C3, C5, C6, S3, ESP32)
```


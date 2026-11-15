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

### 1. Build and Flash (Wireless SoftAP - Recommended)
No environment variables, secrets files, or CSVs needed before flashing:
```bash
idf.py set-target <TARGET>   # esp32c3, esp32c5, esp32c6, or esp32s3
idf.py build flash monitor -p /dev/ttyACM0
```
Connect to the `signal-scout-<mac>` Wi-Fi hotspot (Password: `signalscout`) to enter Wi-Fi credentials, API key, and server URL through the captive portal. See the **[Wireless SoftAP Provisioning Guide](documentation/ap_provisioning_instructions.md)** for details.

### 2. Alternative Flashing Methods
* **[Environment Values / NVS Flashing Guide](documentation/flashing_instructions.md)** — Pre-provision secrets and server URLs via USB using `environment_secrets.sh` and `scripts/flash_nvs.sh`.
* **[Hardware Encrypted Flashing Guide](documentation/encryption_instructions.md)** — Production provisioning covering eFuse key burning (`BLOCK_KEY0`), per-device NVS key generation, and pre-encrypted flashing.

## Project Structure

```text
├── CMakeLists.txt              # Project definition (signal-scout-watcher)
├── partitions.csv              # Custom partition table (NVS, OTA dual-boot, encrypted FAT)
├── environment_secrets.sh      # Server API endpoints (git-ignored)
├── device-secrets/             # Per-device CSV secrets: <MAC>.device_secrets.csv
├── documentation/
│   ├── ap_provisioning_instructions.md # Wireless SoftAP captive portal guide
│   ├── flashing_instructions.md        # Standard unencrypted USB NVS guide
│   └── encryption_instructions.md      # Hardware encryption & eFuse provisioning guide
├── main/
│   ├── CMakeLists.txt          # Main component build config
│   └── main.c                  # Core sniffer, HTTP client, and state machine
├── nvs-partitions/             # Generated binary NVS partitions (git-ignored)
├── scripts/
│   ├── flash_nvs.sh            # Auto-detects device MAC and flashes unencrypted NVS
│   └── flash_encrypted_c3.sh   # Offline pre-encryption & flash script for ESP32-C3
└── sdkconfig.defaults*         # Optimized configs per chip target (C3, C5, C6, S3)
```


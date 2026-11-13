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

1. **Configure Server Endpoints:**
   Copy the example environment secrets file and fill in your Signal Scout server URLs:
   ```bash
   cp environment_secrets.sh.example environment_secrets.sh
   # Edit API_ENDPOINT, API_HEARTBEAT_ENDPOINT, and API_DEFINITIONS_ENDPOINT
   source environment_secrets.sh
   ```

2. **Choose Your Flashing Method:**
   * **[Standard / Unencrypted Flashing Guide](documentation/flashing_instructions.md)** (Easiest)

     Step-by-step instructions to create a device in Signal Scout, build the firmware, and flash the device secrets using `scripts/flash_nvs.sh` (works across C3, C5, C6, and S3).
   * **[Hardware Encrypted Flashing Guide](documentation/encryption_instructions.md)**  (Best if there is any possibility of the device being in a non controlled environment)

     Production provisioning workflow covering eFuse key burning (`BLOCK_KEY0`), per-device NVS key generation, pre-encrypted flashing, and partition offsets.

## Project Structure

```text
├── CMakeLists.txt              # Project definition (signal-scout-watcher)
├── partitions.csv              # Custom partition table (NVS, OTA dual-boot, encrypted FAT)
├── environment_secrets.sh      # Server API endpoints (git-ignored)
├── device-secrets/             # Per-device CSV secrets: <MAC>.device_secrets.csv
├── documentation/
│   ├── flashing_instructions.md   # Standard unencrypted flashing guide
│   └── encryption_instructions.md # Hardware encryption & eFuse provisioning guide
├── main/
│   ├── CMakeLists.txt          # Main component build config
│   └── main.c                  # Core sniffer, HTTP client, and state machine
├── nvs-partitions/             # Generated binary NVS partitions (git-ignored)
├── scripts/
│   ├── flash_nvs.sh            # Auto-detects device MAC and flashes unencrypted NVS
│   └── flash_encrypted_c3.sh   # Offline pre-encryption & flash script for ESP32-C3
└── sdkconfig.defaults*         # Optimized configs per chip target (C3, C5, C6, S3)
```

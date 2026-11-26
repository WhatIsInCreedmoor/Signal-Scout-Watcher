# SoftAP Captive Portal Provisioning Instructions

This guide explains how to flash firmware and wirelessly configure devices using the built-in SoftAP captive portal. This method eliminates the need for build-time environment secrets or per-device CSV files. This will work for unencrypted device. Encrypted devices will need to have firmware encrypted before being flashed. After that, the device can be configured starting in step 2.

---

## 1. Build and Flash Firmware

Set your chip target (`esp32c3`, `esp32c5`, `esp32c6`, or `esp32s3`), build, and flash:

```bash
# Set target if not already configured
idf.py set-target <TARGET>

# Build, flash, and monitor
idf.py build flash monitor -p /dev/ttyACM0
```

*(You do not need to configure any environment variables or run `scripts/flash_nvs.sh`—all server URLs, API keys, and Wi-Fi credentials are configured directly through the captive portal.)*

---

## 2. Connect to the Provisioning Hotspot

When an unprovisioned or unverified device boots, it automatically hosts a Wi-Fi Access Point:

* **SSID:** `signal-scout-<mac>` *(e.g., `signal-scout-aabbccddeeff`)*
* **Password:** `signalscout`
* **Portal URL:** `http://192.168.4.1` *(Captive portal opens automatically when connected)*

---

## 3. Configure via Captive Portal

When the portal opens in your browser at `http://192.168.4.1`:

1. **Wi-Fi Networks:**
   * Enter your network **SSID** and **Password**.
   * Click **Add Another Network** to register backup/secondary Wi-Fi networks.
2. **Device API Key:**
   * Paste the 64-character Watcher API Key generated in your Signal Scout server dashboard (`/admin/targets/watcher/`).
3. **Server URL Configuration:**
   * **Auto-Populated:** The **Server Base URL** automatically pre-fills with any previously configured domain (e.g., `https://scout.example.com`).
   * **Customizable:** You can change this to any domain (e.g., `https://scout.example.com`).
   * **Auto-Normalization:** If you omit `https://`, the portal automatically prepends it and strips duplicate slashes.
   * **Auto-Derived Endpoints:** The Ingest (`/api/ingest/`), Heartbeat (`/api/heartbeat/`), and Definitions (`/api/definitions/`) endpoints are derived automatically. Expand **Advanced Endpoints** if you need to override individual paths.

---

## 4. Save and Handshake Verification

Click **Save & Connect**. The device writes configuration to flash and reboots into operational mode.

* **Automated Verification:** The device connects to Wi-Fi and calls `/api/heartbeat/`.
* **Lock-In (HTTP 200):** Once the server responds with HTTP 200, the device permanently locks in `wifi_verified = 1`. Temporary Wi-Fi drops or field outages will **never** trigger a reset.
* **Fail-Safe (3 Attempts):** If Wi-Fi fails to connect after provisioning, or if the server cannot be reached due to an invalid URL or bad API key, the device retries 3 times, clears the unverified credentials, and **automatically re-launches the `signal-scout-<mac>` hotspot** so you can fix any typos.

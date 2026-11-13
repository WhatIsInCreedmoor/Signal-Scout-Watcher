# Encrypted Firmware Instructions
**This project is compatible with full flash encryption. It has been tested and confirmed working. This specific documentation is lacking and should be used as hints until the fully updated.**

The assumption throughout this flashing guide is that the device's MAC Address is used to identify the device. It is used to identify the device in Signal Scout and the scripts assume you're using a file with the mac address. As far as this project is concerned, the device-secrets file can be named any name that you desire. 

The NVS partition is used to store secrets so that secrets don't need to be included in the firmware. The reason being is that OTA files can't be encrypted before being sent OTA. If someone accesses your OTA firmware, they'll have the ability to determine your API keys and WiFi credentials. These are moved to NVS, which is not part of the OTA files, so that they can't be sniffed mid flight. The tradeoff is that you must physically flash the device again to update API keys or WiFi credentials.

## 1. Find Your Device MAC Address
Run:
```bash
esptool.py --port /dev/ttyACM0 read_mac
```
Use the 6-byte address (`MAC:` on C3/S3, or `BASE MAC:` on C5/C6) in dot format, e.g. `aa.bb.cc.dd.ee.ff`.

## 2. Create Device Secrets & Keys (First Time Only)
1. Create `device-secrets/<MAC>.device_secrets.csv` using `device-secrets/aa.bb.cc.dd.ee.ff.device_secrets.csv.example` as a template.
2. Generate the NVS encryption key (writes into `keys/`):
   ```bash
   python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate-key --keyfile <MAC>.nvs_keys.bin
   ```
3. Generate the flash encryption key and burn it to the chip (**never overwrite an already-burned chip**):
   ```bash
   espsecure.py generate_flash_encryption_key keys/<MAC>.flash-encryption.bin
   espefuse.py --port /dev/ttyACM0 burn_key BLOCK_KEY0 keys/<MAC>.flash-encryption.bin XTS_AES_128_KEY
   ```

## 3. Build Firmware
Set your target (`esp32c3`, `esp32c5`, `esp32c6`, or `esp32s3`), load `environment_secrets.sh`, and build:
```bash
idf.py fullclean && idf.py set-target <TARGET>
source environment_secrets.sh && idf.py build
```

## 4. Encrypt & Flash
* **C3 script:**
  ```bash
  bash scripts/flash_encrypted_c3.sh <MAC>
  ```
* **Manual steps (other targets):**
  1. Encrypt `device-secrets/<MAC>.device_secrets.csv` (`0x6000`) with `nvs_partition_gen.py encrypt ... --inputkey keys/<MAC>.nvs_keys.bin`.
  2. Encrypt `bootloader.bin`, `partition-table.bin`, `keys/<MAC>.nvs_keys.bin`, `ota_data_initial.bin`, and `signal-scout-watcher.bin` with `espsecure.py encrypt_flash_data --aes_xts --keyfile keys/<MAC>.flash-encryption.bin --address <OFFSET> --output <OUT> <IN>`.
  3. Flash all 5 encrypted files plus `device_nvs.bin` (`0x12000`) with `esptool.py --chip <TARGET> --port /dev/ttyACM0 --baud 460800 write_flash --force ...` (add `--no-stub` on `esp32c5`).

## 5. Enable Encryption eFuses (First Time Only — Run After Flashing)
```bash
espefuse.py --port /dev/ttyACM0 burn_efuse SPI_BOOT_CRYPT_CNT 1 DIS_DOWNLOAD_MANUAL_ENCRYPT 1
```

## 6. Flash & Partition Offsets
All targets share `partitions.csv`, `CONFIG_PARTITION_TABLE_OFFSET=0x10000`, and `--flash_mode dio --flash_freq 80m --flash_size 4MB`. Only `bootloader.bin` differs on `esp32c5`:

| Target | `bootloader.bin` | `partition-table.bin` | `nvs_keys` | `nvs` | `phy_init` | `ota_data_initial.bin` (`otadata`) | `signal-scout-watcher.bin` (`ota_0`) | `ota_1` | `storage` |
|---|---|---|---|---|---|---|---|---|---|
| `esp32c3` | `0x0` | `0x10000` | `0x11000` | `0x12000` | `0x18000` | `0x19000` | `0x20000` | `0x1d0000` | `0x379000` |
| `esp32c5` | **`0x2000`** | `0x10000` | `0x11000` | `0x12000` | `0x18000` | `0x19000` | `0x20000` | `0x1d0000` | `0x379000` |
| `esp32c6` | `0x0` | `0x10000` | `0x11000` | `0x12000` | `0x18000` | `0x19000` | `0x20000` | `0x1d0000` | `0x379000` |
| `esp32s3` | `0x0` | `0x10000` | `0x11000` | `0x12000` | `0x18000` | `0x19000` | `0x20000` | `0x1d0000` | `0x379000` |

*Sizes: `nvs_keys` `4K`, `nvs` `24K` (`0x6000`), `phy_init` `4K`, `otadata` `8K` (`0x2000`), `ota_0` `1700K`, `ota_1` `1700K`, `storage` `500K`.*

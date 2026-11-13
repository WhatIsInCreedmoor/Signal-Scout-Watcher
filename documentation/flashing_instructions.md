# Unencrypted Firmware Instructions
The assumption throughout this flashing guide is that the device's MAC Address is used to identify the device. It is used to identify the device in Signal Scout and the scripts assume you're using a file with the mac address. As far as this project is concerned, the device-secrets file can be named any name that you desire.

The NVS partition is used to store secrets so that secrets don't need to be included in the firmware. The reason being is that OTA files can't be encrypted before being sent OTA. If someone accesses your OTA firmware, they'll have the ability to determine your API keys and WiFi credentials. These are moved to NVS, which is not part of the OTA files, so that they can't be sniffed mid flight. The tradeoff is that you must physically flash the device again to update API keys or WiFi credentials.

## 1. Identify Device MAC Address
Run:
```bash
esptool.py --port /dev/ttyACM0 read_mac
```
Use the 6-byte address (`MAC:` on C3/S3, or `BASE MAC:` on C5/C6) in dot format, e.g. `aa.bb.cc.dd.ee.ff`.

## 2. Create Device in Signal Scout
1. Open the Signal Scout admin panel (`/admin/`) and go to **Watchers** → **Add watcher**.
2. Save the new watcher (optionally set `device_type` and `deployment_location`; `mac_address` and `hardware_target` auto-populate on first heartbeat) and copy its generated `auth_key`.

## 3. Create the Device Secrets File
Copy the example file for your device:
```bash
cp device-secrets/aa.bb.cc.dd.ee.ff.device_secrets.csv.example device-secrets/<MAC>.device_secrets.csv
```
Edit `device-secrets/<MAC>.device_secrets.csv`:
* Paste the Signal Scout `auth_key` as `api_key`.
* Set `wifi_creds` (`network1:password1;network2:password2`).

## 4. Set Target, Build, and Flash Firmware
Set `<TARGET>` (`esp32c3`, `esp32c5`, `esp32c6`, or `esp32s3`), load `environment_secrets.sh`, build, and flash:
```bash
idf.py fullclean && idf.py set-target <TARGET>
source environment_secrets.sh && idf.py build
idf.py -p /dev/ttyACM0 flash
```

## 5. Generate and Flash NVS
*(Or simply run `bash scripts/flash_nvs.sh`)*
```bash
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate \
    device-secrets/<MAC>.device_secrets.csv nvs-partitions/<MAC>.device_nvs.bin 0x6000
parttool.py --partition-table-offset 0x10000 --port /dev/ttyACM0 write_partition --partition-name nvs --input nvs-partitions/<MAC>.device_nvs.bin
```

## 6. Monitor
```bash
idf.py -p /dev/ttyACM0 monitor
```

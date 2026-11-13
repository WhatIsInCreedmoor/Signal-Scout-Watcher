#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

// OTA Enable
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"

// NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#ifdef CONFIG_IDF_TARGET_ESP32C6
    #define RF_SW_PWR_PIN 3
    #define RF_ANT_SEL_PIN 14
#endif

// Device auth via NVS partitions
static char device_auth_key[65] = {0};

// FATFS with Wear Levelling
#define STORAGE_BASE_PATH "/storage"
#define STORAGE_PARTITION_LABEL "storage"
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// --- LEAN UPLOAD QUEUE ---
typedef struct {
    uint8_t mac[6];
    char network_type[5];
    int channel;
    int rssi;
    char name[33];
} pending_upload_t;

#define MAX_PENDING_UPLOADS 50
static pending_upload_t upload_queue[MAX_PENDING_UPLOADS];
static volatile bool is_uploading = false;
static int upload_count = 0;
static uint32_t first_detection_time = 0;
static SemaphoreHandle_t data_mutex;
static SemaphoreHandle_t defs_mutex;

// --- DYNAMIC DEFINITIONS ---
#define MAX_OUI 1000
#define MAX_NAMES 500

typedef struct { uint8_t val[3]; } oui_t;

static oui_t target_ouis[MAX_OUI];
static int num_target_ouis = 0;
static char target_names[MAX_NAMES][33];
static int num_target_names = 0;

static oui_t ignore_ouis[MAX_OUI];
static int num_ignore_ouis = 0;
static char ignore_names[MAX_NAMES][33];
static int num_ignore_names = 0;

// --- WIFI CONFIG ---
#define MAX_NETWORKS 10
typedef struct { char *ssid; char *password; } wifi_credential_t;
static wifi_credential_t known_networks[MAX_NETWORKS];
static int num_known_networks = 0;

static int last_network_idx = 0;
static int saved_network_idx = -1;

static void parse_networks(void) {
    char creds[256] = {0};
    nvs_handle_t nvs_handle;

    if (nvs_open("config", NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t required_size = sizeof(creds);
        nvs_get_str(nvs_handle, "wifi_creds", creds, &required_size);
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE("WIFI", "Failed to open 'config' NVS namespace.");
        return;
    }

    char *save_pair, *save_kv;
    char *pair = strtok_r(creds, ";", &save_pair);
    while (pair && num_known_networks < MAX_NETWORKS) {
        char *ssid = strtok_r(pair, ":", &save_kv);
        char *pass = strtok_r(NULL, ":", &save_kv);
        if (ssid && pass) {
            pass[strcspn(pass, "\r\n")] = 0;
            known_networks[num_known_networks].ssid = strdup(ssid);
            known_networks[num_known_networks].password = strdup(pass);
            num_known_networks++;
        }
        pair = strtok_r(NULL, ";", &save_pair);
    }

    FILE *f = fopen(STORAGE_BASE_PATH "/last_ssid.txt", "r");
    if (f) {
        char saved[64] = {0};
        if (fgets(saved, sizeof(saved), f)) {
            saved[strcspn(saved, "\r\n")] = 0;
            for (int i = 0; i < num_known_networks; i++) {
                if (strcmp(known_networks[i].ssid, saved) == 0) {
                    last_network_idx = i;
                    saved_network_idx = i;
                    break;
                }
            }
        }
        fclose(f);
    }
}

static bool connect_to_known_wifi(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    for (int offset = 0; offset < num_known_networks; offset++) {
        int i = (last_network_idx + offset) % num_known_networks;
        ESP_LOGI("WIFI", "Connecting to %s", known_networks[i].ssid);
        wifi_config_t wifi_config = {0};
        strlcpy((char *)wifi_config.sta.ssid, known_networks[i].ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, known_networks[i].password, sizeof(wifi_config.sta.password));

        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();

        int retries = 0;
        ip_info.ip.addr = 0;
        while (ip_info.ip.addr == 0 && retries < 45) {
            esp_netif_get_ip_info(netif, &ip_info);
            if (ip_info.ip.addr != 0) {
                last_network_idx = i;
                if (i != saved_network_idx) {
                    FILE *f = fopen(STORAGE_BASE_PATH "/last_ssid.txt", "w");
                    if (f) {
                        if (fputs(known_networks[i].ssid, f) != EOF) {
                            saved_network_idx = i;
                        }
                        fclose(f);
                    }
                }
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            retries++;
        }
        esp_wifi_disconnect();
    }
    return false;
}

// Fast memory comparison for Quick Sort and Binary Search
static int cmp_oui(const void *a, const void *b) {
    return memcmp(((oui_t*)a)->val, ((oui_t*)b)->val, 3);
}

// --- DYNAMIC PARSING LOGIC ---
static void parse_and_apply_definitions(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    xSemaphoreTake(defs_mutex, portMAX_DELAY);

    cJSON *t_ouis = cJSON_GetObjectItem(root, "target_ouis");
    cJSON *t_names = cJSON_GetObjectItem(root, "target_names");
    cJSON *i_ouis = cJSON_GetObjectItem(root, "ignore_ouis");
    cJSON *i_names = cJSON_GetObjectItem(root, "ignore_names");

    unsigned int m0, m1, m2;
    cJSON *item;

    num_target_ouis = 0;
    if (t_ouis) {
        cJSON_ArrayForEach(item, t_ouis) {
            if (num_target_ouis < MAX_OUI && sscanf(item->valuestring, "%x:%x:%x", &m0, &m1, &m2) == 3) {
                target_ouis[num_target_ouis].val[0] = m0; target_ouis[num_target_ouis].val[1] = m1; target_ouis[num_target_ouis].val[2] = m2;
                num_target_ouis++;
            }
        }
    }

    num_target_names = 0;
    if (t_names) {
        cJSON_ArrayForEach(item, t_names) {
            if (num_target_names < MAX_NAMES) {
                strlcpy(target_names[num_target_names++], item->valuestring, 33);
            }
        }
    }

    num_ignore_ouis = 0;
    if (i_ouis) {
        cJSON_ArrayForEach(item, i_ouis) {
            if (num_ignore_ouis < MAX_OUI && sscanf(item->valuestring, "%x:%x:%x", &m0, &m1, &m2) == 3) {
                ignore_ouis[num_ignore_ouis].val[0] = m0; ignore_ouis[num_ignore_ouis].val[1] = m1; ignore_ouis[num_ignore_ouis].val[2] = m2;
                num_ignore_ouis++;
            }
        }
    }

    num_ignore_names = 0;
    if (i_names) {
        cJSON_ArrayForEach(item, i_names) {
            if (num_ignore_names < MAX_NAMES) {
                strlcpy(ignore_names[num_ignore_names++], item->valuestring, 33);
            }
        }
    }

    if (num_target_ouis > 0) qsort(target_ouis, num_target_ouis, sizeof(oui_t), cmp_oui);
    if (num_ignore_ouis > 0) qsort(ignore_ouis, num_ignore_ouis, sizeof(oui_t), cmp_oui);

    xSemaphoreGive(defs_mutex);
    cJSON_Delete(root);
    ESP_LOGI("DEFS", "Loaded Targets: %d OUIs, %d Names | Loaded Ignores: %d OUIs, %d Names",
             num_target_ouis, num_target_names, num_ignore_ouis, num_ignore_names);
}

static void load_cached_definitions() {
    FILE* f = fopen(STORAGE_BASE_PATH "/defs.json", "r");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf) {
        size_t read_len = fread(buf, 1, size, f);
        buf[read_len] = '\0';
        parse_and_apply_definitions(buf);
        free(buf);
    }
    fclose(f);
}

// --- API PULL LOGIC ---
#define MAX_HTTP_RECV_BUFFER 8192
static char *download_buffer = NULL;
static int download_len = 0;
static char incoming_etag[64] = {0};

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "ETag") == 0) {
                strlcpy(incoming_etag, evt->header_value, sizeof(incoming_etag));
            }
            break;
        case HTTP_EVENT_ON_DATA:
            // Safely capture all incoming data, chunked or not
            char *new_buffer = realloc(download_buffer, download_len + evt->data_len + 1);
            if (new_buffer == NULL) {
                ESP_LOGE("HTTP", "Failed to allocate memory for JSON payload!");
                if (download_buffer) {
                    free(download_buffer);
                    download_buffer = NULL;
                }
                download_len = 0;
                return ESP_FAIL;
            }
            download_buffer = new_buffer;
            memcpy(download_buffer + download_len, evt->data, evt->data_len);
            download_len += evt->data_len;
            break;
        case HTTP_EVENT_ON_FINISH:
            if (download_buffer != NULL) {
                download_buffer[download_len] = '\0';
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void fetch_definitions_from_server() {
    ESP_LOGI("DEFS", "Checking for updated definitions...");

    esp_http_client_config_t config = {
        .url = API_DEFINITIONS_ENDPOINT,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE("DEFS", "Failed to initialize HTTP client");
        return;
    }
    esp_http_client_set_header(client, "X-API-Key", device_auth_key);

    char current_etag[64] = {0};
    FILE *f_etag = fopen(STORAGE_BASE_PATH "/etag.txt", "r");
    if (f_etag) {
        if (fgets(current_etag, sizeof(current_etag), f_etag) != NULL) {
            current_etag[strcspn(current_etag, "\r\n")] = 0; // strip newline
            if (strlen(current_etag) > 0) {
                esp_http_client_set_header(client, "If-None-Match", current_etag);
            }
        }
        fclose(f_etag);
    }

    download_len = 0;
    incoming_etag[0] = '\0';
    if (download_buffer) { free(download_buffer); download_buffer = NULL; }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && download_buffer != NULL) {
            download_buffer[download_len] = '\0';

            FILE* f = fopen(STORAGE_BASE_PATH "/defs.json", "w");
            if (f) {
                if (fputs(download_buffer, f) == EOF) {
                    ESP_LOGE("DEFS", "Failed to write defs.json");
                }
                fclose(f);
            } else {
                ESP_LOGE("DEFS", "Failed to open defs.json for writing");
            }

            if (incoming_etag[0] != '\0') {
                FILE* fe = fopen(STORAGE_BASE_PATH "/etag.txt", "w");
                if (fe) {
                    if (fputs(incoming_etag, fe) == EOF) {
                        ESP_LOGE("DEFS", "Failed to write etag.txt");
                    }
                    fclose(fe);
                } else {
                    ESP_LOGE("DEFS", "Failed to open etag.txt for writing");
                }
            }
            parse_and_apply_definitions(download_buffer);
            ESP_LOGI("DEFS", "Definitions updated successfully.");
        } else if (status == 304) {
            ESP_LOGI("DEFS", "Definitions unchanged (304).");
            ESP_LOGI("DEFS", "Currently active: %d Target OUIs, %d Target Names | %d Ignore OUIs, %d Ignore Names",
                     num_target_ouis, num_target_names, num_ignore_ouis, num_ignore_names);
        } else {
            ESP_LOGE("DEFS", "Definitions HTTP failed: %d", status);
        }
    }
    esp_http_client_cleanup(client);
    if (download_buffer) { free(download_buffer); download_buffer = NULL; }
}

// --- CORE SNIFFER ENGINE ---
static void IRAM_ATTR queue_target_for_upload(const uint8_t *mac, const char *proto, int channel, int rssi, const char *name) {
    if (xSemaphoreTake(data_mutex, 0) == pdTRUE) {
        bool found = false;
        for (int i = 0; i < upload_count; i++) {
            if (memcmp(upload_queue[i].mac, mac, 6) == 0 && upload_queue[i].channel == channel && strcmp(upload_queue[i].network_type, proto) == 0) {
                found = true;
                if (upload_queue[i].name[0] == '\0' && name != NULL && name[0] != '\0') {
                    strlcpy(upload_queue[i].name, name, sizeof(upload_queue[i].name));
                }
                break;
            }
        }

        if (!found && upload_count < MAX_PENDING_UPLOADS) {
            if (upload_count == 0) first_detection_time = xTaskGetTickCount();

            ESP_LOGI("DETECT", "%s Target Queued: %02x:%02x:%02x:%02x:%02x:%02x", proto, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            memcpy(upload_queue[upload_count].mac, mac, 6);
            strcpy(upload_queue[upload_count].network_type, proto);
            upload_queue[upload_count].channel = channel;
            upload_queue[upload_count].rssi = rssi;
            if (name) strlcpy(upload_queue[upload_count].name, name, 33);
            else upload_queue[upload_count].name[0] = '\0';

            upload_count++;
        }
        xSemaphoreGive(data_mutex);
    }
}

static void IRAM_ATTR wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;

    uint8_t *payload = pkt->payload;
    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t f_type = (fc >> 2) & 0x3;
    uint8_t f_subtype = (fc >> 4) & 0xF;

    uint8_t *mac_dst = &payload[4];
    uint8_t *mac_src = &payload[10];
    uint8_t *mac_to_log = mac_src;

    char temp_ssid[33] = {0};

    if (xSemaphoreTake(defs_mutex, 0) == pdTRUE) {
        oui_t src_oui, dst_oui;
        memcpy(src_oui.val, mac_src, 3);
        memcpy(dst_oui.val, mac_dst, 3);

        if (num_ignore_ouis > 0 && (bsearch(&src_oui, ignore_ouis, num_ignore_ouis, sizeof(oui_t), cmp_oui) || bsearch(&dst_oui, ignore_ouis, num_ignore_ouis, sizeof(oui_t), cmp_oui))) {
            xSemaphoreGive(defs_mutex);
            return;
        }

        if (type == WIFI_PKT_MGMT && f_type == 0 && (f_subtype == 4 || f_subtype == 5 || f_subtype == 8)) {
            if (pkt->rx_ctrl.sig_len > 36) {
                uint8_t *tags = payload + 24;
                if (f_subtype == 8 || f_subtype == 5) tags = payload + 36;
                int max_len = pkt->rx_ctrl.sig_len - (tags - payload);
                int i = 0;
                while (i < max_len - 1) {
                    uint8_t tid = tags[i], tlen = tags[i+1];
                    if (i + 2 + tlen > max_len) break;
                    if (tid == 0 && tlen > 0 && tlen <= 32) {
                        memcpy(temp_ssid, &tags[i+2], tlen);
                        temp_ssid[tlen] = '\0';
                        break;
                    }
                    i += 2 + tlen;
                }
            }
        }

        bool trigger = false;

        if (num_target_ouis > 0) {
            if (bsearch(&src_oui, target_ouis, num_target_ouis, sizeof(oui_t), cmp_oui)) {
                trigger = true;
            } else if (bsearch(&dst_oui, target_ouis, num_target_ouis, sizeof(oui_t), cmp_oui)) {
                trigger = true;
                mac_to_log = mac_dst;
            }
        }

        if (!trigger && temp_ssid[0] != '\0') {
            for (int i = 0; i < num_target_names; i++) {
                if (strcasestr(temp_ssid, target_names[i]) != NULL) {
                    trigger = true;
                    break;
                }
            }
        }

        if (trigger && temp_ssid[0] != '\0') {
            for (int i = 0; i < num_ignore_names; i++) {
                if (strcasestr(temp_ssid, ignore_names[i]) != NULL) {
                    trigger = false;
                    break;
                }
            }
        }

        xSemaphoreGive(defs_mutex);

        if (trigger) {
            queue_target_for_upload(mac_to_log, "WIFI", pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi, temp_ssid);
        }
    }
}

static int IRAM_ATTR ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_gap_disc_desc *desc = &event->disc;
        char dev_name[33] = {0};
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                int len = fields.name_len < 32 ? fields.name_len : 32;
                memcpy(dev_name, fields.name, len);
                dev_name[len] = '\0';
            }
        }

        uint8_t mac[6];
        for(int i=0; i<6; i++) mac[i] = desc->addr.val[5-i];

        if (xSemaphoreTake(defs_mutex, 0) == pdTRUE) {
            oui_t mac_oui;
            memcpy(mac_oui.val, mac, 3);

            if (num_ignore_ouis > 0 && bsearch(&mac_oui, ignore_ouis, num_ignore_ouis, sizeof(oui_t), cmp_oui)) {
                xSemaphoreGive(defs_mutex);
                return 0;
            }

            bool trigger = false;

            if (num_target_ouis > 0 && bsearch(&mac_oui, target_ouis, num_target_ouis, sizeof(oui_t), cmp_oui)) {
                trigger = true;
            }

            if (!trigger && dev_name[0] != '\0') {
                for (int i = 0; i < num_target_names; i++) {
                    if (strcasestr(dev_name, target_names[i]) != NULL) { trigger = true; break; }
                }
            }

            if (trigger && dev_name[0] != '\0') {
                for (int i = 0; i < num_ignore_names; i++) {
                    if (strcasestr(dev_name, ignore_names[i]) != NULL) { trigger = false; break; }
                }
            }
            xSemaphoreGive(defs_mutex);

            if (trigger) queue_target_for_upload(mac, "BT", 0, desc->rssi, dev_name);
        }
    }
    return 0;
}

// --- SYSTEM TASKS ---
void upload_task(void *pvParameters) {
    while (1) {
        bool should_upload = false;
        int current_count = 0;

        if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
            if (upload_count > 0 && (xTaskGetTickCount() - first_detection_time) > pdMS_TO_TICKS(10000)) {
                should_upload = true;
                current_count = upload_count;
            }
            xSemaphoreGive(data_mutex);
        }

        if (should_upload) {
            while (is_uploading) vTaskDelay(pdMS_TO_TICKS(1000));
            is_uploading = true;
            esp_wifi_set_promiscuous(false);

            if (connect_to_known_wifi()) {
                for (int i = 0; i < current_count; i++) {
                    esp_http_client_config_t config = {
                        .url = API_ENDPOINT,
                        .method = HTTP_METHOD_POST,
                        .crt_bundle_attach = esp_crt_bundle_attach
                    };
                    esp_http_client_handle_t client = esp_http_client_init(&config);
                    if (client == NULL) {
                        ESP_LOGE("UPLOAD", "Failed to initialize HTTP client");
                        break;
                    }
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    esp_http_client_set_header(client, "X-API-Key", device_auth_key);

                    char post_data[512];
                    snprintf(post_data, sizeof(post_data),
                        "{\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"network_type\":\"%s\",\"channel\":%d,\"signal_strength\":%d,\"device_name\":\"%s\"}",
                        upload_queue[i].mac[0], upload_queue[i].mac[1], upload_queue[i].mac[2], upload_queue[i].mac[3], upload_queue[i].mac[4], upload_queue[i].mac[5],
                        upload_queue[i].network_type, upload_queue[i].channel, upload_queue[i].rssi, upload_queue[i].name);

                    esp_http_client_set_post_field(client, post_data, strlen(post_data));
                    esp_http_client_perform(client);
                    esp_http_client_cleanup(client);
                }
                esp_wifi_disconnect();
            }

            if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
                upload_count = 0;
                xSemaphoreGive(data_mutex);
            }
            esp_wifi_set_promiscuous(true);
            is_uploading = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t _ota_http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        ESP_LOGI("OTA", "Connection established. Injecting X-API-Key header.");
        esp_http_client_set_header(evt->client, "X-API-Key", device_auth_key);
    }
    return ESP_OK;
}

void heartbeat_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        while (is_uploading) vTaskDelay(pdMS_TO_TICKS(1000));

        is_uploading = true;
        esp_wifi_set_promiscuous(false);

        if (connect_to_known_wifi()) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);

            // Get embedded firmware version
            const esp_app_desc_t *app_desc = esp_app_get_description();

            // 1. Send Heartbeat & Check for OTA
            download_len = 0;
            if (download_buffer) { free(download_buffer); download_buffer = NULL; }

            esp_http_client_config_t config = {
                .url = API_HEARTBEAT_ENDPOINT,
                .method = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .event_handler = _http_event_handler,
                .timeout_ms = 15000
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);

            char ota_url_buffer[256] = {0};
            bool update_needed = false;

            if (client != NULL) {
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_header(client, "X-API-Key", device_auth_key);

                char post_data[300];
                snprintf(post_data, sizeof(post_data),
                    "{\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"firmware_version\":\"%s\",\"hardware_target\":\"%s\"}",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], app_desc->version, CONFIG_IDF_TARGET);

                esp_http_client_set_post_field(client, post_data, strlen(post_data));
                ESP_LOGI("HEARTBEAT", "Sending Payload: %s", post_data);
                esp_err_t err = esp_http_client_perform(client);

                if (err == ESP_OK) {
                    int status = esp_http_client_get_status_code(client);
                    if (status == 200 && download_buffer != NULL) {
                        download_buffer[download_len] = '\0';

                        cJSON *root = cJSON_Parse(download_buffer);
                        if (root) {
                            cJSON *ota_url = cJSON_GetObjectItem(root, "ota_url");
                            if (ota_url && ota_url->valuestring) {
                                strncpy(ota_url_buffer, ota_url->valuestring, sizeof(ota_url_buffer) - 1);
                                update_needed = true;
                            }
                            cJSON_Delete(root);
                        }
                    }
                } else {
                    ESP_LOGE("HEARTBEAT", "HTTP POST request failed: %s", esp_err_to_name(err));
                }

                // FULLY CLEAN UP THE HTTP CLIENT AND BUFFERS BEFORE STARTING OTA
                esp_http_client_cleanup(client);
            } else {
                ESP_LOGE("HEARTBEAT", "Failed to initialize HTTP client");
            }
            if (download_buffer) { free(download_buffer); download_buffer = NULL; }
            download_len = 0;

            // START OTA NOW THAT RAM IS FREED
            if (update_needed) {
                ESP_LOGI("OTA", "Starting update from %s", ota_url_buffer);

                esp_http_client_config_t ota_client_config = {
                    .url = ota_url_buffer,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                    .keep_alive_enable = true,
                    .event_handler = _ota_http_event_handler,
                    .timeout_ms = 60000
                };
                esp_https_ota_config_t ota_config = {
                    .http_config = &ota_client_config,
                };

                // Shut down Bluetooth to free massive contiguous RAM for Cloudflare's 16KB TLS buffers
                ESP_LOGI("OTA", "Shutting down BLE to free heap...");
                nimble_port_stop();
                nimble_port_deinit();
                vTaskDelay(pdMS_TO_TICKS(200)); // Give the RTOS a moment to actually free the memory

                esp_err_t ota_ret = esp_https_ota(&ota_config);
                if (ota_ret == ESP_OK) {
                    ESP_LOGI("OTA", "Update successful. Rebooting...");
                    esp_restart();
                } else {
                    ESP_LOGE("OTA", "Update failed: %s", esp_err_to_name(ota_ret));
                    ESP_LOGI("OTA", "Rebooting to recover BLE and network stack...");
                    esp_restart(); // <-- Force a clean slate
                }
            }

            // 2. Fetch and Cache New Definitions
            fetch_definitions_from_server();

            esp_wifi_disconnect();
        }

        esp_wifi_set_promiscuous(true);
        is_uploading = false;
        vTaskDelay(pdMS_TO_TICKS(1800000)); // 30 minutes
    }
}

#if SOC_WIFI_SUPPORT_5G
static const uint8_t hop_channels[] = {1, 6, 11, 36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165};
#else
static const uint8_t hop_channels[] = {1, 6, 11};
#endif

void wifi_channel_hopper(void *pvParameters) {
    int idx = 0;
    while (1) {
        if (!is_uploading) {
            esp_wifi_set_channel(hop_channels[idx], WIFI_SECOND_CHAN_NONE);
            if (++idx >= (sizeof(hop_channels)/sizeof(hop_channels[0]))) idx = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void ble_on_sync(void) {
    struct ble_gap_disc_params disc_params = { .itvl = 160, .window = 80 };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_event_cb, NULL);
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    esp_err_t err;
    const esp_partition_t *key_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);

    if (key_part != NULL) {
        nvs_sec_cfg_t cfg;
        esp_err_t cfg_err = nvs_flash_read_security_cfg(key_part, &cfg);
        if (cfg_err == ESP_OK) {
            err = nvs_flash_secure_init(&cfg);
            if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_ERROR_CHECK(nvs_flash_erase());
                err = nvs_flash_secure_init(&cfg);
            }
            ESP_ERROR_CHECK(err);
        } else {
            ESP_LOGE("NVS", "NVS Keys missing or corrupt. Run provisioning script.");
            err = nvs_flash_init();
            if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_ERROR_CHECK(nvs_flash_erase());
                err = nvs_flash_init();
            }
            ESP_ERROR_CHECK(err);
        }
    } else {
        // Standard unencrypted device
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    }

    // Pull API Key from NVS
    nvs_handle_t nvs_handle;
    err = nvs_open("auth", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t required_size = sizeof(device_auth_key);
        esp_err_t get_err = nvs_get_str(nvs_handle, "api_key", device_auth_key, &required_size);
        if (get_err != ESP_OK) {
            ESP_LOGE("NVS", "Failed to read api_key from NVS! Err: %s", esp_err_to_name(get_err));
        } else {
            ESP_LOGI("NVS", "Successfully loaded Device API Key from NVS.");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE("NVS", "Failed to open 'auth' NVS namespace. Err: %s", esp_err_to_name(err));
        ESP_LOGW("NVS", "Have you run the provisioning script to flash the API key yet?");
    }

    // Mount FAT filesystem with wear levelling
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 2,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_BASE_PATH, STORAGE_PARTITION_LABEL, &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE("STORAGE", "Failed to mount FATFS (%s)", esp_err_to_name(err));
    }

    data_mutex = xSemaphoreCreateMutex();
    defs_mutex = xSemaphoreCreateMutex();

    // Load initial definitions from flash before radio starts
    load_cached_definitions();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    xTaskCreate(upload_task, "upload_task", 8192, NULL, 10, NULL);

#ifdef CONFIG_IDF_TARGET_ESP32C6
    gpio_reset_pin(RF_SW_PWR_PIN); gpio_set_direction(RF_SW_PWR_PIN, GPIO_MODE_OUTPUT); gpio_set_level(RF_SW_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_reset_pin(RF_ANT_SEL_PIN); gpio_set_direction(RF_ANT_SEL_PIN, GPIO_MODE_OUTPUT); gpio_set_level(RF_ANT_SEL_PIN, 1);
#endif

    esp_log_level_set("wifi", ESP_LOG_WARN);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    parse_networks();
    xTaskCreate(wifi_channel_hopper, "wifi_hop", 1536, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat_task", 8192, NULL, 5, NULL);

    // Mark app as valid. Cancel OTA rollback
    esp_ota_mark_app_valid_cancel_rollback();
}

#include "provisioning.h"
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"

#define TAG "PROV"

static char s_cached_api_endpoint[256] = {0};
static char s_cached_hb_endpoint[256] = {0};
static char s_cached_defs_endpoint[256] = {0};
static char s_device_mac_str[18] = {0};

bool is_device_provisioned(void) {
    nvs_handle_t h;
    char key[65] = {0};
    char creds[256] = {0};

    // Check 'auth' namespace for api_key
    esp_err_t err = nvs_open("auth", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(key);
        esp_err_t get_err = nvs_get_str(h, "api_key", key, &len);
        nvs_close(h);
        if (get_err != ESP_OK || strlen(key) == 0) {
            ESP_LOGI(TAG, "Device missing api_key in 'auth' namespace");
            return false;
        }
    } else {
        ESP_LOGI(TAG, "No 'auth' namespace found in NVS (err=%s)", esp_err_to_name(err));
        return false;
    }

    // Check 'config' namespace for wifi_creds
    err = nvs_open("config", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(creds);
        esp_err_t get_err = nvs_get_str(h, "wifi_creds", creds, &len);
        nvs_close(h);
        if (get_err != ESP_OK || strlen(creds) == 0) {
            ESP_LOGI(TAG, "Device missing wifi_creds in 'config' namespace");
            return false;
        }
    } else {
        ESP_LOGI(TAG, "No 'config' namespace found in NVS (err=%s)", esp_err_to_name(err));
        return false;
    }

    // Check for valid API and Heartbeat endpoints (either in NVS or compiled in)
    const char *endpoint = get_api_endpoint();
    if (!endpoint || endpoint[0] == '\0') {
        ESP_LOGI(TAG, "Device missing API endpoint");
        return false;
    }

    const char *hb = get_heartbeat_endpoint();
    if (!hb || hb[0] == '\0') {
        ESP_LOGI(TAG, "Device missing Heartbeat endpoint");
        return false;
    }

    return true;
}

const char* get_api_endpoint(void) {
    if (s_cached_api_endpoint[0] != '\0') return s_cached_api_endpoint;

    nvs_handle_t h;
    if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cached_api_endpoint);
        if (nvs_get_str(h, "api_url", s_cached_api_endpoint, &len) == ESP_OK && s_cached_api_endpoint[0] != '\0') {
            nvs_close(h);
            return s_cached_api_endpoint;
        }
        nvs_close(h);
    }
#ifdef API_ENDPOINT
    return API_ENDPOINT;
#else
    return "";
#endif
}

const char* get_heartbeat_endpoint(void) {
    if (s_cached_hb_endpoint[0] != '\0') return s_cached_hb_endpoint;

    nvs_handle_t h;
    if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cached_hb_endpoint);
        if (nvs_get_str(h, "hb_url", s_cached_hb_endpoint, &len) == ESP_OK && s_cached_hb_endpoint[0] != '\0') {
            nvs_close(h);
            return s_cached_hb_endpoint;
        }
        nvs_close(h);
    }
#ifdef API_HEARTBEAT_ENDPOINT
    return API_HEARTBEAT_ENDPOINT;
#else
    return "";
#endif
}

const char* get_definitions_endpoint(void) {
    if (s_cached_defs_endpoint[0] != '\0') return s_cached_defs_endpoint;

    nvs_handle_t h;
    if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cached_defs_endpoint);
        if (nvs_get_str(h, "defs_url", s_cached_defs_endpoint, &len) == ESP_OK && s_cached_defs_endpoint[0] != '\0') {
            nvs_close(h);
            return s_cached_defs_endpoint;
        }
        nvs_close(h);
    }
#ifdef API_DEFINITIONS_ENDPOINT
    return API_DEFINITIONS_ENDPOINT;
#else
    return "";
#endif
}

bool is_wifi_verified(void) {
    nvs_handle_t h;
    uint8_t verified = 0;
    if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "wifi_verified", &verified) == ESP_OK) {
            nvs_close(h);
            return (verified == 1);
        }
        nvs_close(h);
    }
    return false; // Not verified if key is absent (must achieve initial HTTP 200)
}

void mark_wifi_verified(void) {
    nvs_handle_t h;
    if (nvs_open("config", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t verified = 1;
        nvs_set_u8(h, "wifi_verified", verified);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Wi-Fi connection verified! Device operational state locked in.");
    }
}

void reset_to_provisioning_mode(void) {
    ESP_LOGW(TAG, "Erasing unverified Wi-Fi credentials and resetting to Provisioning AP...");
    nvs_handle_t h;
    if (nvs_open("config", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wifi_creds");
        nvs_erase_key(h, "wifi_verified");
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Embedded Setup Web Page
static const char PROVISIONING_HTML[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"<title>Signal Scout Provisioning</title>"
"<style>"
":root {"
"  --bg: #0f172a;"
"  --card: #1e293b;"
"  --border: #334155;"
"  --text: #f8fafc;"
"  --text-muted: #94a3b8;"
"  --primary: #38bdf8;"
"  --primary-hover: #0284c7;"
"  --accent: #22c55e;"
"  --danger: #ef4444;"
"}"
"* { box-sizing: border-box; margin: 0; padding: 0; }"
"body {"
"  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;"
"  background: var(--bg);"
"  color: var(--text);"
"  padding: 16px;"
"  display: flex;"
"  justify-content: center;"
"}"
".container { width: 100%; max-width: 480px; }"
".card {"
"  background: var(--card);"
"  border: 1px solid var(--border);"
"  border-radius: 12px;"
"  padding: 20px;"
"  margin-bottom: 16px;"
"  box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.3);"
"}"
"h1 { font-size: 1.4rem; font-weight: 700; color: #fff; margin-bottom: 4px; display: flex; align-items: center; gap: 8px; }"
".subtitle { font-size: 0.85rem; color: var(--text-muted); margin-bottom: 16px; }"
".mac-badge {"
"  display: inline-flex;"
"  align-items: center;"
"  gap: 8px;"
"  background: #0f172a;"
"  border: 1px solid var(--border);"
"  border-radius: 6px;"
"  padding: 6px 10px;"
"  font-family: monospace;"
"  font-size: 0.9rem;"
"  color: var(--primary);"
"  margin-bottom: 16px;"
"  cursor: pointer;"
"}"
".mac-badge:hover { border-color: var(--primary); }"
"label { display: block; font-size: 0.85rem; font-weight: 600; margin-bottom: 6px; color: #e2e8f0; }"
".help { font-size: 0.75rem; color: var(--text-muted); margin-top: 4px; margin-bottom: 12px; }"
"input[type=\"text\"], input[type=\"password\"] {"
"  width: 100%;"
"  background: #0f172a;"
"  border: 1px solid var(--border);"
"  border-radius: 6px;"
"  padding: 10px 12px;"
"  color: #fff;"
"  font-size: 0.9rem;"
"  outline: none;"
"  transition: border 0.2s;"
"}"
"input[type=\"text\"]:focus, input[type=\"password\"]:focus {"
"  border-color: var(--primary);"
"}"
".network-row {"
"  background: #0f172a;"
"  border: 1px solid var(--border);"
"  border-radius: 8px;"
"  padding: 12px;"
"  margin-bottom: 12px;"
"  position: relative;"
"}"
".network-header {"
"  display: flex;"
"  justify-content: space-between;"
"  align-items: center;"
"  font-size: 0.8rem;"
"  font-weight: 600;"
"  color: var(--text-muted);"
"  margin-bottom: 8px;"
"}"
".btn-remove {"
"  background: none;"
"  border: none;"
"  color: var(--danger);"
"  font-size: 0.8rem;"
"  cursor: pointer;"
"  font-weight: 600;"
"}"
".btn {"
"  width: 100%;"
"  padding: 12px;"
"  border-radius: 6px;"
"  font-size: 0.95rem;"
"  font-weight: 600;"
"  cursor: pointer;"
"  border: none;"
"  transition: background 0.2s;"
"}"
".btn-primary { background: var(--primary); color: #0f172a; }"
".btn-primary:hover { background: var(--primary-hover); }"
".btn-secondary {"
"  background: transparent;"
"  border: 1px dashed var(--border);"
"  color: var(--primary);"
"  margin-bottom: 16px;"
"}"
".btn-secondary:hover { border-color: var(--primary); background: rgba(56, 189, 248, 0.05); }"
".btn-toggle-eye {"
"  background: #0f172a;"
"  border: 1px solid var(--border);"
"  border-radius: 6px;"
"  color: var(--text-muted);"
"  padding: 0 10px;"
"  cursor: pointer;"
"  display: inline-flex;"
"  align-items: center;"
"  justify-content: center;"
"  flex-shrink: 0;"
"}"
".btn-toggle-eye:hover { border-color: var(--primary); color: #fff; }"
".advanced-toggle {"
"  font-size: 0.8rem;"
"  color: var(--primary);"
"  cursor: pointer;"
"  margin-bottom: 12px;"
"  display: inline-block;"
"}"
".advanced-fields { display: none; margin-top: 8px; }"
".advanced-fields.open { display: block; }"
".toast {"
"  display: none;"
"  background: var(--accent);"
"  color: #0f172a;"
"  padding: 12px;"
"  border-radius: 6px;"
"  font-weight: 600;"
"  text-align: center;"
"  margin-bottom: 16px;"
"}"
".spinner {"
"  display: inline-block;"
"  width: 16px;"
"  height: 16px;"
"  border: 2px solid #0f172a;"
"  border-top-color: transparent;"
"  border-radius: 50%;"
"  animation: spin 0.8s linear infinite;"
"  vertical-align: middle;"
"  margin-right: 6px;"
"}"
"@keyframes spin { to { transform: rotate(360deg); } }"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"  <div class=\"card\">"
"    <h1>Signal Scout</h1>"
"    <div class=\"subtitle\">Edge Sensor Provisioning Portal</div>"
"    <div id=\"mac_box\" class=\"mac-badge\" title=\"Click to copy MAC\">"
"      <span>MAC:</span> <span id=\"mac_display\">Loading...</span>"
"    </div>"
"    <div id=\"success_box\" class=\"toast\"></div>"
"    <form id=\"setup_form\">"
"      <div style=\"margin-bottom: 16px;\">"
"        <label for=\"base_url\">Server Base URL</label>"
"        <input type=\"text\" id=\"base_url\" placeholder=\"https://signalscout.example.org\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\" required>"
"        <div class=\"help\">Auto-populates endpoints below.</div>"
"        <span class=\"advanced-toggle\" onclick=\"toggleAdvanced()\">Advanced Endpoints</span>"
"        <div id=\"advanced_fields\" class=\"advanced-fields\">"
"          <label for=\"api_url\">Ingest Endpoint</label>"
"          <input type=\"text\" id=\"api_url\" placeholder=\"https://.../api/ingest/\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
"          <div class=\"help\">Target alerts upload URL.</div>"
"          <label for=\"hb_url\">Heartbeat Endpoint</label>"
"          <input type=\"text\" id=\"hb_url\" placeholder=\"https://.../api/heartbeat/\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
"          <div class=\"help\">Device health and OTA URL.</div>"
"          <label for=\"defs_url\">Definitions Endpoint</label>"
"          <input type=\"text\" id=\"defs_url\" placeholder=\"https://.../api/definitions/\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
"          <div class=\"help\">Target list sync URL.</div>"
"        </div>"
"      </div>"
"      <div style=\"margin-bottom: 16px;\">"
"        <label for=\"api_key\">Watcher Auth Key</label>"
"        <input type=\"text\" id=\"api_key\" placeholder=\"Paste generated key from Signal Scout admin\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\" required>"
"        <div class=\"help\">Obtained from Signal Scout Admin &rarr; Watchers.</div>"
"      </div>"
"      <div style=\"margin-bottom: 16px;\">"
"        <label>Wi-Fi Networks</label>"
"        <div id=\"wifi_container\"></div>"
"        <button type=\"button\" class=\"btn btn-secondary\" onclick=\"addWifiRow()\">+ Add Wi-Fi Network</button>"
"        <div class=\"help\">Device tries configured networks in order on each cycle.</div>"
"      </div>"
"      <button type=\"submit\" id=\"btn_submit\" class=\"btn btn-primary\">Save & Connect</button>"
"    </form>"
"  </div>"
"</div>"
"<script>"
"  let networkCount = 0;"
"  function toggleAdvanced() {"
"    document.getElementById('advanced_fields').classList.toggle('open');"
"  }"
"  function normalizeUrl(u) {"
"    u = u.trim().replace(/\\/+$/, '');"
"    if (!u) return '';"
"    if (!/^https?:\\/\\//i.test(u)) {"
"      u = 'https://' + u;"
"    }"
"    return u;"
"  }"
"  function updateEndpoints() {"
"    let raw = document.getElementById('base_url').value.trim();"
"    if (!raw) return;"
"    let base = normalizeUrl(raw);"
"    document.getElementById('api_url').value = base + '/api/ingest/';"
"    document.getElementById('hb_url').value = base + '/api/heartbeat/';"
"    document.getElementById('defs_url').value = base + '/api/definitions/';"
"  }"
"  document.getElementById('base_url').addEventListener('input', updateEndpoints);"
"  const eyeOpen = '<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>';"
"  const eyeSlash = '<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24\"/><line x1=\"1\" y1=\"1\" x2=\"23\" y2=\"23\"/></svg>';"
"  function togglePassVisibility(id) {"
"    const inp = document.getElementById('pass_input_' + id);"
"    const btn = document.getElementById('pass_btn_' + id);"
"    if (inp.type === 'text') {"
"      inp.type = 'password';"
"      btn.innerHTML = eyeSlash;"
"      btn.title = 'Show password';"
"    } else {"
"      inp.type = 'text';"
"      btn.innerHTML = eyeOpen;"
"      btn.title = 'Hide password';"
"    }"
"  }"
"  function addWifiRow(ssid = '', pass = '') {"
"    networkCount++;"
"    const container = document.getElementById('wifi_container');"
"    const div = document.createElement('div');"
"    div.className = 'network-row';"
"    div.id = 'net_row_' + networkCount;"
"    const currentId = networkCount;"
"    div.innerHTML = `"
"      <div class=\"network-header\">"
"        <span>Network #${networkCount}</span>"
"        ${networkCount > 1 ? `<button type=\"button\" class=\"btn-remove\" onclick=\"removeWifiRow(${currentId})\">Remove</button>` : ''}"
"      </div>"
"      <div style=\"margin-bottom: 8px;\">"
"        <input type=\"text\" class=\"wifi-ssid\" placeholder=\"Wi-Fi SSID (Name)\" value=\"${ssid}\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\" required>"
"      </div>"
"      <div style=\"display: flex; gap: 6px;\">"
"        <input type=\"text\" id=\"pass_input_${currentId}\" class=\"wifi-pass\" placeholder=\"Wi-Fi Password (blank if open)\" value=\"${pass}\" autocapitalize=\"none\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
"        <button type=\"button\" id=\"pass_btn_${currentId}\" class=\"btn-toggle-eye\" onclick=\"togglePassVisibility(${currentId})\" title=\"Hide password\">${eyeOpen}</button>"
"      </div>"
"    `;"
"    container.appendChild(div);"
"  }"
"  function removeWifiRow(id) {"
"    const row = document.getElementById('net_row_' + id);"
"    if (row) row.remove();"
"  }"
"  addWifiRow();"
"  fetch('/api/info').then(r => r.json()).then(data => {"
"    if (data.mac) {"
"      document.getElementById('mac_display').innerText = data.mac.toUpperCase();"
"      document.getElementById('mac_box').onclick = () => {"
"        navigator.clipboard.writeText(data.mac.toLowerCase());"
"        alert('Copied MAC to clipboard: ' + data.mac.toLowerCase());"
"      };"
"    }"
"    if (data.api_endpoint) document.getElementById('api_url').value = data.api_endpoint;"
"    if (data.hb_endpoint) document.getElementById('hb_url').value = data.hb_endpoint;"
"    if (data.defs_endpoint) document.getElementById('defs_url').value = data.defs_endpoint;"
"    if (data.api_key) document.getElementById('api_key').value = data.api_key;"
"  }).catch(() => {});"
"  document.getElementById('setup_form').addEventListener('submit', async (e) => {"
"    e.preventDefault();"
"    const btn = document.getElementById('btn_submit');"
"    btn.disabled = true;"
"    btn.innerHTML = '<span class=\"spinner\"></span>Saving...';"
"    const ssids = document.querySelectorAll('.wifi-ssid');"
"    const passes = document.querySelectorAll('.wifi-pass');"
"    let pairs = [];"
"    for (let i = 0; i < ssids.length; i++) {"
"      const s = ssids[i].value.trim();"
"      const p = passes[i].value.trim();"
"      if (s) { pairs.push(s + ':' + p); }"
"    }"
"    if (pairs.length === 0) {"
"      alert('Please enter at least one Wi-Fi network.');"
"      btn.disabled = false;"
"      btn.innerText = 'Save & Connect';"
"      return;"
"    }"
"    let baseVal = normalizeUrl(document.getElementById('base_url').value);"
"    let apiVal = document.getElementById('api_url').value.trim();"
"    let hbVal = document.getElementById('hb_url').value.trim();"
"    let defsVal = document.getElementById('defs_url').value.trim();"
"    if (!apiVal && baseVal) apiVal = baseVal + '/api/ingest/';"
"    else if (apiVal) apiVal = normalizeUrl(apiVal) + '/';"
"    if (!hbVal && baseVal) hbVal = baseVal + '/api/heartbeat/';"
"    else if (hbVal) hbVal = normalizeUrl(hbVal) + '/';"
"    if (!defsVal && baseVal) defsVal = baseVal + '/api/definitions/';"
"    else if (defsVal) defsVal = normalizeUrl(defsVal) + '/';"
"    const payload = {"
"      api_key: document.getElementById('api_key').value.trim(),"
"      api_url: apiVal,"
"      hb_url: hbVal,"
"      defs_url: defsVal,"
"      wifi_creds: pairs.join(';')"
"    };"
"    try {"
"      const res = await fetch('/save', {"
"        method: 'POST',"
"        headers: { 'Content-Type': 'application/json' },"
"        body: JSON.stringify(payload)"
"      });"
"      if (res.ok) {"
"        document.getElementById('setup_form').style.display = 'none';"
"        const toast = document.getElementById('success_box');"
"        toast.style.display = 'block';"
"        toast.innerHTML = '<strong>Success!</strong> Configuration saved to flash.<br>Device is rebooting now... You can now reconnect your device to your regular Wi-Fi.';"
"      } else {"
"        const err = await res.text();"
"        alert('Failed to save configuration: ' + err);"
"        btn.disabled = false;"
"        btn.innerText = 'Save & Connect';"
"      }"
"    } catch (err) {"
"      alert('Network error while saving: ' + err.message);"
"      btn.disabled = false;"
"      btn.innerText = 'Save & Connect';"
"    }"
"  });"
"</script>"
"</body>"
"</html>";

// Delayed Reboot Task
static void restart_delayed_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "Rebooting device into operational mode...");
    esp_restart();
}

// HTTP Server Handlers
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROVISIONING_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t *req) {
    char key[65] = {0};
    nvs_handle_t h_auth;
    if (nvs_open("auth", NVS_READONLY, &h_auth) == ESP_OK) {
        size_t len = sizeof(key);
        nvs_get_str(h_auth, "api_key", key, &len);
        nvs_close(h_auth);
    }

    char json_resp[640];
    snprintf(json_resp, sizeof(json_resp),
             "{\"mac\":\"%s\",\"target\":\"%s\",\"api_endpoint\":\"%s\",\"hb_endpoint\":\"%s\",\"defs_endpoint\":\"%s\",\"api_key\":\"%s\"}",
             s_device_mac_str, CONFIG_IDF_TARGET,
             get_api_endpoint(), get_heartbeat_endpoint(), get_definitions_endpoint(), key);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void sanitize_url(const char *in, char *out, size_t out_max) {
    if (!in || strlen(in) == 0) {
        out[0] = '\0';
        return;
    }
    while (*in == ' ' || *in == '\t') in++;
    if (strncasecmp(in, "http://", 7) == 0 || strncasecmp(in, "https://", 8) == 0) {
        strlcpy(out, in, out_max);
    } else {
        snprintf(out, out_max, "https://%s", in);
    }
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\r' || out[len - 1] == '\n')) {
        out[--len] = '\0';
    }
    if (len > 0 && out[len - 1] != '/' && len < out_max - 1) {
        out[len] = '/';
        out[len + 1] = '\0';
    }
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[1024] = {0};
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
        return ESP_FAIL;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    cJSON *j_api_key = cJSON_GetObjectItem(root, "api_key");
    cJSON *j_wifi_creds = cJSON_GetObjectItem(root, "wifi_creds");
    cJSON *j_api_url = cJSON_GetObjectItem(root, "api_url");
    cJSON *j_hb_url = cJSON_GetObjectItem(root, "hb_url");
    cJSON *j_defs_url = cJSON_GetObjectItem(root, "defs_url");

    if (!j_api_key || !j_api_key->valuestring || strlen(j_api_key->valuestring) == 0 ||
        !j_wifi_creds || !j_wifi_creds->valuestring || strlen(j_wifi_creds->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "API key and Wi-Fi credentials are required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving credentials to NVS...");

    // 1. Write 'api_key' to 'auth' namespace
    nvs_handle_t h_auth;
    esp_err_t err = nvs_open("auth", NVS_READWRITE, &h_auth);
    if (err == ESP_OK) {
        nvs_set_str(h_auth, "api_key", j_api_key->valuestring);
        nvs_commit(h_auth);
        nvs_close(h_auth);
        ESP_LOGI(TAG, "Saved 'api_key' to 'auth' namespace");
    } else {
        ESP_LOGE(TAG, "Failed to open 'auth' namespace for writing (%s)", esp_err_to_name(err));
    }

    // 2. Write 'wifi_creds' and custom endpoints to 'config' namespace
    nvs_handle_t h_config;
    err = nvs_open("config", NVS_READWRITE, &h_config);
    if (err == ESP_OK) {
        nvs_set_str(h_config, "wifi_creds", j_wifi_creds->valuestring);
        // Mark as unverified (0) so the first boot verifies connection before locking it in
        nvs_set_u8(h_config, "wifi_verified", 0);

        if (j_api_url && j_api_url->valuestring && strlen(j_api_url->valuestring) > 0) {
            char clean_url[256];
            sanitize_url(j_api_url->valuestring, clean_url, sizeof(clean_url));
            nvs_set_str(h_config, "api_url", clean_url);
        }
        if (j_hb_url && j_hb_url->valuestring && strlen(j_hb_url->valuestring) > 0) {
            char clean_url[256];
            sanitize_url(j_hb_url->valuestring, clean_url, sizeof(clean_url));
            nvs_set_str(h_config, "hb_url", clean_url);
        }
        if (j_defs_url && j_defs_url->valuestring && strlen(j_defs_url->valuestring) > 0) {
            char clean_url[256];
            sanitize_url(j_defs_url->valuestring, clean_url, sizeof(clean_url));
            nvs_set_str(h_config, "defs_url", clean_url);
        }

        nvs_commit(h_config);
        nvs_close(h_config);
        ESP_LOGI(TAG, "Saved Wi-Fi credentials and endpoints to 'config' namespace");
    } else {
        ESP_LOGE(TAG, "Failed to open 'config' namespace for writing (%s)", esp_err_to_name(err));
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Saved! Rebooting...\"}");

    // Spawn restart task
    xTaskCreate(restart_delayed_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t info = { .uri = "/api/info", .method = HTTP_GET, .handler = info_get_handler };
        httpd_register_uri_handler(server, &info);

        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
        httpd_register_uri_handler(server, &save);

        // Captive portal probes for Android, iOS, Windows
        httpd_uri_t gen204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler };
        httpd_register_uri_handler(server, &gen204);

        httpd_uri_t hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler };
        httpd_register_uri_handler(server, &hotspot);

        httpd_uri_t canon = { .uri = "/canonical.html", .method = HTTP_GET, .handler = captive_redirect_handler };
        httpd_register_uri_handler(server, &canon);

        httpd_uri_t connect = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler };
        httpd_register_uri_handler(server, &connect);

        httpd_uri_t ncsi = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler };
        httpd_register_uri_handler(server, &ncsi);

        ESP_LOGI(TAG, "Provisioning Web Server started on port 80");
        return server;
    }
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

// Lightweight DNS Captive Portal Server
static void dns_captive_portal_task(void *pvParameters) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket on port 53: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Captive Portal Server active on UDP port 53");

    uint8_t rx[512];
    uint8_t tx[512];

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;

        // Standard DNS query check
        if ((rx[2] & 0xFA) != 0) continue;

        // Skip to end of Question section
        int q_pos = 12;
        while (q_pos < len && rx[q_pos] != 0) {
            q_pos += 1 + rx[q_pos];
        }
        q_pos += 5; // terminating 0 + QTYPE (2) + QCLASS (2)
        if (q_pos > len || q_pos + 16 > sizeof(tx)) continue;

        memcpy(tx, rx, q_pos);
        tx[2] = 0x81; // Response flags
        tx[3] = 0x80;
        tx[6] = 0x00; tx[7] = 0x01; // ANCOUNT = 1
        tx[8] = 0x00; tx[9] = 0x00;
        tx[10] = 0x00; tx[11] = 0x00;

        int ans_pos = q_pos;
        tx[ans_pos++] = 0xc0; // Name pointer
        tx[ans_pos++] = 0x0c; // Points to offset 12
        tx[ans_pos++] = 0x00; tx[ans_pos++] = 0x01; // Type A
        tx[ans_pos++] = 0x00; tx[ans_pos++] = 0x01; // Class IN
        tx[ans_pos++] = 0x00; tx[ans_pos++] = 0x00; // TTL: 60s
        tx[ans_pos++] = 0x00; tx[ans_pos++] = 0x3c;
        tx[ans_pos++] = 0x00; tx[ans_pos++] = 0x04; // Length 4
        tx[ans_pos++] = 192; tx[ans_pos++] = 168; tx[ans_pos++] = 4; tx[ans_pos++] = 1; // 192.168.4.1

        sendto(sock, tx, ans_pos, 0, (struct sockaddr *)&client_addr, addr_len);
    }
}

void start_provisioning_portal(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_mac_str, sizeof(s_device_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "signal-scout-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Configuring SoftAP with SSID: %s", ap_ssid);

    // Initialize SoftAP netif
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {
        .ap = {
            .password = "signalscout",
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start DNS captive portal task
    xTaskCreate(dns_captive_portal_task, "dns_task", 3072, NULL, 5, NULL);

    // Start HTTP server
    start_webserver();

    ESP_LOGW(TAG, "==========================================================");
    ESP_LOGW(TAG, " SIGNAL SCOUT PROVISIONING PORTAL READY");
    ESP_LOGW(TAG, " Device MAC : %s", s_device_mac_str);
    ESP_LOGW(TAG, " Wi-Fi SSID : %s", ap_ssid);
    ESP_LOGW(TAG, " Wi-Fi Pass : signalscout");
    ESP_LOGW(TAG, " Web URL    : http://192.168.4.1/");
    ESP_LOGW(TAG, "==========================================================");

    // Block this task indefinitely while server runs
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

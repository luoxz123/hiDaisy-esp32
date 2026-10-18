#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <esp_netif_ip_addr.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>
#include <algorithm>
#include <esp_system.h>
#include <lwip/sockets.h>

static const char *TAG = "wifi";

static const int MAX_RETRY = 5;
static const int BASE_RETRY_DELAY_MS = 1000;
static const int PROVISION_TRIGGER_FAILS = 3;

// ── HTML Templates ──────────────────────────────────────────────────────────

static const char CONFIG_HTML_HEADER[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HiLily WiFi</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 20px;background:#f5f5f5}"
    ".card{background:white;border-radius:12px;padding:20px;margin:12px 0;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
    "input{width:100%%;padding:10px;margin:6px 0;box-sizing:border-box;font-size:16px;border:1px solid #ddd;border-radius:6px}"
    "button{padding:12px 20px;border:none;border-radius:6px;font-size:16px;cursor:pointer}"
    ".btn-add{width:100%%;background:#4CAF50;color:white}"
    ".btn-del{background:#f44336;color:white;padding:8px 16px;font-size:14px}"
    ".btn-use{background:#2196F3;color:white;padding:8px 16px;font-size:14px}"
    ".net{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #eee}"
    ".actions{display:flex;gap:8px;align-items:center}"
    ".net:last-child{border:none}"
    ".rssi{font-size:12px;color:#888}"
    "h2{color:#333}h3{color:#666;margin:8px 0}"
    ".status{padding:8px 12px;border-radius:6px;margin:8px 0;font-size:14px}"
    ".ok{background:#e8f5e9;color:#2e7d32}"
    ".warn{background:#fff3e0;color:#e65100}"
    "</style></head><body>"
    "<h2>&#127803; HiLily WiFi Setup</h2>";

static const char CONFIG_HTML_FOOTER[] =
    "</body></html>";

static const char PROVISION_OK_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HiLily</title>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:80px auto;padding:0 20px;text-align:center}"
    ".ok{font-size:48px;margin:20px 0}"
    "button{padding:14px 40px;background:#4CAF50;color:white;border:none;font-size:18px;"
    "border-radius:8px;cursor:pointer;margin-top:20px}</style></head><body>"
    "<div class='ok'>&#9989;</div>"
    "<h2>WiFi Saved!</h2>"
    "<p>Please press the <b>RST</b> button on the device to restart and connect.</p>"
    "<button onclick=\"fetch('/restart',{method:'POST'})\">Restart Now</button>"
    "</body></html>";

// ── Helper: URL decode ──────────────────────────────────────────────────────

static std::string url_decode(const std::string& val) {
    std::string decoded;
    for (size_t i = 0; i < val.size(); i++) {
        if (val[i] == '+') decoded += ' ';
        else if (val[i] == '%' && i + 2 < val.size()) {
            char hex[3] = {val[i+1], val[i+2], 0};
            decoded += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else decoded += val[i];
    }
    return decoded;
}

static std::string parse_form_field(const std::string& body, const char* key) {
    std::string k = std::string(key) + "=";
    size_t pos = body.find(k);
    if (pos == std::string::npos) return "";
    pos += k.length();
    size_t end = body.find('&', pos);
    return url_decode(body.substr(pos, end == std::string::npos ? end : end - pos));
}

static std::string html_escape(const std::string& val) {
    std::string escaped;
    for (char c : val) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

static constexpr const char* kPortalUrl = "http://192.168.4.1/";

static void SetPortalResponseHeaders(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

static const char* HttpMethodName(int method)
{
    switch (method) {
        case HTTP_GET:  return "GET";
        case HTTP_POST: return "POST";
        case HTTP_HEAD: return "HEAD";
        default:        return "OTHER";
    }
}

static esp_err_t SendPortalRedirect(httpd_req_t* req)
{
    SetPortalResponseHeaders(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", kPortalUrl);
    return httpd_resp_send(req, NULL, 0);
}

static bool ConsumeForceApBootFlag()
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    uint8_t force_ap = 0;
    esp_err_t err = nvs_get_u8(handle, "force_ap", &force_ap);
    if (err == ESP_OK && force_ap != 0) {
        nvs_erase_key(handle, "force_ap");
        nvs_commit(handle);
        nvs_close(handle);
        return true;
    }

    nvs_close(handle);
    return false;
}

static void SetForceApBootFlag(bool enabled)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    if (enabled) {
        nvs_set_u8(handle, "force_ap", 1);
        nvs_erase_key(handle, "try_saved");
    } else {
        nvs_erase_key(handle, "force_ap");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

static bool ConsumeTrySavedWifiBootFlag()
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    uint8_t try_saved = 0;
    esp_err_t err = nvs_get_u8(handle, "try_saved", &try_saved);
    if (err == ESP_OK && try_saved != 0) {
        nvs_erase_key(handle, "try_saved");
        nvs_commit(handle);
        nvs_close(handle);
        return true;
    }

    nvs_close(handle);
    return false;
}

static void SetTrySavedWifiBootFlag(bool enabled)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    if (enabled) {
        nvs_set_u8(handle, "try_saved", 1);
        nvs_erase_key(handle, "force_ap");
    } else {
        nvs_erase_key(handle, "try_saved");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

static void LogApIpInfo(esp_netif_t* netif)
{
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return;
    }

    char ip_str[16];
    char gw_str[16];
    char mask_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ip_info.gw));
    snprintf(mask_str, sizeof(mask_str), IPSTR, IP2STR(&ip_info.netmask));
    ESP_LOGI(TAG, "AP netif ready: ip=%s gw=%s mask=%s", ip_str, gw_str, mask_str);
}

// ── Singleton ───────────────────────────────────────────────────────────────

WifiManager& WifiManager::GetInstance()
{
    static WifiManager instance;
    return instance;
}

// ── NVS Multi-SSID ─────────────────────────────────────────────────────────

bool WifiManager::HasStoredCredentials() const
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t ret = nvs_get_str(handle, "ssid0", NULL, &len);
    nvs_close(handle);
    return (ret == ESP_OK && len > 1);
}

bool WifiManager::LoadAllNetworks()
{
    networks_.clear();
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return false;

    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char ssid_key[8], pass_key[8];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i);
        snprintf(pass_key, sizeof(pass_key), "pass%d", i);

        char buf[64] = {};
        size_t len = sizeof(buf);
        if (nvs_get_str(handle, ssid_key, buf, &len) == ESP_OK && len > 1) {
            WifiNetwork net;
            net.ssid = buf;
            len = sizeof(buf);
            memset(buf, 0, sizeof(buf));
            if (nvs_get_str(handle, pass_key, buf, &len) == ESP_OK) {
                net.password = buf;
            }
            networks_.push_back(net);
        }
    }
    nvs_close(handle);

    // Migration: if no ssid0 but old "ssid" key exists, migrate it
    if (networks_.empty()) {
        nvs_handle_t h2;
        if (nvs_open("wifi", NVS_READONLY, &h2) == ESP_OK) {
            char buf[64] = {};
            size_t len = sizeof(buf);
            if (nvs_get_str(h2, "ssid", buf, &len) == ESP_OK && len > 1) {
                WifiNetwork net;
                net.ssid = buf;
                len = sizeof(buf);
                memset(buf, 0, sizeof(buf));
                if (nvs_get_str(h2, "pass", buf, &len) == ESP_OK) {
                    net.password = buf;
                }
                networks_.push_back(net);
                nvs_close(h2);
                SaveAllNetworks();
                ESP_LOGI(TAG, "Migrated legacy WiFi credentials to slot 0");
                return true;
            }
            nvs_close(h2);
        }
    }

    ESP_LOGI(TAG, "Loaded %d WiFi network(s) from NVS", (int)networks_.size());
    return !networks_.empty();
}

void WifiManager::SaveAllNetworks()
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for wifi");
        return;
    }

    // Clear all slots first
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char ssid_key[8], pass_key[8];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i);
        snprintf(pass_key, sizeof(pass_key), "pass%d", i);
        nvs_erase_key(handle, ssid_key);
        nvs_erase_key(handle, pass_key);
    }
    // Also clear legacy single-network keys so deleted entries do not get migrated back.
    nvs_erase_key(handle, "ssid");
    nvs_erase_key(handle, "pass");

    // Write current networks
    for (int i = 0; i < (int)networks_.size() && i < WIFI_MAX_NETWORKS; i++) {
        char ssid_key[8], pass_key[8];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i);
        snprintf(pass_key, sizeof(pass_key), "pass%d", i);
        nvs_set_str(handle, ssid_key, networks_[i].ssid.c_str());
        nvs_set_str(handle, pass_key, networks_[i].password.c_str());
    }

    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved %d WiFi network(s) to NVS", (int)networks_.size());
}

bool WifiManager::AddNetwork(const std::string& ssid, const std::string& password)
{
    // Empty password for an existing SSID means "reuse saved password".
    std::string password_to_save = password;
    for (auto it = networks_.begin(); it != networks_.end(); ++it) {
        if (it->ssid == ssid) {
            if (password_to_save.empty()) {
                password_to_save = it->password;
            }
            networks_.erase(it);
            break;
        }
    }
    if ((int)networks_.size() >= WIFI_MAX_NETWORKS) {
        ESP_LOGW(TAG, "Max %d networks, removing last", WIFI_MAX_NETWORKS);
        networks_.pop_back();
    }
    networks_.insert(networks_.begin(), {ssid, password_to_save});
    SaveAllNetworks();
    ESP_LOGI(TAG, "Saved WiFi network: %s (total: %d, password=%s)",
             ssid.c_str(), (int)networks_.size(),
             password_to_save.empty() ? "empty/open" : "set");
    return true;
}

bool WifiManager::RemoveNetwork(int index)
{
    if (index < 0 || index >= (int)networks_.size()) return false;
    ESP_LOGI(TAG, "Removed WiFi network: %s", networks_[index].ssid.c_str());
    networks_.erase(networks_.begin() + index);
    SaveAllNetworks();
    return true;
}

// ── Init ────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::Init()
{
    bool has_networks = LoadAllNetworks();
    bool force_ap = ConsumeForceApBootFlag();
    bool try_saved_before_config = ConsumeTrySavedWifiBootFlag();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (force_ap || !has_networks) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    } else {
        sta_netif_ = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, this, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &IpEventHandler, this, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &IpEventHandler, this, NULL));

    initialized_ = true;

    if (force_ap) {
        ESP_LOGW(TAG, "Force AP flag set, booting directly into provisioning mode");
        return StartProvisioning();
    } else if (try_saved_before_config && has_networks) {
        ESP_LOGI(TAG, "WiFi setup requested; trying saved networks before provisioning");
        return InitSta();
    } else if (has_networks) {
        return InitSta();
    } else {
        ESP_LOGW(TAG, "No stored WiFi credentials, starting provisioning");
        return StartProvisioning();
    }
}

esp_err_t WifiManager::Init(const std::string& ssid, const std::string& password)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, this, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &IpEventHandler, this, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &IpEventHandler, this, NULL));

    initialized_ = true;

    // Load existing networks, add this one at front if not already there
    LoadAllNetworks();
    bool found = false;
    for (auto& n : networks_) {
        if (n.ssid == ssid) { found = true; break; }
    }
    if (!found) {
        AddNetwork(ssid, password);
    }

    return InitSta();
}

esp_err_t WifiManager::InitSta()
{
    if (networks_.empty()) {
        ESP_LOGE(TAG, "No networks configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sta_netif_) {
        sta_netif_ = esp_netif_create_default_wifi_sta();
    }
    current_network_idx_ = 0;
    connect_attempt_idx_ = 0;

    auto& net = networks_[current_network_idx_];
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, net.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, net.password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "WiFi STA init, trying: %s (%d networks configured)",
             net.ssid.c_str(), (int)networks_.size());
    return ESP_OK;
}

// ── DNS Captive Portal ───────────────────────────────────────────────────────

void WifiManager::DnsServerTask(void* arg)
{
    auto* self = static_cast<WifiManager*>(arg);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS server socket failed");
        vTaskDelete(NULL);
        return;
    }
    self->dns_socket_ = sock;

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS server bind failed");
        close(sock);
        self->dns_socket_ = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS captive portal started on port 53");

    uint8_t rx_buf[128];
    uint8_t tx_buf[256];
    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (true) {
        client_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr*)&client_addr, &client_len);
        if (len < 12) continue;  // Minimum DNS header

        // Build response: copy query, set response flags, append A record → 192.168.4.1
        memcpy(tx_buf, rx_buf, len);
        tx_buf[2] = 0x84;  // QR=1, AA=1
        tx_buf[3] = 0x00;  // RCODE=0 (no error)
        tx_buf[6] = 0x00;  // ANCOUNT = 1
        tx_buf[7] = 0x01;

        int off = len;
        tx_buf[off++] = 0xC0; tx_buf[off++] = 0x0C;  // Name pointer
        tx_buf[off++] = 0x00; tx_buf[off++] = 0x01;  // Type A
        tx_buf[off++] = 0x00; tx_buf[off++] = 0x01;  // Class IN
        tx_buf[off++] = 0x00; tx_buf[off++] = 0x00;
        tx_buf[off++] = 0x00; tx_buf[off++] = 0x01;  // TTL = 1s
        tx_buf[off++] = 0x00; tx_buf[off++] = 0x04;  // RDLENGTH = 4
        tx_buf[off++] = 192;  tx_buf[off++] = 168;
        tx_buf[off++] = 4;    tx_buf[off++] = 1;     // 192.168.4.1

        sendto(sock, tx_buf, off, 0,
              (struct sockaddr*)&client_addr, client_len);
        ESP_LOGI(TAG, "DNS query from %s → 192.168.4.1",
                 inet_ntoa(client_addr.sin_addr));
    }
}

void WifiManager::StartDnsServer()
{
    if (dns_task_) return;
    xTaskCreatePinnedToCore(DnsServerTask, "dns_cp", 3072, this, 3, &dns_task_, 0);
}

void WifiManager::StopDnsServer()
{
    if (dns_socket_ >= 0) {
        close(dns_socket_);
        dns_socket_ = -1;
    }
    if (dns_task_) {
        vTaskDelete(dns_task_);
        dns_task_ = nullptr;
    }
}

esp_err_t WifiManager::HttpCaptiveRedirect(httpd_req_t* req, httpd_err_code_t err)
{
    return SendPortalRedirect(req);
}

// Captive portal: catch-all wildcard handler for all unmatched GET requests.
// Returns 302 redirect to config page, triggering OS captive portal popup.
static esp_err_t HttpCaptiveHandler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "Captive redirect: %s %s", HttpMethodName(req->method), req->uri);
    return SendPortalRedirect(req);
}

esp_err_t WifiManager::HttpRestartHandler(httpd_req_t* req)
{
    SetPortalResponseHeaders(req);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Restarting...", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "Restart requested via web UI");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ── Provisioning (AP-only, for first setup) ─────────────────────────────────

esp_err_t WifiManager::StartProvisioning()
{
    provisioning_ = true;

    ESP_ERROR_CHECK(EnsureApNetif());

    // Derive AP SSID from MAC and store for external access
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid_buf[32];
    snprintf(ssid_buf, sizeof(ssid_buf), "HiLily-%02X%02X", mac[4], mac[5]);
    ap_ssid_ = ssid_buf;

    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ap_ssid_.c_str(), sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = ap_ssid_.length();
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    // Keep STA side enabled during provisioning so the web portal can actively scan nearby APs.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    LogApIpInfo(ap_netif_);

    // Start HTTP server with wildcard URI matching for captive portal
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    if (httpd_start(&http_server_, &config) == ESP_OK) {
        httpd_uri_t get_uri = { .uri = "/", .method = HTTP_GET,
            .handler = HttpGetHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &get_uri);

        httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST,
            .handler = HttpPostHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &post_uri);

        httpd_uri_t del_uri = { .uri = "/delete", .method = HTTP_POST,
            .handler = HttpDeleteHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &del_uri);

        httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET,
            .handler = HttpScanHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &scan_uri);

        httpd_uri_t restart_uri = { .uri = "/restart", .method = HTTP_POST,
            .handler = HttpRestartHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &restart_uri);

        // Captive portal: wildcard catch-all for ALL unmatched GET requests.
        // Must be registered LAST so specific handlers take priority.
        // Handles iOS (captive.apple.com), Android (connectivitycheck),
        // Windows (msftconnecttest), Firefox (detectportal) detection URLs.
        httpd_uri_t captive_uri = { .uri = "/*", .method = HTTP_GET,
            .handler = HttpCaptiveHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &captive_uri);

        // Also catch POST requests to unknown URIs
        httpd_uri_t captive_post = { .uri = "/*", .method = HTTP_POST,
            .handler = HttpCaptiveHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &captive_post);

        httpd_uri_t captive_head = { .uri = "/*", .method = HTTP_HEAD,
            .handler = HttpCaptiveHandler, .user_ctx = this };
        httpd_register_uri_handler(http_server_, &captive_head);

        httpd_register_err_handler(http_server_, HTTPD_404_NOT_FOUND, HttpCaptiveRedirect);
        httpd_register_err_handler(http_server_, HTTPD_405_METHOD_NOT_ALLOWED, HttpCaptiveRedirect);
    }

    // Start DNS server to hijack all DNS queries → 192.168.4.1
    StartDnsServer();

    ESP_LOGI(TAG, "Provisioning started: AP='%s', http://192.168.4.1",
             ap_ssid_.c_str());

    if (provisioning_callback_) {
        provisioning_callback_();
    }
    return ESP_OK;
}

void WifiManager::StopProvisioning()
{
    StopDnsServer();
    if (http_server_) {
        httpd_stop(http_server_);
        http_server_ = nullptr;
    }
    provisioning_ = false;
}

// ── Config Mode (STA+AP coexistence) ────────────────────────────────────────

esp_err_t WifiManager::StartConfigMode()
{
    if (provisioning_) return ESP_OK;  // already in config mode

    if (initialized_) {
        if (HasStoredCredentials()) {
            ESP_LOGI(TAG, "WiFi setup requested; rebooting to try saved networks first");
            SetTrySavedWifiBootFlag(true);
        } else {
            ESP_LOGI(TAG, "WiFi setup requested with no saved networks; rebooting into AP config mode");
            SetForceApBootFlag(true);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return ESP_OK;
    }

    provisioning_ = true;
    ESP_ERROR_CHECK(EnsureApNetif());

    // Derive AP SSID from MAC (reuse stored value if already set)
    if (ap_ssid_.empty()) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char ssid_buf[32];
        snprintf(ssid_buf, sizeof(ssid_buf), "HiLily-%02X%02X", mac[4], mac[5]);
        ap_ssid_ = ssid_buf;
    }

    if (connected_) {
        // STA+AP mode — keep connection while serving config
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ap_ssid_.c_str(), sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = ap_ssid_.length();
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (!connected_) {
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    LogApIpInfo(ap_netif_);

    // Start HTTP server if not already running
    if (!http_server_) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.lru_purge_enable = true;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.max_uri_handlers = 12;
        if (httpd_start(&http_server_, &config) == ESP_OK) {
            httpd_uri_t get_uri = { .uri = "/", .method = HTTP_GET,
                .handler = HttpGetHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &get_uri);

            httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST,
                .handler = HttpPostHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &post_uri);

            httpd_uri_t del_uri = { .uri = "/delete", .method = HTTP_POST,
                .handler = HttpDeleteHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &del_uri);

            httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET,
                .handler = HttpScanHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &scan_uri);

            httpd_uri_t restart_uri = { .uri = "/restart", .method = HTTP_POST,
                .handler = HttpRestartHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &restart_uri);

            httpd_uri_t captive_uri = { .uri = "/*", .method = HTTP_GET,
                .handler = HttpCaptiveHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &captive_uri);

            httpd_uri_t captive_post = { .uri = "/*", .method = HTTP_POST,
                .handler = HttpCaptiveHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &captive_post);

            httpd_uri_t captive_head = { .uri = "/*", .method = HTTP_HEAD,
                .handler = HttpCaptiveHandler, .user_ctx = this };
            httpd_register_uri_handler(http_server_, &captive_head);

            httpd_register_err_handler(http_server_, HTTPD_404_NOT_FOUND, HttpCaptiveRedirect);
            httpd_register_err_handler(http_server_, HTTPD_405_METHOD_NOT_ALLOWED, HttpCaptiveRedirect);
        }
    }

    ESP_LOGI(TAG, "Config mode started: AP='%s', IP=%s",
             ap_ssid_.c_str(), current_ip_.c_str());
    return ESP_OK;
}

void WifiManager::StopConfigMode()
{
    StopProvisioning();
    if (connected_) {
        // Return to STA-only mode
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

esp_err_t WifiManager::EnsureApNetif()
{
    if (ap_netif_) {
        return ESP_OK;
    }

    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (!ap_netif_) {
        ESP_LOGE(TAG, "Failed to create default WiFi AP netif");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created default WiFi AP netif");
    return ESP_OK;
}

// ── RSSI Monitoring & Roaming ───────────────────────────────────────────────

void WifiManager::StartRssiMonitor()
{
    if (rssi_timer_ || networks_.size() < 2) return;

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = RssiTimerCallback;
    timer_args.arg = this;
    timer_args.name = "rssi_mon";

    if (esp_timer_create(&timer_args, &rssi_timer_) == ESP_OK) {
        esp_timer_start_periodic(rssi_timer_, RSSI_CHECK_INTERVAL_S * 1000000ULL);
        ESP_LOGI(TAG, "RSSI monitor started (interval=%ds, threshold=%ddBm)",
                 RSSI_CHECK_INTERVAL_S, RSSI_ROAM_THRESHOLD);
    }
}

void WifiManager::StopRssiMonitor()
{
    if (rssi_timer_) {
        esp_timer_stop(rssi_timer_);
        esp_timer_delete(rssi_timer_);
        rssi_timer_ = nullptr;
    }
    rssi_bad_count_ = 0;
}

void WifiManager::RssiTimerCallback(void* arg)
{
    auto* self = static_cast<WifiManager*>(arg);
    self->CheckRssiAndRoam();
}

void WifiManager::CheckRssiAndRoam()
{
    if (!connected_ || scanning_ || provisioning_) return;
    if (networks_.size() < 2) return;

    int8_t rssi = GetRssi();
    if (rssi < RSSI_ROAM_THRESHOLD) {
        rssi_bad_count_++;
        ESP_LOGW(TAG, "RSSI=%d dBm (bad count: %d/%d)", rssi, rssi_bad_count_, RSSI_BAD_COUNT_TRIGGER);
        if (rssi_bad_count_ >= RSSI_BAD_COUNT_TRIGGER) {
            ESP_LOGW(TAG, "Signal weak, scanning for better network...");
            rssi_bad_count_ = 0;
            ScanAndRoam();
        }
    } else {
        rssi_bad_count_ = 0;
    }
}

void WifiManager::ScanAndRoam()
{
    if (scanning_) return;
    scanning_ = true;

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);  // non-blocking
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        scanning_ = false;
    }
}

// ── Connect / Disconnect ────────────────────────────────────────────────────

esp_err_t WifiManager::Connect()
{
    if (provisioning_ && !connected_) {
        ESP_LOGW(TAG, "In provisioning mode, cannot connect as STA");
        return ESP_ERR_INVALID_STATE;
    }
    retry_count_ = 0;
    sta_fail_count_ = 0;
    return esp_wifi_start();
}

void WifiManager::Disconnect()
{
    StopRssiMonitor();
    esp_wifi_disconnect();
    esp_wifi_stop();
}

int8_t WifiManager::GetRssi() const
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -127;
}

std::string WifiManager::GetIpAddress() const
{
    return current_ip_;
}

void WifiManager::OnStatusChange(StatusCallback callback)
{
    status_callback_ = std::move(callback);
}

void WifiManager::OnProvisioningStart(ProvisioningCallback callback)
{
    provisioning_callback_ = std::move(callback);
}

void WifiManager::TryNextNetwork()
{
    if (networks_.empty()) return;

    connect_attempt_idx_ = (connect_attempt_idx_ + 1) % (int)networks_.size();
    auto& net = networks_[connect_attempt_idx_];

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, net.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, net.password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_LOGI(TAG, "Trying network %d/%d: %s", connect_attempt_idx_ + 1,
             (int)networks_.size(), net.ssid.c_str());
    esp_wifi_connect();
}

// ── Event Handlers ──────────────────────────────────────────────────────────

void WifiManager::WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<WifiManager*>(arg);

    if (id == WIFI_EVENT_STA_START) {
        if (self->provisioning_) {
            ESP_LOGI(TAG, "Ignoring STA start while provisioning");
            return;
        }
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started: SSID='%s'", self->ap_ssid_.c_str());
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->provisioning_) {
            ESP_LOGI(TAG, "Ignoring STA disconnect while provisioning");
            return;
        }
        self->HandleDisconnect();
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)data;
        ESP_LOGI(TAG, "Station connected to AP: " MACSTR " (AID=%d)",
                 MAC2STR(event->mac), event->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)data;
        ESP_LOGI(TAG, "Station disconnected from AP: " MACSTR " (AID=%d)",
                 MAC2STR(event->mac), event->aid);
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        // Handle scan results for roaming
        if (!self->scanning_) return;
        self->scanning_ = false;

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count == 0) return;

        std::vector<wifi_ap_record_t> ap_records(ap_count);
        esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());

        // Find best matching configured network
        int8_t current_rssi = self->GetRssi();
        int best_idx = -1;
        int8_t best_rssi = -127;

        for (int i = 0; i < (int)self->networks_.size(); i++) {
            for (auto& ap : ap_records) {
                if (self->networks_[i].ssid == (char*)ap.ssid) {
                    if (ap.rssi > best_rssi) {
                        best_rssi = ap.rssi;
                        best_idx = i;
                    }
                }
            }
        }

        if (best_idx >= 0 && best_idx != self->current_network_idx_ &&
            best_rssi > current_rssi + RSSI_ROAM_HYSTERESIS) {
            ESP_LOGI(TAG, "Roaming: %s (RSSI=%d) → %s (RSSI=%d)",
                     self->networks_[self->current_network_idx_].ssid.c_str(), current_rssi,
                     self->networks_[best_idx].ssid.c_str(), best_rssi);

            self->current_network_idx_ = best_idx;
            self->connect_attempt_idx_ = best_idx;
            auto& net = self->networks_[best_idx];

            wifi_config_t wifi_config = {};
            strncpy((char*)wifi_config.sta.ssid, net.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char*)wifi_config.sta.password, net.password.c_str(), sizeof(wifi_config.sta.password) - 1);
            wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
            wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            esp_wifi_connect();
        } else {
            ESP_LOGD(TAG, "No better network found (current=%d, best=%d, delta=%d)",
                     current_rssi, best_rssi, best_rssi - current_rssi);
        }
    }
}

void WifiManager::IpEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<WifiManager*>(arg);
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(data);

        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        self->current_ip_ = ip_str;

        ESP_LOGI(TAG, "Got STA IP: %s", ip_str);
        self->HandleConnect();
    } else if (id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t* event = static_cast<ip_event_ap_staipassigned_t*>(data);
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip));
        ESP_LOGI(TAG, "Assigned IP %s to AP client " MACSTR,
                 ip_str, MAC2STR(event->mac));
    }
}

void WifiManager::HandleConnect()
{
    connected_ = true;
    ever_connected_ = true;
    retry_count_ = 0;
    sta_fail_count_ = 0;
    rssi_bad_count_ = 0;

    esp_wifi_set_ps(WIFI_PS_NONE);

    // Identify which network we connected to
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        for (int i = 0; i < (int)networks_.size(); i++) {
            if (networks_[i].ssid == (char*)ap_info.ssid) {
                current_network_idx_ = i;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Connected to '%s' (RSSI=%d, IP=%s, PS=off)",
             networks_.empty() ? "?" : networks_[current_network_idx_].ssid.c_str(),
             GetRssi(), current_ip_.c_str());

    if (status_callback_) {
        status_callback_(true);
    }

    StartRssiMonitor();
    StartTimeSync();
}

void WifiManager::HandleDisconnect()
{
    connected_ = false;
    StopRssiMonitor();

    if (status_callback_) {
        status_callback_(false);
    }

    sta_fail_count_++;

    // Initial boot: fast fail (2 retries per network, ~5s total)
    // Runtime reconnect: patient retry (5 retries with exponential backoff)
    int max_retry = ever_connected_ ? MAX_RETRY : 2;

    // Try cycling through all networks before entering provisioning
    int total_fails = sta_fail_count_;
    int networks_tried = total_fails / max_retry;

    if (networks_tried >= (int)networks_.size() && !provisioning_) {
        ESP_LOGW(TAG, "All %d networks failed, rebooting into provisioning mode", (int)networks_.size());
        SetForceApBootFlag(true);
        esp_wifi_stop();
        // Defer restart via one-shot timer to avoid restarting directly from sys_evt task.
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void* arg) {
            esp_restart();
        };
        timer_args.arg = this;
        timer_args.name = "prov_reboot";
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, 10000);  // 10ms delay
        }
        return;
    }

    // Try next network after max retries on current one
    if (retry_count_ >= max_retry && networks_.size() > 1) {
        retry_count_ = 0;
        TryNextNetwork();
        return;
    }

    ScheduleReconnect();
}

void WifiManager::ScheduleReconnect()
{
    int max_retry = ever_connected_ ? MAX_RETRY : 2;

    if (retry_count_ < max_retry) {
        int delay;
        if (ever_connected_) {
            // Runtime reconnect: exponential backoff
            delay = BASE_RETRY_DELAY_MS * (1 << retry_count_);
            if (delay > 30000) delay = 30000;
        } else {
            // Initial boot: fixed 1s delay for fast fail
            delay = 1000;
        }
        retry_count_++;
        ESP_LOGW(TAG, "Reconnecting in %d ms (attempt %d/%d, network: %s)",
                 delay, retry_count_, max_retry,
                 networks_.empty() ? "?" : networks_[connect_attempt_idx_].ssid.c_str());
        vTaskDelay(pdMS_TO_TICKS(delay));
        esp_wifi_connect();
    } else {
        ESP_LOGE(TAG, "Max retries on current network, trying next...");
        retry_count_ = 0;
        TryNextNetwork();
    }
}

// ── HTTP Handlers ───────────────────────────────────────────────────────────

esp_err_t WifiManager::HttpGetHandler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "HTTP GET /  (root handler)");
    auto* self = static_cast<WifiManager*>(req->user_ctx);

    // Build dynamic config page
    SetPortalResponseHeaders(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, CONFIG_HTML_HEADER);

    // Status bar
    if (self->connected_) {
        char status[256];
        snprintf(status, sizeof(status),
            "<div class='status ok'>&#9989; Connected to <b>%s</b> (RSSI: %d dBm) | IP: %s</div>",
            self->networks_.empty() ? "?" : self->networks_[self->current_network_idx_].ssid.c_str(),
            self->GetRssi(), self->current_ip_.c_str());
        httpd_resp_sendstr_chunk(req, status);
    } else {
        httpd_resp_sendstr_chunk(req, "<div class='status warn'>&#9888; Not connected</div>");
    }

    // Saved networks
    httpd_resp_sendstr_chunk(req, "<div class='card'><h3>Saved Networks (stored on device)</h3>");
    if (self->networks_.empty()) {
        httpd_resp_sendstr_chunk(req, "<p>No networks configured</p>");
    } else {
        for (int i = 0; i < (int)self->networks_.size(); i++) {
            std::string ssid_html = html_escape(self->networks_[i].ssid);
            char net_html[512];
            snprintf(net_html, sizeof(net_html),
                "<div class='net'><span><b>%d.</b> %s %s</span>"
                "<div class='actions'>"
                "<form method='POST' action='/save' style='margin:0'>"
                "<input type='hidden' name='ssid' value='%s'>"
                "<button class='btn-use' type='submit'>Use</button>"
                "</form>"
                "<form method='POST' action='/delete' style='margin:0'>"
                "<input type='hidden' name='idx' value='%d'>"
                "<button class='btn-del' type='submit'>Delete</button>"
                "</form></div></div>",
                i + 1, ssid_html.c_str(),
                (i == self->current_network_idx_ && self->connected_) ? "<span class='rssi'>(active)</span>" : "",
                ssid_html.c_str(),
                i);
            httpd_resp_sendstr_chunk(req, net_html);
        }
    }
    httpd_resp_sendstr_chunk(req, "</div>");

    // Add/update network form with scan. Leaving password blank reuses a saved
    // password when the SSID already exists.
    {
        httpd_resp_sendstr_chunk(req,
            "<div class='card'><h3>Add / Update Network</h3>"
            "<div id='scan-area'>"
            "<button class='btn-add' onclick='doScan()' style='background:#2196F3;margin-bottom:12px'>&#128269; Scan WiFi Networks</button>"
            "<div id='scan-results'></div>"
            "</div>"
            "<form method='POST' action='/save' id='add-form'>"
            "<input name='ssid' id='ssid-input' placeholder='WiFi Name (SSID)' required>"
            "<input name='pass' id='pass-input' type='password' placeholder='Password (blank uses saved password)'>"
            "<br><button class='btn-add' type='submit'>Save / Connect</button>"
            "</form>"
            "<script>"
            "function doScan(){"
            "document.getElementById('scan-results').innerHTML='<p>Scanning...</p>';"
            "fetch('/scan').then(r=>r.json()).then(d=>{"
            "let h='';"
            "d.forEach(n=>{"
            "h+='<div class=\"net\" style=\"cursor:pointer\" onclick=\"pickNet(\\''+n.ssid+'\\')\"><span>'+n.ssid+'</span><span class=\"rssi\">'+n.rssi+' dBm</span></div>';"
            "});"
            "if(!d.length)h='<p>No networks found</p>';"
            "document.getElementById('scan-results').innerHTML=h;"
            "}).catch(e=>{document.getElementById('scan-results').innerHTML='<p>Scan failed</p>';});"
            "}"
            "function pickNet(s){document.getElementById('ssid-input').value=s;document.getElementById('pass-input').focus();}"
            "</script>"
            "</div>");
    }

    httpd_resp_sendstr_chunk(req, CONFIG_HTML_FOOTER);
    httpd_resp_sendstr_chunk(req, NULL);  // end chunked
    return ESP_OK;
}

esp_err_t WifiManager::HttpPostHandler(httpd_req_t* req)
{
    auto* self = static_cast<WifiManager*>(req->user_ctx);

    char buf[256] = {};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    std::string body(buf, received);

    // Handle save network
    std::string ssid = parse_form_field(body, "ssid");
    std::string pass = parse_form_field(body, "pass");

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    self->AddNetwork(ssid, pass);

    if (self->provisioning_ && !self->connected_) {
        // First-time setup: show success page, user presses RST or restart button
        SetPortalResponseHeaders(req);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PROVISION_OK_HTML, strlen(PROVISION_OK_HTML));
        ESP_LOGI(TAG, "First network saved, waiting for user to restart");
    } else {
        // Already connected — just redirect back
        SetPortalResponseHeaders(req);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t WifiManager::HttpDeleteHandler(httpd_req_t* req)
{
    auto* self = static_cast<WifiManager*>(req->user_ctx);

    char buf[64] = {};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    std::string body(buf, received);
    std::string idx_str = parse_form_field(body, "idx");

    if (!idx_str.empty()) {
        int idx = atoi(idx_str.c_str());
        if (self->RemoveNetwork(idx)) {
            ESP_LOGI(TAG, "Deleted network at index %d", idx);
        }
    }

    SetPortalResponseHeaders(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t WifiManager::HttpScanHandler(httpd_req_t* req)
{
    // Trigger a synchronous WiFi scan
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // blocking scan
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed from portal: %s", esp_err_to_name(err));
        SetPortalResponseHeaders(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;  // limit results

    std::vector<wifi_ap_record_t> ap_records(ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());

    // Sort by RSSI (strongest first)
    std::sort(ap_records.begin(), ap_records.end(),
        [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
            return a.rssi > b.rssi;
        });

    // Build JSON response, dedup by SSID
    SetPortalResponseHeaders(req);
    httpd_resp_set_type(req, "application/json");
    std::string json = "[";
    std::vector<std::string> seen;

    for (auto& ap : ap_records) {
        std::string ssid((char*)ap.ssid);
        if (ssid.empty()) continue;
        // Dedup
        bool dup = false;
        for (auto& s : seen) { if (s == ssid) { dup = true; break; } }
        if (dup) continue;
        seen.push_back(ssid);

        if (json.size() > 1) json += ",";
        // Escape quotes in SSID
        std::string escaped;
        for (char c : ssid) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        char entry[128];
        snprintf(entry, sizeof(entry),
            "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
            escaped.c_str(), ap.rssi, ap.authmode);
        json += entry;
    }
    json += "]";

    httpd_resp_sendstr(req, json.c_str());
    ESP_LOGI(TAG, "Scan returned %d unique networks", (int)seen.size());
    return ESP_OK;
}

// ── Time Sync ───────────────────────────────────────────────────────────────

esp_err_t WifiManager::StartTimeSync()
{
    ESP_LOGI(TAG, "Starting SNTP time sync...");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = TimeSyncCallback;
    esp_netif_sntp_init(&config);

    return ESP_OK;
}

void WifiManager::TimeSyncCallback(struct timeval* tv)
{
    ESP_LOGI(TAG, "NTP time synced");
    WifiManager::GetInstance().time_synced_ = true;
}

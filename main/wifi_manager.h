#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <string>
#include <vector>

static constexpr int WIFI_MAX_NETWORKS = 3;
static constexpr int RSSI_ROAM_THRESHOLD = -75;   // dBm — trigger scan below this
static constexpr int RSSI_ROAM_HYSTERESIS = 10;    // dB — alternative must be this much better
static constexpr int RSSI_CHECK_INTERVAL_S = 10;   // seconds between RSSI checks
static constexpr int RSSI_BAD_COUNT_TRIGGER = 3;   // consecutive bad readings before roam

struct WifiNetwork {
    std::string ssid;
    std::string password;
};

class WifiManager {
public:
    using StatusCallback = std::function<void(bool connected)>;
    using ProvisioningCallback = std::function<void()>;

    static WifiManager& GetInstance();

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

    // Initialize WiFi subsystem. Checks NVS for stored credentials.
    esp_err_t Init();

    // Initialize with explicit credentials (legacy, also saves to NVS as slot 0)
    esp_err_t Init(const std::string& ssid, const std::string& password);

    // Connect as STA using stored/provided credentials
    esp_err_t Connect();
    void Disconnect();
    bool IsConnected() const { return connected_; }
    int8_t GetRssi() const;
    std::string GetIpAddress() const;

    // True if device is in AP provisioning/config mode
    bool IsProvisioning() const { return provisioning_; }

    // Check if any credentials are stored in NVS
    bool HasStoredCredentials() const;

    // Multi-SSID management
    const std::vector<WifiNetwork>& GetNetworks() const { return networks_; }
    bool AddNetwork(const std::string& ssid, const std::string& password);
    bool RemoveNetwork(int index);
    int GetNetworkCount() const { return (int)networks_.size(); }

    // Enter WiFi config mode (STA+AP or AP-only depending on connection state)
    esp_err_t StartConfigMode();
    void StopConfigMode();
    const std::string& GetApSsid() const { return ap_ssid_; }

    void OnStatusChange(StatusCallback callback);
    void OnProvisioningStart(ProvisioningCallback callback);

    // Start SNTP time sync
    esp_err_t StartTimeSync();
    bool IsTimeSynced() const { return time_synced_; }

private:
    WifiManager() = default;
    ~WifiManager() = default;

    esp_err_t InitSta();
    esp_err_t StartProvisioning();
    void StopProvisioning();
    esp_err_t EnsureApNetif();

    // Multi-SSID NVS management
    bool LoadAllNetworks();
    void SaveAllNetworks();

    // Roaming
    void StartRssiMonitor();
    void StopRssiMonitor();
    static void RssiTimerCallback(void* arg);
    void CheckRssiAndRoam();
    void ScanAndRoam();
    void TryNextNetwork();

    static void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void IpEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void TimeSyncCallback(struct timeval* tv);

    // HTTP server handlers
    static esp_err_t HttpGetHandler(httpd_req_t* req);
    static esp_err_t HttpPostHandler(httpd_req_t* req);
    static esp_err_t HttpDeleteHandler(httpd_req_t* req);
    static esp_err_t HttpScanHandler(httpd_req_t* req);
    static esp_err_t HttpCaptiveRedirect(httpd_req_t* req, httpd_err_code_t err);
    static esp_err_t HttpRestartHandler(httpd_req_t* req);

    // DNS captive portal (resolves all queries to AP IP)
    void StartDnsServer();
    void StopDnsServer();
    static void DnsServerTask(void* arg);

    void HandleConnect();
    void HandleDisconnect();
    void ScheduleReconnect();

    // Multi-SSID storage
    std::vector<WifiNetwork> networks_;
    int current_network_idx_ = 0;      // index of currently connected network
    int connect_attempt_idx_ = 0;      // index being attempted during reconnect

    bool connected_ = false;
    bool ever_connected_ = false;   // true after first successful connection (never reset)
    bool time_synced_ = false;
    bool provisioning_ = false;
    bool initialized_ = false;
    bool scanning_ = false;
    int retry_count_ = 0;
    int sta_fail_count_ = 0;
    int rssi_bad_count_ = 0;

    httpd_handle_t http_server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    esp_timer_handle_t rssi_timer_ = nullptr;

    std::string ap_ssid_;              // AP SSID (HiLily-XXYY), set on provisioning/config
    std::string current_ip_;

    StatusCallback status_callback_;
    ProvisioningCallback provisioning_callback_;

    // DNS captive portal
    TaskHandle_t dns_task_ = nullptr;
    int dns_socket_ = -1;
};

#endif // WIFI_MANAGER_H

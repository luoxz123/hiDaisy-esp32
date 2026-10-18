/*
 * QEMU mock WiFi manager.
 * Simulates WiFi as always connected with virtual Ethernet.
 */
#include "wifi_manager.h"
#include <esp_log.h>
#include <cstring>

static const char *TAG = "wifi_qemu";

WifiManager& WifiManager::GetInstance() {
    static WifiManager instance;
    return instance;
}

esp_err_t WifiManager::Init() {
    ESP_LOGI(TAG, "[QEMU] WiFi mock init (always connected)");
    initialized_ = true;
    ssid_ = "QEMU-Virtual";
    password_ = "";
    return ESP_OK;
}

esp_err_t WifiManager::Init(const std::string& ssid, const std::string& password) {
    ssid_ = ssid;
    password_ = password;
    return Init();
}

esp_err_t WifiManager::Connect() {
    ESP_LOGI(TAG, "[QEMU] WiFi mock connect (instant success)");
    connected_ = true;
    if (status_callback_) {
        status_callback_(true);
    }
    // Auto-trigger time sync
    StartTimeSync();
    return ESP_OK;
}

void WifiManager::Disconnect() {
    ESP_LOGI(TAG, "[QEMU] WiFi mock disconnect");
    connected_ = false;
    if (status_callback_) {
        status_callback_(false);
    }
}

bool WifiManager::HasStoredCredentials() const {
    return true; // Always has "credentials" in QEMU mode
}

int8_t WifiManager::GetRssi() const {
    return -45; // Good signal
}

void WifiManager::OnStatusChange(StatusCallback callback) {
    status_callback_ = std::move(callback);
}

esp_err_t WifiManager::StartTimeSync() {
    ESP_LOGI(TAG, "[QEMU] Time sync mock (using host time)");
    time_synced_ = true;
    return ESP_OK;
}

// Stubs for internal methods (not used in mock)
esp_err_t WifiManager::InitSta() { return ESP_OK; }
esp_err_t WifiManager::StartProvisioning() { return ESP_OK; }
void WifiManager::StopProvisioning() {}
bool WifiManager::LoadCredentials() { return true; }
void WifiManager::SaveCredentials(const std::string&, const std::string&) {}
void WifiManager::WifiEventHandler(void*, esp_event_base_t, int32_t, void*) {}
void WifiManager::IpEventHandler(void*, esp_event_base_t, int32_t, void*) {}
void WifiManager::TimeSyncCallback(struct timeval*) {}
esp_err_t WifiManager::HttpGetHandler(httpd_req_t*) { return ESP_OK; }
esp_err_t WifiManager::HttpPostHandler(httpd_req_t*) { return ESP_OK; }
void WifiManager::HandleConnect() {}
void WifiManager::HandleDisconnect() {}
void WifiManager::ScheduleReconnect() {}

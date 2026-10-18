#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <esp_err.h>
#include <esp_timer.h>
#include <string>

class OtaManager {
public:
    static OtaManager& GetInstance();

    OtaManager(const OtaManager&) = delete;
    OtaManager& operator=(const OtaManager&) = delete;

    // Mark current firmware as valid (call after successful boot)
    void MarkValid();

    // Get current firmware version
    const char* GetVersion() const;

    // Set server base URL for OTA check (e.g. "http://4.197.40.220:8000")
    void SetServerUrl(const std::string& url);

    // Set device ID for OTA check
    void SetDeviceId(const std::string& id);

    // Check server for firmware update; if available, download and install
    // Returns ESP_OK if no update or update succeeded (device will restart)
    esp_err_t CheckForUpdate();

    // Start OTA download from URL with retry and progress display
    esp_err_t StartUpdate(const std::string& url, const std::string& version = "");

    // Start periodic version check timer (every hour)
    void StartPeriodicCheck();

    // Check if OTA is in progress
    bool IsUpdating() const { return updating_; }

    // Compare two semantic version strings: returns >0 if a>b, 0 if equal, <0 if a<b
    static int CompareVersions(const char* a, const char* b);

    static constexpr int MAX_RETRY = 3;
    static constexpr int CHECK_INTERVAL_MS = 3600000; // 1 hour

private:
    OtaManager() = default;
    ~OtaManager() = default;

    static void PeriodicCheckCallback(void* arg);
    esp_err_t DoDownload(const std::string& url, const char* ver);

    volatile bool updating_ = false;
    std::string server_url_;
    std::string device_id_;
    esp_timer_handle_t check_timer_ = nullptr;
};

#endif // OTA_MANAGER_H

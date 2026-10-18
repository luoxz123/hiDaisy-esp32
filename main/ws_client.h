#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <esp_err.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <functional>
#include <string>

struct AuthResult {
    std::string ws_url;
    std::string token;
    int expires_in = 0;
};

class WsClient {
public:
    using JsonCallback = std::function<void(const char* json_str, int len)>;
    using BinaryCallback = std::function<void(const uint8_t* data, int len)>;
    using StatusCallback = std::function<void(bool connected)>;

    static WsClient& GetInstance();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // HTTP auth to get token and ws_url
    esp_err_t Authenticate(const std::string& server_url, const std::string& device_id,
                           const std::string& secret_key, const std::string& firmware_version,
                           uint8_t battery_level);

    esp_err_t Connect();
    void Disconnect();
    bool IsConnected() const { return connected_; }

    // Send text JSON frame
    esp_err_t SendJson(const char* json_str);
    // Send binary audio frame
    esp_err_t SendBinary(const uint8_t* data, int len);

    void OnJson(JsonCallback callback) { json_callback_ = std::move(callback); }
    void OnBinary(BinaryCallback callback) { binary_callback_ = std::move(callback); }
    void OnStatus(StatusCallback callback) { status_callback_ = std::move(callback); }

    // Start heartbeat timer
    void StartHeartbeat();
    void StopHeartbeat();

private:
    WsClient() = default;
    ~WsClient();

    static void EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void HeartbeatTimerCallback(void* arg);

    AuthResult auth_;
    esp_websocket_client_handle_t client_ = NULL;
    esp_timer_handle_t heartbeat_timer_ = NULL;
    bool connected_ = false;

    JsonCallback json_callback_;
    BinaryCallback binary_callback_;
    StatusCallback status_callback_;
};

#endif // WS_CLIENT_H

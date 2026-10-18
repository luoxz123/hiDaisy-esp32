/*
 * QEMU mock UDP client.
 * Discards sent audio, does not receive. For testing state machine flow.
 */
#include "udp_client.h"
#include <esp_log.h>

static const char *TAG = "udp_qemu";

UdpClient &UdpClient::GetInstance() {
    static UdpClient instance;
    return instance;
}

UdpClient::~UdpClient() {}

esp_err_t UdpClient::Connect(const std::string &server_ip, uint16_t port,
                              const std::string &key_hex,
                              const std::string &nonce_hex) {
    ESP_LOGI(TAG, "[QEMU] UDP mock connect (%s:%u)", server_ip.c_str(), port);
    sock_ = 999; // Fake socket
    return ESP_OK;
}

void UdpClient::Disconnect() {
    ESP_LOGI(TAG, "[QEMU] UDP mock disconnect");
    sock_ = -1;
}

esp_err_t UdpClient::Send(const uint8_t *opus_data, int len, uint32_t timestamp) {
    // Silently discard
    return ESP_OK;
}

void UdpClient::StartReceiveTask() {
    ESP_LOGI(TAG, "[QEMU] UDP receive task mock (no-op)");
}

void UdpClient::ReceiveTask(void *arg) {}

bool UdpClient::HexToBytes(const std::string &hex, uint8_t *out, size_t expected_len) {
    return true;
}

bool UdpClient::AesCtrCrypt(const uint8_t *nonce, const uint8_t *in, uint8_t *out, int len) {
    return true;
}

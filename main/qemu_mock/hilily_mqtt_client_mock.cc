/*
 * QEMU mock MQTT client.
 * Simulates successful auth and MQTT connection without real networking.
 * JSON messages are logged but not sent.
 */
#include "hilily_mqtt_client.h"
#include <esp_log.h>

static const char *TAG = "mqtt_qemu";

HililyMqttClient &HililyMqttClient::GetInstance() {
    static HililyMqttClient instance;
    return instance;
}

HililyMqttClient::~HililyMqttClient() {}

esp_err_t HililyMqttClient::Authenticate(const std::string &server_url,
                                          const std::string &device_id,
                                          const std::string &secret_key,
                                          const std::string &firmware_version,
                                          uint8_t battery_level) {
    ESP_LOGI(TAG, "[QEMU] Auth mock (device=%s, fw=%s, bat=%d)",
             device_id.c_str(), firmware_version.c_str(), battery_level);

    // Fill with mock values
    auth_.mqtt_broker = "mqtt://localhost:1883";
    auth_.mqtt_client_id = "qemu-" + device_id;
    auth_.mqtt_user = "qemu_user";
    auth_.mqtt_password = "qemu_pass";
    auth_.publish_topic = "device/" + device_id + "/up";
    auth_.subscribe_topic = "device/" + device_id + "/down";
    auth_.expires_in = 3600;

    return ESP_OK;
}

esp_err_t HililyMqttClient::Connect() {
    ESP_LOGI(TAG, "[QEMU] MQTT mock connect (instant success)");
    connected_ = true;
    if (status_callback_) {
        status_callback_(true);
    }
    return ESP_OK;
}

void HililyMqttClient::Disconnect() {
    ESP_LOGI(TAG, "[QEMU] MQTT mock disconnect");
    connected_ = false;
    if (status_callback_) {
        status_callback_(false);
    }
}

esp_err_t HililyMqttClient::PublishJson(const char *json_str) {
    ESP_LOGD(TAG, "[QEMU] Publish: %s", json_str);
    return ESP_OK;
}

void HililyMqttClient::EventHandler(void *, esp_event_base_t, int32_t, void *) {}

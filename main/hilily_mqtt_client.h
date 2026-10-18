#ifndef HILILY_MQTT_CLIENT_H
#define HILILY_MQTT_CLIENT_H

#include <esp_err.h>
#include <esp_timer.h>
#include <functional>
#include <mqtt_client.h> // ESP-IDF built-in MQTT component
#include <string>

/**
 * 设备注册响应结构体
 * 对应服务器 POST /api/v1/device/register 的 JSON 响应
 */
struct DeviceRegistration {
  std::string activation_code;
  int expires_in = 86400;
};

/**
 * 激活状态查询响应结构体
 * 对应服务器 GET /api/v1/device/register/status 的 JSON 响应
 */
struct ActivationStatus {
  std::string status;       // "pending" or "active"
  std::string device_token; // non-empty when active
};

/**
 * 认证响应结构体 (新 MQTT+UDP 架构)
 * 对应服务器 /api/v1/device/auth 的 JSON 响应
 */
struct MqttAuthResult {
  std::string mqtt_broker;    // e.g. "mqtt://broker.hilily.com:1883"
  std::string mqtt_client_id; // e.g. "device-dcb4d9243ef0"
  std::string mqtt_user;
  std::string mqtt_password;
  std::string publish_topic;   // device/{mac}/up
  std::string subscribe_topic; // device/{mac}/down
  int expires_in = 300;
  std::string device_token;    // 首次激活时服务端下发的 token (非空=需持久化到 NVS)
};

class HililyMqttClient {
public:
  using JsonCallback = std::function<void(const char *json_str, int len)>;
  using StatusCallback = std::function<void(bool connected)>;

  static HililyMqttClient &GetInstance();

  HililyMqttClient(const HililyMqttClient &) = delete;
  HililyMqttClient &operator=(const HililyMqttClient &) = delete;

  /**
   * 设备注册: POST /api/v1/device/register
   * 返回激活码和过期时间; 409 已注册返回 ESP_ERR_INVALID_STATE
   */
  esp_err_t RegisterDevice(const std::string &server_url,
                           const std::string &mac_address,
                           DeviceRegistration &out);

  /**
   * 查询激活状态: GET /api/v1/device/register/status?mac=...
   * 返回 pending 或 active (附 device_token)
   */
  esp_err_t CheckActivationStatus(const std::string &server_url,
                                  const std::string &mac_address,
                                  ActivationStatus &out);

  /**
   * HTTP Auth: 从服务端获取 MQTT 连接凭证
   * 与旧 WsClient::Authenticate 接口签名一致，内部解析新响应格式
   */
  esp_err_t Authenticate(const std::string &server_url,
                         const std::string &device_id,
                         const std::string &secret_key,
                         const std::string &firmware_version,
                         uint8_t battery_level);

  /** 连接 MQTT Broker (使用 auth_ 中的凭证) */
  esp_err_t Connect();

  /** 断开连接并清理 */
  void Disconnect();

  bool IsConnected() const { return connected_; }

  /**
   * 向服务器发布 JSON 控制消息
   * 使用 auth_.publish_topic (device/{mac}/up)
   */
  esp_err_t PublishJson(const char *json_str);

  /**
   * 注册 JSON 消息回调 (收到服务器 device/{mac}/down 消息时触发)
   */
  void OnJson(JsonCallback cb) { json_callback_ = std::move(cb); }

  /**
   * 注册连接状态变化回调
   */
  void OnStatus(StatusCallback cb) { status_callback_ = std::move(cb); }

  /** 访问认证结果 */
  const MqttAuthResult &GetAuth() const { return auth_; }

private:
  HililyMqttClient() = default;
  ~HililyMqttClient();

  static void EventHandler(void *arg, esp_event_base_t base, int32_t event_id,
                           void *event_data);

  MqttAuthResult auth_;
  esp_mqtt_client_handle_t client_ = nullptr;
  bool connected_ = false;

  JsonCallback json_callback_;
  StatusCallback status_callback_;
};

#endif // HILILY_MQTT_CLIENT_H

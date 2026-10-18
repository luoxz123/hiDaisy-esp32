#include "hilily_mqtt_client.h"
#include "battery.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "mqtt_client";

HililyMqttClient &HililyMqttClient::GetInstance() {
  static HililyMqttClient instance;
  return instance;
}

HililyMqttClient::~HililyMqttClient() { Disconnect(); }

// HTTP event handler to accumulate response body
static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  auto *buf = static_cast<std::string *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
    buf->append(static_cast<const char *>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

// -----------------------------------------------------------------------
// Device Registration
// -----------------------------------------------------------------------

esp_err_t HililyMqttClient::RegisterDevice(const std::string &server_url,
                                           const std::string &mac_address,
                                           DeviceRegistration &out) {
  // Lowercase MAC
  std::string mac_lower = mac_address;
  for (auto &c : mac_lower)
    c = tolower(static_cast<unsigned char>(c));

  // Build JSON body
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "mac_address", mac_lower.c_str());
  char *body_str = cJSON_PrintUnformatted(body);
  int body_len = strlen(body_str);

  // Build full URL
  std::string full_url = server_url;
  if (!full_url.empty() && full_url.back() == '/')
    full_url.pop_back();
  full_url += "/api/v1/device/register";

  ESP_LOGI(TAG, "RegisterDevice endpoint: %s", full_url.c_str());

  std::string response_body;
  esp_http_client_config_t config = {};
  config.url = full_url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = _http_event_handler;
  config.user_data = &response_body;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    cJSON_free(body_str);
    cJSON_Delete(body);
    return ESP_FAIL;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body_str, body_len);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);

  cJSON_free(body_str);
  cJSON_Delete(body);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  esp_http_client_cleanup(client);

  ESP_LOGI(TAG, "HTTP status: %d, body: %d bytes", status, (int)response_body.size());

  if (status == 409) {
    ESP_LOGW(TAG, "Device already registered");
    return ESP_ERR_INVALID_STATE;
  }

  if (status != 200) {
    ESP_LOGE(TAG, "RegisterDevice failed: status=%d body=%.200s", status, response_body.c_str());
    return ESP_FAIL;
  }

  if (response_body.empty()) {
    ESP_LOGE(TAG, "Empty response body");
    return ESP_FAIL;
  }

  // Parse JSON response
  cJSON *resp = cJSON_Parse(response_body.c_str());
  if (!resp) {
    ESP_LOGE(TAG, "RegisterDevice response parse failed: %.200s", response_body.c_str());
    return ESP_FAIL;
  }

  cJSON *code_item = cJSON_GetObjectItem(resp, "activation_code");
  const char *code_str = code_item ? cJSON_GetStringValue(code_item) : nullptr;
  out.activation_code = code_str ? std::string(code_str) : std::string();

  cJSON *exp = cJSON_GetObjectItem(resp, "expires_in");
  out.expires_in = exp ? exp->valueint : 86400;

  cJSON_Delete(resp);

  ESP_LOGI(TAG, "RegisterDevice OK — activation_code: %s, expires_in: %d",
           out.activation_code.c_str(), out.expires_in);
  return ESP_OK;
}

// -----------------------------------------------------------------------
// Check Activation Status
// -----------------------------------------------------------------------

esp_err_t HililyMqttClient::CheckActivationStatus(const std::string &server_url,
                                                   const std::string &mac_address,
                                                   ActivationStatus &out) {
  // Lowercase MAC
  std::string mac_lower = mac_address;
  for (auto &c : mac_lower)
    c = tolower(static_cast<unsigned char>(c));

  // Build full URL with query parameter
  std::string full_url = server_url;
  if (!full_url.empty() && full_url.back() == '/')
    full_url.pop_back();
  full_url += "/api/v1/device/register/status?mac=";
  full_url += mac_lower;

  ESP_LOGI(TAG, "CheckActivationStatus endpoint: %s", full_url.c_str());

  std::string response_body;
  esp_http_client_config_t config = {};
  config.url = full_url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = _http_event_handler;
  config.user_data = &response_body;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    return ESP_FAIL;
  }

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  esp_http_client_cleanup(client);

  ESP_LOGI(TAG, "HTTP status: %d, body: %d bytes", status, (int)response_body.size());

  if (status != 200) {
    ESP_LOGE(TAG, "CheckActivationStatus failed: status=%d body=%.200s", status, response_body.c_str());
    return ESP_FAIL;
  }

  if (response_body.empty()) {
    ESP_LOGE(TAG, "Empty response body");
    return ESP_FAIL;
  }

  // Parse JSON response
  cJSON *resp = cJSON_Parse(response_body.c_str());
  if (!resp) {
    ESP_LOGE(TAG, "CheckActivationStatus response parse failed: %.200s", response_body.c_str());
    return ESP_FAIL;
  }

  auto get_str = [&](const char *key) -> std::string {
    cJSON *item = cJSON_GetObjectItem(resp, key);
    const char *s = item ? cJSON_GetStringValue(item) : nullptr;
    return s ? std::string(s) : std::string();
  };

  out.status = get_str("status");
  out.device_token = get_str("device_token");

  cJSON_Delete(resp);

  ESP_LOGI(TAG, "CheckActivationStatus OK — status: %s", out.status.c_str());
  return ESP_OK;
}

// -----------------------------------------------------------------------
// HTTP Auth
// -----------------------------------------------------------------------

esp_err_t HililyMqttClient::Authenticate(const std::string &server_url,
                                         const std::string &device_id,
                                         const std::string &secret_key,
                                         const std::string &firmware_version,
                                         uint8_t battery_level) {
  // Build JSON body
  std::string mac_lower = device_id;
  for (auto &c : mac_lower)
    c = tolower(static_cast<unsigned char>(c));

  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "mac_address", mac_lower.c_str());
  cJSON_AddStringToObject(body, "device_token", secret_key.c_str());
  char *body_str = cJSON_PrintUnformatted(body);
  int body_len = strlen(body_str);

  // Build full URL
  std::string full_url = server_url;
  // Strip trailing slash if present
  if (!full_url.empty() && full_url.back() == '/')
    full_url.pop_back();
  full_url += "/api/v1/device/auth";

  ESP_LOGI(TAG, "Auth endpoint: %s", full_url.c_str());

  // Use esp_http_client for DNS resolution + TLS support
  std::string response_body;
  esp_http_client_config_t config = {};
  config.url = full_url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = _http_event_handler;
  config.user_data = &response_body;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    cJSON_free(body_str);
    cJSON_Delete(body);
    return ESP_FAIL;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body_str, body_len);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);

  cJSON_free(body_str);
  cJSON_Delete(body);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  esp_http_client_cleanup(client);

  ESP_LOGI(TAG, "HTTP status: %d, body: %d bytes", status, (int)response_body.size());

  if (status == 401) {
    // 解析 401 响应体，区分 pending_activation 和其他错误
    cJSON *err_json = cJSON_Parse(response_body.c_str());
    if (err_json) {
      cJSON *detail = cJSON_GetObjectItem(err_json, "detail");
      if (detail && cJSON_IsString(detail) &&
          strcmp(detail->valuestring, "device_pending_activation") == 0) {
        ESP_LOGW(TAG, "Device pending activation, awaiting admin approval");
        cJSON_Delete(err_json);
        return ESP_ERR_NOT_FINISHED;  // 特殊错误码: 设备待激活
      }
      cJSON_Delete(err_json);
    }
    ESP_LOGE(TAG, "Auth failed: status=401 body=%.200s", response_body.c_str());
    return ESP_FAIL;
  }
  if (status != 200) {
    ESP_LOGE(TAG, "Auth failed: status=%d body=%.200s", status, response_body.c_str());
    return ESP_FAIL;
  }

  if (response_body.empty()) {
    ESP_LOGE(TAG, "Empty response body");
    return ESP_FAIL;
  }

  // Parse JSON response
  cJSON *resp = cJSON_Parse(response_body.c_str());
  if (!resp) {
    ESP_LOGE(TAG, "Auth response parse failed: %.200s", response_body.c_str());
    return ESP_FAIL;
  }

  auto get_str = [&](const char *key) -> std::string {
    cJSON *item = cJSON_GetObjectItem(resp, key);
    const char *s = item ? cJSON_GetStringValue(item) : nullptr;
    return s ? std::string(s) : std::string();
  };

  auth_.mqtt_broker = get_str("mqtt_broker");
  auth_.mqtt_client_id = get_str("mqtt_client_id");
  auth_.mqtt_user = get_str("mqtt_user");
  auth_.mqtt_password = get_str("mqtt_password");
  auth_.publish_topic = get_str("publish_topic");
  auth_.subscribe_topic = get_str("subscribe_topic");

  cJSON *exp = cJSON_GetObjectItem(resp, "expires_in");
  auth_.expires_in = exp ? exp->valueint : 300;

  // 首次激活时服务端下发的 device_token (需持久化到 NVS)
  auth_.device_token = get_str("device_token");

  cJSON_Delete(resp);

  if (auth_.mqtt_broker.empty() || auth_.publish_topic.empty()) {
    ESP_LOGE(TAG, "Auth response missing mqtt_broker or publish_topic");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Auth OK — broker: %s, client_id: %s",
           auth_.mqtt_broker.c_str(), auth_.mqtt_client_id.c_str());
  return ESP_OK;
}

// -----------------------------------------------------------------------
// MQTT Connect
// -----------------------------------------------------------------------

esp_err_t HililyMqttClient::Connect() {
  if (auth_.mqtt_broker.empty()) {
    ESP_LOGE(TAG, "Not authenticated, call Authenticate() first");
    return ESP_FAIL;
  }

  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = auth_.mqtt_broker.c_str();
  mqtt_cfg.credentials.client_id = auth_.mqtt_client_id.c_str();
  mqtt_cfg.credentials.username = auth_.mqtt_user.c_str();
  mqtt_cfg.credentials.authentication.password = auth_.mqtt_password.c_str();
  mqtt_cfg.session.keepalive = 240;
  mqtt_cfg.network.reconnect_timeout_ms = 5000;
  // Only use TLS cert bundle for mqtts:// connections
  if (auth_.mqtt_broker.find("mqtts://") == 0) {
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  mqtt_cfg.task.stack_size = 8192;  // Ensure enough stack

  // LWT: broker sends goodbye on ungraceful disconnect
  static const char lwt_msg[] = "{\"type\":\"goodbye\",\"reason\":\"lwt\"}";
  mqtt_cfg.session.last_will.topic = auth_.publish_topic.c_str();
  mqtt_cfg.session.last_will.msg = lwt_msg;
  mqtt_cfg.session.last_will.msg_len = sizeof(lwt_msg) - 1;
  mqtt_cfg.session.last_will.qos = 1;
  mqtt_cfg.session.last_will.retain = 0;

  ESP_LOGI(TAG, "Free heap: %lu, Internal: %lu (largest: %lu), PSRAM: %lu",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  client_ = esp_mqtt_client_init(&mqtt_cfg);
  if (!client_) {
    ESP_LOGE(TAG, "esp_mqtt_client_init failed");
    return ESP_FAIL;
  }

  esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);

  esp_err_t ret = esp_mqtt_client_start(client_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "MQTT connecting to %s", auth_.mqtt_broker.c_str());
  return ESP_OK;
}

void HililyMqttClient::Disconnect() {
  if (client_) {
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
  }
  connected_ = false;
}

// -----------------------------------------------------------------------
// Publish
// -----------------------------------------------------------------------

esp_err_t HililyMqttClient::PublishJson(const char *json_str) {
  if (!connected_ || !client_)
    return ESP_FAIL;

  int msg_id =
      esp_mqtt_client_publish(client_, auth_.publish_topic.c_str(), json_str,
                              0, // len=0 → auto strlen
                              1, // QoS 1
                              0  // retain=0
      );

  if (msg_id < 0) {
    ESP_LOGW(TAG, "Publish failed");
    return ESP_FAIL;
  }
  return ESP_OK;
}

// -----------------------------------------------------------------------
// Event Handler
// -----------------------------------------------------------------------

void HililyMqttClient::EventHandler(void *arg, esp_event_base_t /*base*/,
                                    int32_t event_id, void *event_data) {
  auto *self = static_cast<HililyMqttClient *>(arg);
  esp_mqtt_event_handle_t event =
      static_cast<esp_mqtt_event_handle_t>(event_data);

  switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT connected");
    self->connected_ = true;
    // Subscribe to device down-link topic
    esp_mqtt_client_subscribe(self->client_,
                              self->auth_.subscribe_topic.c_str(),
                              1 // QoS 1
    );
    ESP_LOGI(TAG, "Subscribed: %s", self->auth_.subscribe_topic.c_str());
    if (self->status_callback_)
      self->status_callback_(true);
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT disconnected (will auto-reconnect)");
    self->connected_ = false;
    if (self->status_callback_)
      self->status_callback_(false);
    break;

  case MQTT_EVENT_DATA:
    // event->topic / event->topic_len / event->data / event->data_len
    if (event->data && event->data_len > 0 && self->json_callback_) {
      self->json_callback_(event->data, event->data_len);
    }
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT error: type=%d", event->error_handle->error_type);
    break;

  default:
    break;
  }
}

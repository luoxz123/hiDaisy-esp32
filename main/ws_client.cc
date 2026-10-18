#include "ws_client.h"
#include "battery.h"
#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <cstring>

static const char *TAG = "ws_client";

static const int HEARTBEAT_INTERVAL_MS = 30000;

WsClient& WsClient::GetInstance()
{
    static WsClient instance;
    return instance;
}

WsClient::~WsClient()
{
    StopHeartbeat();
    Disconnect();
}

// HTTP response buffer for auth
static char http_response_buf[2048];
static int http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (http_response_len + evt->data_len < sizeof(http_response_buf) - 1) {
            memcpy(http_response_buf + http_response_len, evt->data, evt->data_len);
            http_response_len += evt->data_len;
            http_response_buf[http_response_len] = 0;
        }
    }
    return ESP_OK;
}

esp_err_t WsClient::Authenticate(const std::string& server_url, const std::string& device_id,
                                  const std::string& secret_key, const std::string& firmware_version,
                                  uint8_t battery_level)
{
    std::string url = server_url + "/api/v1/device/auth";

    // Server expects mac_address in lowercase
    std::string mac_lower = device_id;
    for (auto& c : mac_lower) c = tolower(static_cast<unsigned char>(c));

    // Build JSON body matching server's DeviceAuthRequest schema
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "mac_address", mac_lower.c_str());
    cJSON_AddStringToObject(body, "device_token", secret_key.c_str());
    char *body_str = cJSON_PrintUnformatted(body);

    http_response_len = 0;
    memset(http_response_buf, 0, sizeof(http_response_buf));

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    cJSON_free(body_str);
    cJSON_Delete(body);

    if (ret != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Auth failed: ret=%s, status=%d", esp_err_to_name(ret), status);
        return ESP_FAIL;
    }

    // Parse flat response: {ws_url, ws_ticket, expires_in, device_id}
    cJSON *resp = cJSON_Parse(http_response_buf);
    if (!resp) {
        ESP_LOGE(TAG, "Auth response parse failed");
        return ESP_FAIL;
    }

    cJSON *ws_url = cJSON_GetObjectItem(resp, "ws_url");
    cJSON *ticket = cJSON_GetObjectItem(resp, "ws_ticket");
    cJSON *expires = cJSON_GetObjectItem(resp, "expires_in");

    const char *ws_url_str = ws_url ? cJSON_GetStringValue(ws_url) : NULL;
    const char *ticket_str = ticket ? cJSON_GetStringValue(ticket) : NULL;

    if (ws_url_str && ticket_str) {
        // ws_url already contains ?ticket=xxx, use directly
        auth_.ws_url = ws_url_str;
        auth_.token = ticket_str;
        auth_.expires_in = expires ? expires->valueint : 30;
        ESP_LOGI(TAG, "Auth success, ws_url: %s, expires: %d",
                 auth_.ws_url.c_str(), auth_.expires_in);
    } else {
        ESP_LOGE(TAG, "Auth response missing ws_url or ws_ticket");
        cJSON_Delete(resp);
        return ESP_FAIL;
    }

    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t WsClient::Connect()
{
    if (auth_.ws_url.empty() || auth_.token.empty()) {
        ESP_LOGE(TAG, "Not authenticated, call Authenticate() first");
        return ESP_FAIL;
    }

    // ws_url already contains ticket as query param, no extra auth header needed
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = auth_.ws_url.c_str();
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.buffer_size = 4096;
    ws_cfg.reconnect_timeout_ms = 5000;
    ws_cfg.network_timeout_ms = 10000;

    client_ = esp_websocket_client_init(&ws_cfg);
    if (!client_) {
        ESP_LOGE(TAG, "WebSocket client init failed");
        return ESP_FAIL;
    }

    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, EventHandler, this);

    esp_err_t ret = esp_websocket_client_start(client_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket connecting to %s", auth_.ws_url.c_str());
    return ESP_OK;
}

void WsClient::Disconnect()
{
    StopHeartbeat();
    if (client_) {
        esp_websocket_client_close(client_, portMAX_DELAY);
        esp_websocket_client_destroy(client_);
        client_ = NULL;
    }
    connected_ = false;
}

esp_err_t WsClient::SendJson(const char* json_str)
{
    if (!connected_ || !client_) return ESP_FAIL;
    int len = strlen(json_str);
    int sent = esp_websocket_client_send_text(client_, json_str, len, pdMS_TO_TICKS(1000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

esp_err_t WsClient::SendBinary(const uint8_t* data, int len)
{
    if (!connected_ || !client_) return ESP_FAIL;
    int sent = esp_websocket_client_send_bin(client_, (const char*)data, len, pdMS_TO_TICKS(1000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

void WsClient::EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<WsClient*>(arg);
    esp_websocket_event_data_t* ws_data = (esp_websocket_event_data_t*)data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            self->connected_ = true;
            self->StartHeartbeat();
            if (self->status_callback_) self->status_callback_(true);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED: {
            if (id == WEBSOCKET_EVENT_CLOSED) {
                ESP_LOGW(TAG, "WebSocket closed by peer");
            } else {
                ESP_LOGW(TAG, "WebSocket disconnected");
            }
            bool was_connected = self->connected_;
            self->connected_ = false;
            self->StopHeartbeat();
            if (was_connected && self->status_callback_) self->status_callback_(false);
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (ws_data->op_code == 0x01) { // Text frame
                if (self->json_callback_ && ws_data->data_ptr && ws_data->data_len > 0) {
                    self->json_callback_(ws_data->data_ptr, ws_data->data_len);
                }
            } else if (ws_data->op_code == 0x02) { // Binary frame
                if (self->binary_callback_ && ws_data->data_ptr && ws_data->data_len > 0) {
                    self->binary_callback_((const uint8_t*)ws_data->data_ptr, ws_data->data_len);
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

void WsClient::StartHeartbeat()
{
    if (heartbeat_timer_) return;

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = HeartbeatTimerCallback;
    timer_args.arg = this;
    timer_args.name = "ws_heartbeat";

    esp_timer_create(&timer_args, &heartbeat_timer_);
    esp_timer_start_periodic(heartbeat_timer_, HEARTBEAT_INTERVAL_MS * 1000);
    ESP_LOGI(TAG, "Heartbeat started (%ds)", HEARTBEAT_INTERVAL_MS / 1000);
}

void WsClient::StopHeartbeat()
{
    if (heartbeat_timer_) {
        esp_timer_stop(heartbeat_timer_);
        esp_timer_delete(heartbeat_timer_);
        heartbeat_timer_ = NULL;
    }
}

void WsClient::HeartbeatTimerCallback(void* arg)
{
    auto* self = static_cast<WsClient*>(arg);
    if (!self->connected_) return;

    cJSON *ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "type", "ping");
    cJSON_AddNumberToObject(ping, "battery", Battery::GetInstance().GetPercentage());
    cJSON_AddNumberToObject(ping, "wifi_rssi", WifiManager::GetInstance().GetRssi());

    char *str = cJSON_PrintUnformatted(ping);
    self->SendJson(str);

    cJSON_free(str);
    cJSON_Delete(ping);
}

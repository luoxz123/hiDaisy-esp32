#include "udp_client.h"

#include <arpa/inet.h>
#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/aes.h>
#include <sys/socket.h>

static const char *TAG = "udp_client";

// Receive buffer: max Opus packet + 16-byte header
static constexpr int UDP_RX_BUF_SIZE = 1600;

// -----------------------------------------------------------------------
// Singleton
// -----------------------------------------------------------------------

UdpClient &UdpClient::GetInstance() {
  static UdpClient instance;
  return instance;
}

UdpClient::~UdpClient() { Disconnect(); }

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

bool UdpClient::HexToBytes(const std::string &hex, uint8_t *out,
                           size_t expected_len) {
  if (hex.length() != expected_len * 2)
    return false;
  for (size_t i = 0; i < expected_len; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    auto hex_digit = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    int h = hex_digit(hi), l = hex_digit(lo);
    if (h < 0 || l < 0)
      return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

/**
 * AES-CTR decrypt/encrypt (symmetric).
 * nonce: 16 bytes used as IV for AES-CTR.
 */
bool UdpClient::AesCtrCrypt(const uint8_t *nonce, const uint8_t *in,
                            uint8_t *out, int len) {
  if (!aes_ready_)
    return false;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, aes_key_, 128);

  uint8_t stream_block[16] = {};
  uint8_t nc_off_buf[16];
  memcpy(nc_off_buf, nonce, 16);
  size_t nc_off = 0;

  int ret = mbedtls_aes_crypt_ctr(&ctx, (size_t)len, &nc_off, nc_off_buf,
                                  stream_block, in, out);
  mbedtls_aes_free(&ctx);
  return ret == 0;
}

// -----------------------------------------------------------------------
// Connect
// -----------------------------------------------------------------------

esp_err_t UdpClient::Connect(const std::string &server_ip, uint16_t port,
                             const std::string &key_hex,
                             const std::string &nonce_hex) {
  // Close existing socket first to prevent fd leak on reconnect.
  // This also unblocks ReceiveTask (if running) from its recvfrom() call.
  if (sock_ >= 0) {
    ESP_LOGI(TAG, "Closing old UDP socket (fd=%d) for reconnect", sock_);
    aes_ready_ = false;  // prevent Send() racing during socket swap
    close(sock_);
    sock_ = -1;
  }

  // Parse AES key and base nonce
  if (!HexToBytes(key_hex, aes_key_, 16)) {
    ESP_LOGE(TAG, "Invalid AES key hex (expect 32 chars): %s", key_hex.c_str());
    return ESP_FAIL;
  }
  if (!HexToBytes(nonce_hex, base_nonce_, 16)) {
    ESP_LOGE(TAG, "Invalid nonce hex (expect 32 chars): %s", nonce_hex.c_str());
    return ESP_FAIL;
  }
  local_seq_ = 0;
  remote_seq_ = 0;

  // Create UDP socket
  sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
    return ESP_FAIL;
  }

  // Set receive timeout (needed by ReceiveTask, which may already be running)
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Bind to any local port so we have a fixed source for send
  struct sockaddr_in local_addr = {};
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = 0; // OS assigns ephemeral port
  local_addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock_, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
    ESP_LOGW(TAG, "bind() failed: errno=%d (non-fatal)", errno);
  } else {
    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);
    getsockname(sock_, (struct sockaddr *)&bound_addr, &bound_len);
    ESP_LOGI(TAG, "UDP bound to local port %d", ntohs(bound_addr.sin_port));
  }

  // Store server address for send (do NOT connect — allows recvfrom any source)
  memset(&server_addr_, 0, sizeof(server_addr_));
  server_addr_.sin_family = AF_INET;
  server_addr_.sin_port = htons(port);
  inet_aton(server_ip.c_str(), &server_addr_.sin_addr);

  // Compute SSRC as a simple hash of the nonce
  ssrc_ = ((uint32_t)base_nonce_[0] << 24) | ((uint32_t)base_nonce_[1] << 16) |
          ((uint32_t)base_nonce_[2] << 8) | (uint32_t)base_nonce_[3];

  // Enable encryption AFTER socket and SSRC are fully set up
  aes_ready_ = true;

  ESP_LOGI(TAG, "UDP socket connected to %s:%u (fd=%d, ssrc=0x%08lx), AES ready",
           server_ip.c_str(), port, sock_, (unsigned long)ssrc_);
  return ESP_OK;
}

void UdpClient::Disconnect() {
  if (sock_ >= 0) {
    close(sock_);
    sock_ = -1;
  }
  aes_ready_ = false;
  ESP_LOGI(TAG, "UDP disconnected");
}

// -----------------------------------------------------------------------
// Send (uplink)
// -----------------------------------------------------------------------

esp_err_t UdpClient::Send(const uint8_t *opus_data, int len,
                          uint32_t timestamp) {
  if (sock_ < 0 || !aes_ready_)
    return ESP_FAIL;

  local_seq_++;

  // Build 16-byte header (acts as AES-CTR nonce)
  UdpPacketHeader header = {};
  header.type = 0x01;
  header.flags = 0x00;
  header.payload_len = htons((uint16_t)len);
  header.ssrc = htonl(ssrc_);
  header.timestamp = htonl(timestamp);
  header.sequence = htonl(local_seq_);

  const uint8_t *nonce = reinterpret_cast<const uint8_t *>(&header);

  // Encrypt payload
  static uint8_t encrypted_buf[UDP_RX_BUF_SIZE];
  if (len > (int)(sizeof(encrypted_buf))) {
    ESP_LOGW(TAG, "Opus packet too large (%d), dropped", len);
    return ESP_FAIL;
  }

  if (!AesCtrCrypt(nonce, opus_data, encrypted_buf, len)) {
    ESP_LOGW(TAG, "AES-CTR encrypt failed");
    return ESP_FAIL;
  }

  // Build final packet: header + encrypted payload
  static uint8_t pkt_buf[sizeof(UdpPacketHeader) + UDP_RX_BUF_SIZE];
  memcpy(pkt_buf, &header, sizeof(header));
  memcpy(pkt_buf + sizeof(header), encrypted_buf, len);

  int total = sizeof(header) + len;
  int sent = sendto(sock_, pkt_buf, total, 0,
                    (struct sockaddr *)&server_addr_, sizeof(server_addr_));
  if (sent != total) {
    ESP_LOGW(TAG, "send() partial: sent=%d, expected=%d", sent, total);
    return ESP_FAIL;
  }
  return ESP_OK;
}

// -----------------------------------------------------------------------
// ReceiveTask (downlink)
// -----------------------------------------------------------------------

void UdpClient::StartReceiveTask() {
  if (rx_task_started_) {
    ESP_LOGI(TAG, "UDP receive task already started");
    return;
  }
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGE(TAG, ">>> CREATING UDP RX TASK (sock=%d, internal_free=%u) <<<",
           sock_, (unsigned)internal_free);

  // Allocate stack from PSRAM to avoid exhausting internal SRAM
  static const int STACK_SIZE = 8192;
  static StackType_t *stack_buf = nullptr;
  static StaticTask_t task_tcb;

  if (!stack_buf) {
    stack_buf = (StackType_t *)heap_caps_malloc(STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!stack_buf) {
      ESP_LOGE(TAG, "Failed to allocate stack from PSRAM!");
      return;
    }
  }

  TaskHandle_t h = xTaskCreateStaticPinnedToCore(
      ReceiveTask, "udp_rx", STACK_SIZE, this, 11, stack_buf, &task_tcb, 0);
  if (!h) {
    ESP_LOGE(TAG, "FAILED to create udp_rx task (static)!");
  } else {
    rx_task_started_ = true;
    ESP_LOGE(TAG, ">>> udp_rx task created OK (PSRAM stack) <<<");
  }
}

void UdpClient::ReceiveTask(void *arg) {
  auto *self = static_cast<UdpClient *>(arg);
  static uint8_t rx_buf[sizeof(UdpPacketHeader) + UDP_RX_BUF_SIZE];
  static uint8_t decrypted_buf[UDP_RX_BUF_SIZE];

  ESP_LOGE(TAG, ">>> UDP RECEIVE TASK ALIVE (Core %d, sock=%d) <<<", xPortGetCoreID(), self->sock_);

  // Set receive timeout so we can log periodic status
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(self->sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  int rx_count = 0;
  int64_t last_keepalive_us = esp_timer_get_time();
  static constexpr int64_t KEEPALIVE_INTERVAL_US = 30 * 1000000LL; // 30 seconds

  // Send immediate keepalive to register client_addr with server
  {
    static const uint8_t keepalive = 0;
    self->Send(&keepalive, 1, (uint32_t)(last_keepalive_us / 1000));
    ESP_LOGI(TAG, "UDP initial keepalive sent");
  }

  while (true) {
    self->rx_loop_count_++;
    if (self->sock_ < 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    int received = recvfrom(self->sock_, rx_buf, sizeof(rx_buf), 0,
                            (struct sockaddr *)&src_addr, &addr_len);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Timeout — log every time (5s interval from SO_RCVTIMEO)
        ESP_LOGW(TAG, "UDP rx timeout (total rx=%d, sock=%d)", rx_count, self->sock_);
        // Send keepalive to refresh server session
        int64_t now = esp_timer_get_time();
        if (now - last_keepalive_us >= KEEPALIVE_INTERVAL_US) {
          last_keepalive_us = now;
          // Send minimal 1-byte keepalive via existing Send()
          static const uint8_t keepalive = 0;
          self->Send(&keepalive, 1, (uint32_t)(now / 1000));
          ESP_LOGI(TAG, "UDP keepalive sent");
        }
        continue;
      }
      ESP_LOGW(TAG, "recv() error: errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (received <= (int)sizeof(UdpPacketHeader)) {
      ESP_LOGW(TAG, "UDP rx too short: %d bytes", received);
      continue;
    }

    rx_count++;
    if (rx_count <= 5) {
      char src_ip[16];
      inet_ntoa_r(src_addr.sin_addr, src_ip, sizeof(src_ip));
      ESP_LOGI(TAG, "UDP rx #%d from %s:%d, %d bytes",
               rx_count, src_ip, ntohs(src_addr.sin_port), received);
    }
    const UdpPacketHeader *hdr =
        reinterpret_cast<const UdpPacketHeader *>(rx_buf);

    // Validate type
    if (hdr->type != 0x01) {
      ESP_LOGW(TAG, "UDP rx unknown type: 0x%02x", hdr->type);
      continue;
    }

    uint32_t seq = ntohl(hdr->sequence);

    // Simple anti-replay: drop old packets
    if (seq < self->remote_seq_) {
      ESP_LOGD(TAG, "Old seq %lu dropped", (unsigned long)seq);
      continue;
    }
    self->remote_seq_ = seq;

    // The 16-byte header IS the nonce for AES-CTR
    const uint8_t *nonce = rx_buf;
    const uint8_t *payload = rx_buf + sizeof(UdpPacketHeader);
    int pay_len = received - sizeof(UdpPacketHeader);

    if (!self->AesCtrCrypt(nonce, payload, decrypted_buf, pay_len)) {
      ESP_LOGW(TAG, "Decrypt failed, dropping packet");
      continue;
    }

    if (rx_count <= 3) {
      ESP_LOGI(TAG, "UDP rx #%d: %d bytes payload, seq=%lu",
               rx_count, pay_len, (unsigned long)seq);
    }

    // Deliver decoded Opus frame to registered callback
    if (self->audio_callback_) {
      self->audio_callback_(decrypted_buf, pay_len);
    }
  }
}

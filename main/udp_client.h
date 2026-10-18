#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include <esp_err.h>
#include <functional>
#include <lwip/sockets.h>
#include <stdint.h>
#include <string>

/**
 * UDP 音频包头格式 (与小智 mqtt_protocol.cc 兼容)
 * 16 字节头，同时作为 AES-CTR nonce
 *
 * | type 1u | flags 1u | payload_len 2u | ssrc 4u | timestamp 4u | seq 4u |
 */
struct UdpPacketHeader {
  uint8_t type;         // 0x01 = audio
  uint8_t flags;        // 0x00
  uint16_t payload_len; // big-endian, 不含 header
  uint32_t ssrc;        // 设备端固定为设备 ID hash；服务端为 0
  uint32_t timestamp;   // 毫秒时间戳（环回）
  uint32_t sequence;    // 单调递增包序号
} __attribute__((packed));

static_assert(sizeof(UdpPacketHeader) == 16,
              "UdpPacketHeader must be 16 bytes");

/**
 * UdpClient: 负责 ESP32 端的 UDP 音频通道
 *
 * 流程:
 *   1. 调用 Connect(server_ip, port) 创建已连接的 UDP Socket
 *   2. 调用 SetAesKey(key_hex, nonce_hex) 初始化 AES-CTR 上下文
 *   3. 调用 StartReceiveTask() 在后台持续接收从服务器下发的音频包
 *   4. 调用 Send(opus_data, len, timestamp) 发送加密的 Opus 上行音频包
 */
class UdpClient {
public:
  using AudioCallback = std::function<void(const uint8_t *opus_data, int len)>;

  static UdpClient &GetInstance();

  UdpClient(const UdpClient &) = delete;
  UdpClient &operator=(const UdpClient &) = delete;

  /**
   * 解析 hello 响应，连接 UDP 服务器并初始化 AES-CTR 密钥
   * @param server_ip  服务器对外 IP（字符串）
   * @param port       UDP 端口号
   * @param key_hex    32 字符十六进制 AES-128 密钥
   * @param nonce_hex  32 字符十六进制 AES-CTR nonce (16 bytes)
   */
  esp_err_t Connect(const std::string &server_ip, uint16_t port,
                    const std::string &key_hex, const std::string &nonce_hex);

  void Disconnect();
  bool IsConnected() const { return sock_ >= 0; }
  int GetSock() const { return sock_; }
  bool IsAesReady() const { return aes_ready_; }
  int GetRxLoopCount() const { return rx_loop_count_; }

  /**
   * 发送 Opus 音频帧（自动加 header + AES-CTR 加密）
   * @param opus_data  opus 编码后的裸数据
   * @param len        数据字节长度
   * @param timestamp  当前时间戳（ms，uint32_t 回绕）
   */
  esp_err_t Send(const uint8_t *opus_data, int len, uint32_t timestamp);

  /**
   * 启动后台接收任务（需在 Connect() 之后调用）
   */
  void StartReceiveTask();

  /**
   * 注册收到服务器下行音频帧的回调
   * 回调中 opus_data 是解密后的载荷，调用者负责 Opus 解码
   */
  void OnAudio(AudioCallback cb) { audio_callback_ = std::move(cb); }

private:
  UdpClient() = default;
  ~UdpClient();

  static void ReceiveTask(void *arg);

  /** 将 hex 字符串解析为字节数组 */
  static bool HexToBytes(const std::string &hex, uint8_t *out,
                         size_t expected_len);

  /** AES-CTR 加解密（对称操作）; nonce = 填入 ts/seq 后的 16 字节头部 */
  bool AesCtrCrypt(const uint8_t *nonce, const uint8_t *in, uint8_t *out,
                   int len);

  int sock_ = -1;
  struct sockaddr_in server_addr_ = {};  // Server address for sendto()
  uint32_t local_seq_ = 0;
  uint32_t remote_seq_ = 0;
  uint32_t ssrc_ = 0; // 设备 ID hash，Send 时用

  // AES-CTR 密钥 (16 bytes) 和基础 nonce (16 bytes)
  uint8_t aes_key_[16] = {};
  uint8_t base_nonce_[16] = {};
  bool aes_ready_ = false;

  AudioCallback audio_callback_;
  volatile int rx_loop_count_ = 0;
  bool rx_task_started_ = false;
};

#endif // UDP_CLIENT_H

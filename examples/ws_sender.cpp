// ws_sender.cpp — 发送端示例
//
// 实际用例: 定频读视频流 → 编码 → WebSocket 发送 300 字节/包 @ 50Hz
//
// 依赖: libixwebsocket (apt install libixwebsocket-dev 或 vcpkg install
// ixwebsocket)
//       也可替换为任何 WebSocket 库, 核心逻辑不变
//
// 编译 (独立):
//   g++ -std=c++17 -O2 ws_sender.cpp \
//       ../src/compressed_stream_pipeline.cpp ../src/stream_encoder.cpp \
//       ../src/bitstream_packetizer.cpp ../src/frame_preprocessor.cpp \
//       ../src/pipeline_config.cpp \
//       -I../include $(pkg-config --cflags --libs libavcodec libavutil
//       libavformat libswscale) \
//       $(pkg-config --cflags --libs opencv4) -lixwebsocket -lyaml-cpp
//       -lpthread -lz \ -o ws_sender

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/opencv.hpp>

// ---- WebSocket 头文件 ----
// 此处使用 IXWebSocket 作为示例, 可替换为其他库
// 如果没有安装, 参考下方 "无 WebSocket 库" 的 stub 实现
#ifdef USE_IXWEBSOCKET
#include <ixwebsocket/IXWebSocketServer.h>
#else
// ---- Stub: 无 WebSocket 库时的最小替代 ----
// 用 TCP socket 发送, 仅供演示结构
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "bitstream_packetizer.h"
#include "compressed_stream_pipeline.h"
#include "pipeline_config.h"

// ============================================================
// WebSocket 发送器 (抽象层)
// ============================================================

class PacketSender {
public:
  virtual ~PacketSender() = default;
  virtual bool start(int port) = 0;
  virtual void stop() = 0;
  virtual void sendPacket(const uint8_t *data, size_t len) = 0;
  virtual int clientCount() const = 0;
};

#ifdef USE_IXWEBSOCKET

// ---- IXWebSocket 实现 ----
class WsSender : public PacketSender {
public:
  bool start(int port) override {
    server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket &ws,
               const ix::WebSocketMessagePtr &msg) {
          if (msg->type == ix::WebSocketMessageType::Open) {
            std::lock_guard<std::mutex> lk(mu_);
            clients_.insert(&ws);
            std::printf("[WS] Client connected: %s (%zu total)\n",
                        state->getRemoteIp().c_str(), clients_.size());
          } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::lock_guard<std::mutex> lk(mu_);
            clients_.erase(&ws);
            std::printf("[WS] Client disconnected (%zu remaining)\n",
                        clients_.size());
          }
        });

    auto res = server_.listen(port);
    if (!res.first) {
      std::fprintf(stderr, "[WS] Failed to listen on port %d: %s\n", port,
                   res.second.c_str());
      return false;
    }
    server_.start();
    std::printf("[WS] Server listening on ws://0.0.0.0:%d\n", port);
    return true;
  }

  void stop() override { server_.stop(); }

  void sendPacket(const uint8_t *data, size_t len) override {
    std::lock_guard<std::mutex> lk(mu_);
    std::string binary(reinterpret_cast<const char *>(data), len);
    for (auto *ws : clients_) {
      ws->sendBinary(binary);
    }
  }

  int clientCount() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(clients_.size());
  }

private:
  ix::WebSocketServer server_;
  std::set<ix::WebSocket *> clients_;
  mutable std::mutex mu_;
};

#else

// ---- TCP Stub 实现 (无 WebSocket 库时的演示) ----
// 直接通过 TCP 发送裸 300 字节包, 接收端需要对应处理
class WsSender : public PacketSender {
public:
  ~WsSender() override { stop(); }

  bool start(int port) override {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0)
      return false;

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd_, (sockaddr *)&addr, sizeof(addr)) < 0) {
      std::fprintf(stderr, "[TCP] Failed to bind port %d\n", port);
      return false;
    }
    listen(server_fd_, 4);

    running_ = true;
    accept_thread_ = std::thread([this]() { acceptLoop(); });

    std::printf("[TCP] Server listening on tcp://0.0.0.0:%d\n",
                ntohs(addr.sin_port));
    std::printf(
        "[TCP] NOTE: This is a raw TCP stub. For production, use WebSocket.\n");
    return true;
  }

  void stop() override {
    running_ = false;
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
    if (accept_thread_.joinable())
      accept_thread_.join();

    std::lock_guard<std::mutex> lk(mu_);
    for (int fd : client_fds_)
      ::close(fd);
    client_fds_.clear();
  }

  void sendPacket(const uint8_t *data, size_t len) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = client_fds_.begin();
    while (it != client_fds_.end()) {
      ssize_t sent = ::send(*it, data, len, MSG_NOSIGNAL);
      if (sent <= 0) {
        ::close(*it);
        it = client_fds_.erase(it);
      } else {
        ++it;
      }
    }
  }

  int clientCount() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(client_fds_.size());
  }

private:
  void acceptLoop() {
    while (running_) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(server_fd_, &fds);
      timeval tv{0, 100000}; // 100ms
      if (select(server_fd_ + 1, &fds, nullptr, nullptr, &tv) > 0) {
        int fd = accept(server_fd_, nullptr, nullptr);
        if (fd >= 0) {
          std::lock_guard<std::mutex> lk(mu_);
          client_fds_.push_back(fd);
          std::printf("[TCP] Client connected (%zu total)\n",
                      client_fds_.size());
        }
      }
    }
  }

  int server_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::vector<int> client_fds_;
  mutable std::mutex mu_;
};

#endif

// ============================================================
// 主程序
// ============================================================

static void printUsage(const char *prog) {
  std::fprintf(stderr,
               "Usage: %s <input_video_or_device> [options]\n"
               "\n"
               "定频读取视频流, AV1 编码后通过 WebSocket 发送 300B 包\n"
               "\n"
               "输入:\n"
               "  /dev/video0              USB 摄像头 (V4L2)\n"
               "  rtsp://ip/stream         RTSP 网络摄像头\n"
               "  input.mp4                本地视频文件\n"
               "  0                        默认摄像头 (OpenCV index)\n"
               "\n"
               "Options:\n"
               "  --config <path>          配置文件 (默认: ./config.yaml)\n"
               "  --port <int>             WebSocket 端口 (默认: 9002)\n"
               "  --max-frames <int>       最大帧数 (默认: 无限制)\n"
               "  --debug                  显示预处理预览\n",
               "  --loop                    视频文件循环播放\n", prog);
}

int main(int argc, char **argv) {
  // --- 解析参数 ---
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::string input = argv[1];
  std::string config_path = "config.yaml";
  int port = 9002;
  int max_frames = 0;
  bool debug = false;
  bool loop = false;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    auto nextVal = [&]() -> const char * {
      if (i + 1 < argc)
        return argv[++i];
      std::fprintf(stderr, "Missing value for %s\n", arg.c_str());
      std::exit(1);
    };

    if (arg == "--config")
      config_path = nextVal();
    else if (arg == "--port")
      port = std::atoi(nextVal());
    else if (arg == "--max-frames")
      max_frames = std::atoi(nextVal());
    else if (arg == "--debug")
      debug = true;
    else if (arg == "--loop")
      loop = true;
    else {
      std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      printUsage(argv[0]);
      return 1;
    }
  }

  // --- 加载配置 ---
  PipelineConfig pcfg;
  if (!PipelineConfig::load(config_path, pcfg)) {
    std::fprintf(stderr, "Failed to load config: %s\n", config_path.c_str());
    return 1;
  }
  pcfg.print();

  if (pcfg.encode_width % 2 != 0)
    --pcfg.encode_width;
  if (pcfg.encode_height % 2 != 0)
    --pcfg.encode_height;

  // --- 打开视频源 ---
  cv::VideoCapture cap;

  // 尝试作为数字 (摄像头 index)
  bool is_device = false;
  try {
    int idx = std::stoi(input);
    cap.open(idx);
    is_device = true;
  } catch (...) {
    // 非数字, 作为文件/URL 打开
    cap.open(input);
  }

  if (!cap.isOpened()) {
    std::fprintf(stderr, "Failed to open video source: %s\n", input.c_str());
    return 1;
  }

  double src_fps = cap.get(cv::CAP_PROP_FPS);
  if (src_fps <= 0.0)
    src_fps = pcfg.input_fps;

  int src_w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int src_h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

  std::printf("Source: %s (%dx%d @ %.1f fps)\n", input.c_str(), src_w, src_h,
              src_fps);
  std::printf("Encode: %dx%d @ %d fps, bitrate=%d bps\n", pcfg.encode_width,
              pcfg.encode_height, pcfg.encode_fps, pcfg.bitrate);
  std::printf("Packet: %zu bytes @ %d Hz = %d bps\n",
              BitstreamPacketizer::PACKET_SIZE,
              BitstreamPacketizer::SEND_RATE_HZ,
              static_cast<int>(BitstreamPacketizer::PACKET_SIZE * 8 *
                               BitstreamPacketizer::SEND_RATE_HZ));

  // --- 启动 WebSocket 服务器 ---
  WsSender sender;
  if (!sender.start(port))
    return 1;

  // --- 配置编码管线 ---
  CompressedStreamPipeline::Config cfg;
  cfg.encode_width = pcfg.encode_width;
  cfg.encode_height = pcfg.encode_height;
  cfg.fps = pcfg.encode_fps;
  cfg.bitrate = pcfg.bitrate;
  cfg.max_bitrate = pcfg.bitrate;
  cfg.gop_size = pcfg.gop;
  cfg.cpu_used = pcfg.cpu_used;
  cfg.encoder_threads = pcfg.encoder_threads;

  cfg.gray = true;
  cfg.bitshift = pcfg.bitshift;
  cfg.gamma = pcfg.gamma;
  cfg.lut_type = (pcfg.shadow_gain > 1.0) ? "shadow" : "none";
  cfg.shadow_gain = pcfg.shadow_gain;
  cfg.shadow_threshold = pcfg.shadow_threshold;
  cfg.shadow_floor = pcfg.shadow_floor;
  cfg.highlight_roof = pcfg.highlight_roof;
  cfg.edge_enhance = pcfg.edge_enhance;
  cfg.high_freq_boost = pcfg.high_freq_boost;
  cfg.motion_enhance = pcfg.motion_enhance;
  cfg.center_crop_ratio = pcfg.center_crop;

  CompressedStreamPipeline pipeline(cfg);

  // 调试回调
  bool debug_active = debug;
  if (debug_active) {
    pipeline.setDebugFrameCallback([&debug_active](const cv::Mat &processed) {
      if (!debug_active)
        return;
      cv::imshow("Preprocessed", processed);
      int key = cv::waitKey(1);
      if (key == 27) {
        debug_active = false;
        cv::destroyWindow("Preprocessed");
      }
    });
  }

  // 包输出回调: 通过 WebSocket 发送
  std::atomic<uint64_t> packets_sent{0};
  pipeline.setPacketCallback([&](const BitstreamPacketizer::Packet &pkt) {
    sender.sendPacket(pkt.data, BitstreamPacketizer::PACKET_SIZE);
    ++packets_sent;
  });

  // --- 主循环: 定频读取 + 编码 + 50Hz 发包 ---
  //
  // 核心时序:
  //   输入线程 (此线程): 按 input_fps 读帧 → 帧率转换 → feedFrame →
  //   producePacket 发包节奏: 每编码一帧, 按比例产生 N 个包 (encode_fps:50Hz)
  //   WebSocket: 包通过回调实时发送给所有客户端
  //
  // 对于实时源 (摄像头/RTSP), cap.read() 本身就是按源帧率阻塞的
  // 对于文件输入, 需要手动限速

  double input_fps = pcfg.input_fps;
  double encode_fps = static_cast<double>(pcfg.encode_fps);
  double fps_accumulator = 0.0;

  int src_frame_idx = 0;
  int encoded_frames = 0;
  auto start_time = std::chrono::steady_clock::now();
  auto frame_interval = std::chrono::duration<double>(1.0 / input_fps);

  std::printf("\nStreaming started. Waiting for clients on port %d...\n", port);
  std::printf("Press Ctrl+C to stop.\n\n");

  while (true) {
    auto frame_start = std::chrono::steady_clock::now();

    cv::Mat bgr;
    if (!cap.read(bgr) || bgr.empty()) {
      if (is_device) {
        // 摄像头断开, 尝试重连
        std::fprintf(stderr, "Camera read failed, retrying...\n");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      // 文件/URL 结束
      if (loop) {
        bool rewind_ok = cap.set(cv::CAP_PROP_POS_FRAMES, 0);
        if (!rewind_ok) {
          cap.release();
          cap.open(input);
          rewind_ok = cap.isOpened();
        }

        if (rewind_ok) {
          std::printf("[Sender] Looping input: %s\n", input.c_str());
          fps_accumulator = 0.0;
          continue;
        }

        std::fprintf(stderr, "Failed to loop input: %s\n", input.c_str());
      }

      break; // 文件结束
    }

    ++src_frame_idx;

    // 帧率转换 (累加器)
    fps_accumulator += encode_fps;
    if (fps_accumulator < input_fps) {
      // 对文件输入限速
      if (!is_device) {
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        auto sleep_time = frame_interval - elapsed;
        if (sleep_time > std::chrono::microseconds(100))
          std::this_thread::sleep_for(sleep_time);
      }
      continue;
    }
    fps_accumulator -= input_fps;

    // 编码
    if (!pipeline.feedFrame(bgr)) {
      std::fprintf(stderr, "feedFrame failed at frame %d\n", encoded_frames);
      break;
    }
    ++encoded_frames;

    // 按比例产生包 (50Hz / encode_fps 个包/帧)
    int pkts_this_frame =
        (encoded_frames * BitstreamPacketizer::SEND_RATE_HZ / pcfg.encode_fps) -
        ((encoded_frames - 1) * BitstreamPacketizer::SEND_RATE_HZ /
         pcfg.encode_fps);

    for (int p = 0; p < pkts_this_frame; ++p) {
      pipeline.producePacket();
      // producePacket 内部会触发 PacketCallback → WebSocket 发送
    }

    // 进度报告
    if (encoded_frames % 100 == 0) {
      auto now = std::chrono::steady_clock::now();
      double elapsed = std::chrono::duration<double>(now - start_time).count();
      std::printf("[Sender] %d frames encoded, %llu packets sent, "
                  "%d clients, %.1f fps, buffer: %zu bytes\n",
                  encoded_frames,
                  static_cast<unsigned long long>(packets_sent.load()),
                  sender.clientCount(), encoded_frames / elapsed,
                  pipeline.bufferedBytes());
    }

    if (max_frames > 0 && encoded_frames >= max_frames)
      break;

    // 文件输入限速: 模拟实时帧率
    if (!is_device) {
      auto elapsed = std::chrono::steady_clock::now() - frame_start;
      auto sleep_time = frame_interval - elapsed;
      if (sleep_time > std::chrono::microseconds(100))
        std::this_thread::sleep_for(sleep_time);
    }
  }

  // --- 清理 ---
  std::printf("\nFlushing encoder...\n");
  pipeline.flush();
  while (pipeline.bufferedBytes() > 0)
    pipeline.producePacket();

  sender.stop();
  cap.release();

  auto total_time = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start_time)
                        .count();

  std::printf("\n=== Sender Summary ===\n");
  std::printf("Frames encoded: %d (from %d input)\n", encoded_frames,
              src_frame_idx);
  std::printf("Packets sent: %llu\n",
              static_cast<unsigned long long>(packets_sent.load()));
  std::printf("Duration: %.2f s\n", total_time);
  std::printf("Avg fps: %.1f\n", encoded_frames / total_time);

  return 0;
}

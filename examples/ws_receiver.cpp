// ws_receiver.cpp — 接收端示例
//
// 实际用例: WebSocket 接收 300 字节/包 → AV1 解码 → 实时显示 + 可选保存
//
// 依赖: libixwebsocket (apt install libixwebsocket-dev 或 vcpkg install ixwebsocket)
//       也可替换为任何 WebSocket 库, 核心逻辑不变
//
// 编译 (独立):
//   g++ -std=c++17 -O2 ws_receiver.cpp \
//       ../src/decoder_pipeline.cpp ../src/stream_decoder.cpp \
//       ../src/packet_reassembler.cpp ../src/bitstream_packetizer.cpp \
//       ../src/pipeline_config.cpp \
//       -I../include $(pkg-config --cflags --libs libavcodec libavutil libavformat libswscale) \
//       $(pkg-config --cflags --libs opencv4) -lixwebsocket -lyaml-cpp -lpthread -lz \
//       -o ws_receiver

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/opencv.hpp>

// ---- WebSocket 头文件 ----
#ifdef USE_IXWEBSOCKET
#include <ixwebsocket/IXWebSocket.h>
#else
// ---- Stub: TCP 客户端替代 ----
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "bitstream_packetizer.h"
#include "decoder_pipeline.h"
#include "pipeline_config.h"

// ============================================================
// 线程安全的包队列
// ============================================================

class PacketQueue
{
public:
    explicit PacketQueue(size_t max_packets = 5000)
        : max_packets_(max_packets) {}

    // 入队 (网络线程调用)
    void push(const uint8_t *data, size_t len)
    {
        if (len != BitstreamPacketizer::PACKET_SIZE)
            return;

        std::lock_guard<std::mutex> lk(mu_);

        // 队列满时丢弃旧包 (保持实时性)
        if (queue_.size() >= max_packets_)
        {
            queue_.pop_front();
            ++dropped_;
        }

        queue_.emplace_back();
        std::memcpy(queue_.back().data(), data, BitstreamPacketizer::PACKET_SIZE);
        ++total_received_;
        cv_.notify_one();
    }

    // 出队 (解码线程调用, 阻塞等待)
    bool pop(uint8_t *out, int timeout_ms = 100)
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [this]
                          { return !queue_.empty() || stopped_; }))
        {
            return false;
        }
        if (queue_.empty())
            return false;

        std::memcpy(out, queue_.front().data(), BitstreamPacketizer::PACKET_SIZE);
        queue_.pop_front();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopped_ = true;
        cv_.notify_all();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }

    uint64_t totalReceived() const { return total_received_; }
    uint64_t dropped() const { return dropped_; }

private:
    using PacketBuf = std::array<uint8_t, BitstreamPacketizer::PACKET_SIZE>;

    std::deque<PacketBuf> queue_;
    size_t max_packets_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopped_ = false;
    std::atomic<uint64_t> total_received_{0};
    std::atomic<uint64_t> dropped_{0};
};

// ============================================================
// WebSocket 接收器 (抽象层)
// ============================================================

class PacketReceiver
{
public:
    virtual ~PacketReceiver() = default;
    virtual bool connect(const std::string &url) = 0;
    virtual void disconnect() = 0;
    virtual bool connected() const = 0;

    void setQueue(PacketQueue *q) { queue_ = q; }

protected:
    PacketQueue *queue_ = nullptr;
};

#ifdef USE_IXWEBSOCKET

// ---- IXWebSocket 实现 ----
class WsReceiver : public PacketReceiver
{
public:
    bool connect(const std::string &url) override
    {
        ws_.setUrl(url);
        ws_.setOnMessageCallback(
            [this](const ix::WebSocketMessagePtr &msg)
            {
                if (msg->type == ix::WebSocketMessageType::Message &&
                    msg->binary)
                {
                    queue_->push(
                        reinterpret_cast<const uint8_t *>(msg->str.data()),
                        msg->str.size());
                }
                else if (msg->type == ix::WebSocketMessageType::Open)
                {
                    std::printf("[WS] Connected to server\n");
                    connected_ = true;
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    std::printf("[WS] Disconnected\n");
                    connected_ = false;
                }
                else if (msg->type == ix::WebSocketMessageType::Error)
                {
                    std::fprintf(stderr, "[WS] Error: %s\n",
                                 msg->errorInfo.reason.c_str());
                }
            });

        ws_.start();
        // 等待连接
        for (int i = 0; i < 50; ++i)
        {
            if (connected_)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "[WS] Connection timeout\n");
        return false;
    }

    void disconnect() override
    {
        ws_.stop();
        connected_ = false;
    }

    bool connected() const override { return connected_; }

private:
    ix::WebSocket ws_;
    std::atomic<bool> connected_{false};
};

#else

// ---- TCP Stub 实现 ----
class WsReceiver : public PacketReceiver
{
public:
    ~WsReceiver() override { disconnect(); }

    bool connect(const std::string &url) override
    {
        // 解析 tcp://host:port
        std::string host = "127.0.0.1";
        int port = 9002;

        // 简单解析
        auto pos = url.find("://");
        std::string addr = (pos != std::string::npos) ? url.substr(pos + 3) : url;
        auto colon = addr.find(':');
        if (colon != std::string::npos)
        {
            host = addr.substr(0, colon);
            port = std::atoi(addr.substr(colon + 1).c_str());
        }

        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &sa.sin_addr);

        if (::connect(fd_, (sockaddr *)&sa, sizeof(sa)) < 0)
        {
            std::fprintf(stderr, "[TCP] Failed to connect to %s:%d\n",
                         host.c_str(), port);
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        connected_ = true;
        recv_thread_ = std::thread([this]()
                                   { recvLoop(); });

        std::printf("[TCP] Connected to %s:%d\n", host.c_str(), port);
        return true;
    }

    void disconnect() override
    {
        connected_ = false;
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        if (recv_thread_.joinable())
            recv_thread_.join();
    }

    bool connected() const override { return connected_; }

private:
    void recvLoop()
    {
        uint8_t buf[BitstreamPacketizer::PACKET_SIZE];
        while (connected_)
        {
            // TCP 可能分段, 确保读满 300 字节
            size_t total = 0;
            while (total < BitstreamPacketizer::PACKET_SIZE && connected_)
            {
                ssize_t n = recv(fd_, buf + total,
                                 BitstreamPacketizer::PACKET_SIZE - total, 0);
                if (n <= 0)
                {
                    connected_ = false;
                    return;
                }
                total += n;
            }
            if (total == BitstreamPacketizer::PACKET_SIZE && queue_)
            {
                queue_->push(buf, total);
            }
        }
    }

    int fd_ = -1;
    std::atomic<bool> connected_{false};
    std::thread recv_thread_;
};

#endif

// ============================================================
// 主程序
// ============================================================

static void printUsage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "\n"
                 "从 WebSocket 接收 300 字节包, AV1 解码, 实时预览 + 可选保存\n"
                 "\n"
                 "Options:\n"
                 "  --config <path>          配置文件 (默认: ./config.yaml)\n"
#ifdef USE_IXWEBSOCKET
                 "  --url <ws://host:port>   WebSocket 地址 (默认: ws://127.0.0.1:9002)\n"
#else
                 "  --url <host:port>        TCP 地址 (默认: 127.0.0.1:9002)\n"
#endif
                 "  --output <path>          保存视频路径 (默认: 不保存)\n"
                 "  --no-preview             不显示预览窗口\n",
                 prog);
}

int main(int argc, char **argv)
{
    // --- 解析参数 ---
    std::string config_path = "config.yaml";
#ifdef USE_IXWEBSOCKET
    std::string url = "ws://127.0.0.1:9002";
#else
    std::string url = "127.0.0.1:9002";
#endif
    std::string output;
    bool show_preview = true;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto nextVal = [&]() -> const char *
        {
            if (i + 1 < argc)
                return argv[++i];
            std::fprintf(stderr, "Missing value for %s\n", arg.c_str());
            std::exit(1);
        };

        if (arg == "--config")
            config_path = nextVal();
        else if (arg == "--url")
            url = nextVal();
        else if (arg == "--output")
            output = nextVal();
        else if (arg == "--no-preview")
            show_preview = false;
        else if (arg == "--help" || arg == "-h")
        {
            printUsage(argv[0]);
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    if (!show_preview && output.empty())
    {
        std::fprintf(stderr, "Error: --no-preview without --output, nothing to do\n");
        return 1;
    }

    // --- 加载配置 ---
    PipelineConfig pcfg;
    if (!PipelineConfig::load(config_path, pcfg))
    {
        std::fprintf(stderr, "Failed to load config: %s\n", config_path.c_str());
        return 1;
    }
    pcfg.print();

    // --- 包队列 ---
    PacketQueue queue(5000); // 最多缓存 5000 包 (~100 秒)

    // --- 连接 WebSocket ---
    WsReceiver receiver;
    receiver.setQueue(&queue);
    std::printf("Connecting to %s ...\n", url.c_str());
    if (!receiver.connect(url))
    {
        std::fprintf(stderr, "Failed to connect\n");
        return 1;
    }

    // --- 配置解码管线 ---
    DecoderPipeline::Config cfg;
    cfg.reassembler.require_keyframe_after_loss = true;
    cfg.decoder.threads = pcfg.decode_threads;
    cfg.decoder.prefer_dav1d = pcfg.prefer_dav1d;
    cfg.decoder.bitshift = pcfg.bitshift;
    cfg.decoder.reverse_bitshift = (pcfg.bitshift > 0);
    cfg.output_path = output; // 空 = 不保存
    cfg.fps = pcfg.output_fps;
    cfg.show_preview = show_preview;
    cfg.preview_window = "Live Decode";

    DecoderPipeline pipeline(cfg);
    if (!pipeline.open())
    {
        std::fprintf(stderr, "Failed to open decoder pipeline\n");
        return 1;
    }

    // --- 解码循环 ---
    std::printf("\nReceiving and decoding... Press ESC in preview to quit.\n\n");

    uint8_t packet_buf[BitstreamPacketizer::PACKET_SIZE];
    uint64_t packets_processed = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (receiver.connected())
    {
        if (!queue.pop(packet_buf, 100))
        {
            // 超时, 检查预览是否关闭
            if (show_preview && !pipeline.previewActive())
            {
                std::printf("Preview closed by user\n");
                break;
            }
            continue;
        }

        if (!pipeline.pushPacket(packet_buf))
        {
            std::fprintf(stderr, "Pipeline error at packet %llu\n",
                         static_cast<unsigned long long>(packets_processed));
            break;
        }

        ++packets_processed;

        // 进度
        if (packets_processed % 500 == 0)
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            auto st = pipeline.stats();
            std::printf("[Receiver] %llu pkts (queue: %zu, dropped: %llu), "
                        "%llu frames decoded, %.1fs elapsed\n",
                        static_cast<unsigned long long>(packets_processed),
                        queue.size(),
                        static_cast<unsigned long long>(queue.dropped()),
                        static_cast<unsigned long long>(st.decoder.frames_decoded),
                        elapsed);
        }

        // 预览关闭 + 不保存 → 退出
        if (show_preview && !pipeline.previewActive() && output.empty())
        {
            std::printf("Preview closed, stopping\n");
            break;
        }
    }

    // --- 清理 ---
    queue.stop();
    receiver.disconnect();
    pipeline.close();

    auto total_time = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - start_time)
                          .count();
    auto final_stats = pipeline.stats();

    std::printf("\n=== Receiver Summary ===\n");
    std::printf("Packets received: %llu (dropped: %llu)\n",
                static_cast<unsigned long long>(queue.totalReceived()),
                static_cast<unsigned long long>(queue.dropped()));
    std::printf("Packets processed: %llu\n",
                static_cast<unsigned long long>(packets_processed));
    std::printf("Frames decoded: %llu, errors: %llu\n",
                static_cast<unsigned long long>(final_stats.decoder.frames_decoded),
                static_cast<unsigned long long>(final_stats.decoder.decode_errors));
    std::printf("Frames written: %llu, shown: %llu\n",
                static_cast<unsigned long long>(final_stats.frames_written),
                static_cast<unsigned long long>(final_stats.frames_shown));
    std::printf("Duration: %.2f s\n", total_time);
    if (!output.empty() && final_stats.frames_written > 0)
        std::printf("Output: %s\n", output.c_str());

    return 0;
}

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

// Forward declarations for FFmpeg types (avoid leaking FFmpeg headers)
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// 流式 AV1 编码器
// 输入: cv::Mat (CV_8UC1 灰度图 或 CV_8UC3 BGR)
// 输出: 变长编码数据块 (通过回调函数输出)
//
// 使用 libaom-av1 via FFmpeg libavcodec，严格 CBR 模式
// 目标码率: 120kbps (2400bit × 50Hz)
class StreamEncoder
{
public:
    // 编码数据回调: (data, size, is_keyframe, frame_id)
    using EncodedCallback = std::function<void(const uint8_t *data, size_t size,
                                               bool is_keyframe, uint32_t frame_id)>;

    struct Config
    {
        int width = 480;
        int height = 480;
        int fps = 24;
        int bitrate = 120000;     // bit/s, 目标码率
        int max_bitrate = 120000; // bit/s, 最大码率 (CBR 模式下等于 bitrate)
        int buf_size = 60000;     // bit, RC 缓冲区大小 (约 0.5s 的数据)
        int gop_size = 48;        // 关键帧间隔 (2 秒 @ 24fps)
        int cpu_used = 8;         // libaom-av1 cpu-used, 0-8, 越高越快
        int threads = 4;          // 编码线程数
    };

    explicit StreamEncoder(const Config &config);
    ~StreamEncoder();

    // 不允许拷贝
    StreamEncoder(const StreamEncoder &) = delete;
    StreamEncoder &operator=(const StreamEncoder &) = delete;

    // 允许移动
    StreamEncoder(StreamEncoder &&other) noexcept;
    StreamEncoder &operator=(StreamEncoder &&other) noexcept;

    // 设置编码数据回调
    void setCallback(EncodedCallback cb);

    // 送入一帧进行编码
    // gray: CV_8UC1 灰度图, 尺寸必须与 Config 中的 width/height 一致
    // 编码完成后通过回调输出数据
    // 返回: true 成功, false 编码出错
    bool feedFrame(const cv::Mat &gray);

    // 刷新编码器, 输出所有缓存帧
    bool flush();

    // 获取当前帧计数
    uint32_t frameCount() const { return frame_count_; }

    // 获取配置
    const Config &config() const { return config_; }

private:
    bool initEncoder();
    void cleanup();
    bool receivePackets();
    bool convertMatToFrame(const cv::Mat &gray);

    Config config_;
    EncodedCallback callback_;

    AVCodecContext *codec_ctx_ = nullptr;
    AVFrame *av_frame_ = nullptr;
    AVPacket *av_packet_ = nullptr;

    uint32_t frame_count_ = 0;
    bool initialized_ = false;
};

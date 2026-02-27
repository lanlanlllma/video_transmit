#pragma once

#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

// Forward declarations (避免泄漏 FFmpeg 头文件)
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// 流式 AV1 解码器
// 与 StreamEncoder 对称: 编码器输入帧输出码流, 解码器输入码流输出帧
//
// 输入: 完整编码帧 (重组后的 AV1 OBU 比特流)
// 输出: cv::Mat (CV_8UC1 灰度图)
//
// 使用 libdav1d (优先) 或 libaom-av1 via FFmpeg libavcodec
// 无需 AV1 parser — 输入已是完整帧边界

class StreamDecoder
{
public:
    struct Config
    {
        int threads = 0;              // 解码线程数 (0 = FFmpeg 默认)
        bool prefer_dav1d = true;     // 优先使用 libdav1d
        int bitshift = 0;             // 比特右移量 (编码端使用的)
        bool reverse_bitshift = true; // 是否反转 bitshift (左移恢复亮度)
    };

    struct Stats
    {
        uint64_t packets_sent = 0;   // 送入解码器的帧数
        uint64_t frames_decoded = 0; // 成功解码的帧数
        uint64_t decode_errors = 0;  // 解码错误次数
        uint64_t flushes = 0;        // flush 次数
    };

    explicit StreamDecoder(const Config &config);
    ~StreamDecoder();

    // 不允许拷贝
    StreamDecoder(const StreamDecoder &) = delete;
    StreamDecoder &operator=(const StreamDecoder &) = delete;

    // 初始化解码器
    bool init();

    // 重置解码器状态 (丢包恢复时调用)
    void reset();

    // 解码一个完整编码帧
    // data: 重组后的 AV1 OBU 比特流
    // len: 数据长度
    // is_keyframe: 是否关键帧 (辅助 FFmpeg)
    // out_frames: 输出解码后的灰度帧 (通常为 1 帧, 可能为 0)
    // 返回: true 成功, false 解码出错
    bool decodeFrame(const uint8_t *data, size_t len, bool is_keyframe,
                     std::vector<cv::Mat> &out_frames);

    // 刷新解码器, 输出所有缓存帧
    bool flush(std::vector<cv::Mat> &out_frames);

    // 获取统计信息
    const Stats &stats() const { return stats_; }

    // 获取配置
    const Config &config() const { return config_; }

private:
    // 从 AVFrame 提取 Y 平面为灰度 cv::Mat
    cv::Mat avFrameToGrayMat(const AVFrame *frame);

    // 反转 bitshift: 饱和左移恢复亮度
    void applyReverseBitshift(cv::Mat &gray);

    // 接收所有解码后的帧
    bool receiveFrames(std::vector<cv::Mat> &out_frames);

    void cleanup();

    Config config_;
    Stats stats_;

    AVCodecContext *codec_ctx_ = nullptr;
    AVFrame *av_frame_ = nullptr;
    AVPacket *av_packet_ = nullptr;
    bool initialized_ = false;
};

#pragma once

#include <functional>
#include <opencv2/highgui.hpp>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "bitstream_packetizer.h"
#include "frame_preprocessor.h"
#include "stream_encoder.h"

// 压缩流管线
// 完整流程: 原始帧 → 预处理 → AV1 编码 → 固定大小分包
//
// 使用方式:
//   CompressedStreamPipeline pipeline(config);
//   pipeline.setPacketCallback([](const auto& pkt) { send(pkt); });
//   pipeline.feedFrame(raw_bgr_frame);  // 输入原始帧
//   auto pkt = pipeline.producePacket(); // 50Hz 调用, 产生固定大小包

class CompressedStreamPipeline
{
public:
    struct Config
    {
        // 编码参数
        int encode_width = 480;
        int encode_height = 480;
        int fps = 24;
        int bitrate = 120000;     // bit/s
        int max_bitrate = 120000; // bit/s
        int gop_size = 48;
        int cpu_used = 8;
        int encoder_threads = 4;

        // 预处理参数 (传递给 FramePreprocessor)
        int blur = 0;
        int posterize = 0;
        double gamma = 1.0;
        std::string lut_type = "none";
        int shadow_threshold = 64;
        double shadow_gain = 1.4;
        bool gray = true;
        double crop_top_ratio = 0.0;
        int bitshift = 4;
        double edge_enhance = 0.0;
        double high_freq_boost = 0.0;
        int shadow_floor = 0;
        int highlight_roof = 255;
        double motion_enhance = 0.0;
        int motion_threshold = 10;
        int motion_dilation = 3;
        double center_crop_ratio = 1.0;
    };

    explicit CompressedStreamPipeline(const Config &config);
    ~CompressedStreamPipeline();

    // 不允许拷贝
    CompressedStreamPipeline(const CompressedStreamPipeline &) = delete;
    CompressedStreamPipeline &operator=(const CompressedStreamPipeline &) = delete;

    // 设置包输出回调
    void setPacketCallback(BitstreamPacketizer::PacketCallback cb);

    // 设置调试帧回调 (在预处理后、编码前调用)
    // 回调参数: 预处理后的灰度图像
    using DebugFrameCallback = std::function<void(const cv::Mat &processed_frame)>;
    void setDebugFrameCallback(DebugFrameCallback cb);

    // 送入一帧原始图像 (BGR 或灰度)
    // 内部执行: 预处理 → 缩放 → 编码 → 推入分包器缓冲
    // 返回: true 成功, false 失败
    bool feedFrame(const cv::Mat &frame);

    // 产生一个固定大小输出包 (50Hz 调用)
    // 内部从分包器缓冲区取数据
    BitstreamPacketizer::Packet producePacket();

    // 刷新编码器 (结束编码时调用)
    bool flush();

    // 获取统计信息
    uint32_t encodedFrames() const;
    uint32_t producedPackets() const;
    size_t bufferedBytes() const;
    size_t droppedBytes() const;

private:
    Config config_;
    std::unique_ptr<FramePreprocessor> preprocessor_;
    std::unique_ptr<StreamEncoder> encoder_;
    std::unique_ptr<BitstreamPacketizer> packetizer_;
    DebugFrameCallback debug_frame_cb_;
};

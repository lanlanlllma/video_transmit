#pragma once

#include <cstdint>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "packet_reassembler.h"
#include "stream_decoder.h"

// 解码管线
// 与 CompressedStreamPipeline 对称: 编码管线打包, 解码管线还原
//
// 完整流程: 300字节包 → 重组 → AV1 解码 → 灰度帧 → 保存视频 + 实时预览
//
// 使用方式:
//   DecoderPipeline pipeline(config);
//   pipeline.open();
//   while (有数据) {
//       pipeline.pushPacket(packet_data);
//   }
//   pipeline.close();

class DecoderPipeline
{
public:
    struct Config
    {
        // 重组器配置
        PacketReassembler::Config reassembler;

        // 解码器配置
        StreamDecoder::Config decoder;

        // 输出视频参数
        std::string output_path;        // 输出视频路径 (空 = 不保存)
        double fps = 24.0;              // 输出帧率
        int fourcc = 0;                 // VideoWriter fourcc (0 = 自动选择)

        // 预览配置
        bool show_preview = true;       // 是否显示预览窗口
        int preview_wait_ms = 1;        // cv::waitKey 等待毫秒
        std::string preview_window = "Decoded"; // 窗口名称
    };

    struct Stats
    {
        PacketReassembler::Stats reassembler;
        StreamDecoder::Stats decoder;
        uint64_t frames_written = 0;    // 写入视频文件的帧数
        uint64_t frames_shown = 0;      // 显示预览的帧数
    };

    explicit DecoderPipeline(const Config &config);
    ~DecoderPipeline();

    // 不允许拷贝
    DecoderPipeline(const DecoderPipeline &) = delete;
    DecoderPipeline &operator=(const DecoderPipeline &) = delete;

    // 打开管线 (初始化解码器)
    bool open();

    // 关闭管线 (刷新解码器, 关闭文件)
    void close();

    // 送入一个 300 字节数据包
    // 内部流程: 重组 → 解码 → 输出
    // 返回: true 成功处理, false 出错
    bool pushPacket(const uint8_t *packet_data);

    // 获取统计信息
    Stats stats() const;

    // 预览是否仍然活跃 (ESC 关闭后变为 false)
    bool previewActive() const { return preview_active_; }

private:
    // 处理解码后的帧: 写文件 + 预览
    void outputFrame(const cv::Mat &gray);

    // 初始化 VideoWriter (延迟到第一帧, 因为需要知道分辨率)
    bool initWriter(int width, int height);

    Config config_;
    PacketReassembler reassembler_;
    StreamDecoder decoder_;

    cv::VideoWriter writer_;
    bool writer_open_ = false;
    bool preview_active_ = false;
    bool need_decoder_reset_ = false; // 丢包恢复标记

    uint64_t frames_written_ = 0;
    uint64_t frames_shown_ = 0;
};

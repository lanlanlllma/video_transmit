#pragma once

#include <cstdint>
#include <opencv2/core.hpp>

// 基于 FFmpeg 的灰度 Mat 视频编码器
// 输入: 单帧灰度图像 (CV_8UC1)
// 输出: 压缩后比特流, 使用 1xN 的 CV_8UC1 Mat 承载
class MatVideoEncoder
{
public:
    // codec_name 例如 "libx264"，width/height 为编码分辨率
    // bitrate: 目标码率, 单位 bit/s
    // fps: 帧率
    MatVideoEncoder(const std::string &codec_name,
                    int width,
                    int height,
                    int bitrate = 60000,
                    int fps = 24);

    ~MatVideoEncoder();

    // 编码一帧灰度图像, 输出比特流
    // gray: CV_8UC1, 尺寸必须与构造时的 width/height 一致
    // bitstream: 1xN, CV_8UC1, 内部会重新分配
    // 返回: true 表示编码成功, false 表示出错
    bool encode(const cv::Mat &gray, cv::Mat &bitstream);

    // flush 编码器剩余数据 (例如 B 帧), 输出比特流
    // 若无剩余数据, 返回的 bitstream 将为空 Mat
    bool flush(cv::Mat &bitstream);

    // 禁止拷贝, 允许移动
    MatVideoEncoder(const MatVideoEncoder &) = delete;
    MatVideoEncoder &operator=(const MatVideoEncoder &) = delete;

    MatVideoEncoder(MatVideoEncoder &&other) noexcept;
    MatVideoEncoder &operator=(MatVideoEncoder &&other) noexcept;

private:
    bool sendFrameAndReceivePackets(struct AVFrame *frame, cv::Mat &bitstream);

    struct AVCodecContext *codec_ctx_ = nullptr;
    struct AVFrame *frame_ = nullptr; // YUV420P 目标帧
    struct AVPacket *packet_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int64_t next_pts_ = 0;
    bool flushed_ = false;
};

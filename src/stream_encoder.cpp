#include "stream_encoder.h"

#include <cstring>
#include <stdexcept>
#include <utility>
#include <opencv2/imgproc.hpp>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

StreamEncoder::StreamEncoder(const Config &config)
    : config_(config)
{
    if (!initEncoder())
    {
        cleanup();
        throw std::runtime_error("StreamEncoder: failed to initialize AV1 encoder");
    }
    initialized_ = true;
}

StreamEncoder::~StreamEncoder()
{
    cleanup();
}

StreamEncoder::StreamEncoder(StreamEncoder &&other) noexcept
    : config_(other.config_),
      callback_(std::move(other.callback_)),
      codec_ctx_(other.codec_ctx_),
      av_frame_(other.av_frame_),
      av_packet_(other.av_packet_),
      frame_count_(other.frame_count_),
      initialized_(other.initialized_)
{
    other.codec_ctx_ = nullptr;
    other.av_frame_ = nullptr;
    other.av_packet_ = nullptr;
    other.initialized_ = false;
}

StreamEncoder &StreamEncoder::operator=(StreamEncoder &&other) noexcept
{
    if (this != &other)
    {
        cleanup();
        config_ = other.config_;
        callback_ = std::move(other.callback_);
        codec_ctx_ = other.codec_ctx_;
        av_frame_ = other.av_frame_;
        av_packet_ = other.av_packet_;
        frame_count_ = other.frame_count_;
        initialized_ = other.initialized_;

        other.codec_ctx_ = nullptr;
        other.av_frame_ = nullptr;
        other.av_packet_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

void StreamEncoder::setCallback(EncodedCallback cb)
{
    callback_ = std::move(cb);
}

bool StreamEncoder::initEncoder()
{
    // 查找 libaom-av1 编码器
    const AVCodec *codec = avcodec_find_encoder_by_name("libaom-av1");
    if (!codec)
    {
        // 回退到通用 AV1 编码器
        codec = avcodec_find_encoder(AV_CODEC_ID_AV1);
    }
    if (!codec)
    {
        std::fprintf(stderr, "StreamEncoder: libaom-av1 codec not found\n");
        return false;
    }

    // 分配编码上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_)
    {
        std::fprintf(stderr, "StreamEncoder: failed to allocate codec context\n");
        return false;
    }

    // 基本视频参数
    codec_ctx_->width = config_.width;
    codec_ctx_->height = config_.height;
    codec_ctx_->time_base = AVRational{1, config_.fps};
    codec_ctx_->framerate = AVRational{config_.fps, 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

    // 严格 CBR 码率控制
    codec_ctx_->bit_rate = config_.bitrate;
    codec_ctx_->rc_max_rate = config_.max_bitrate;
    codec_ctx_->rc_min_rate = config_.bitrate;
    codec_ctx_->rc_buffer_size = config_.buf_size;
    codec_ctx_->rc_initial_buffer_occupancy = config_.buf_size * 3 / 4;

    // GOP 和延迟设置
    codec_ctx_->gop_size = config_.gop_size;
    codec_ctx_->max_b_frames = 0; // 无 B 帧, 最低延迟
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // 多线程
    codec_ctx_->thread_count = config_.threads;

    // libaom-av1 特定选项 (必须在 avcodec_open2 之前设置)
    av_opt_set(codec_ctx_->priv_data, "cpu-used",
               std::to_string(config_.cpu_used).c_str(), 0);
    av_opt_set(codec_ctx_->priv_data, "usage", "realtime", 0);
    av_opt_set(codec_ctx_->priv_data, "row-mt", "1", 0);
    av_opt_set(codec_ctx_->priv_data, "tiles", "2x2", 0);

    // 打开编码器
    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0)
    {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "StreamEncoder: failed to open codec: %s\n", err_buf);
        return false;
    }

    // 分配 AVFrame
    av_frame_ = av_frame_alloc();
    if (!av_frame_)
    {
        std::fprintf(stderr, "StreamEncoder: failed to allocate AVFrame\n");
        return false;
    }
    av_frame_->format = codec_ctx_->pix_fmt;
    av_frame_->width = codec_ctx_->width;
    av_frame_->height = codec_ctx_->height;

    ret = av_image_alloc(av_frame_->data, av_frame_->linesize,
                         codec_ctx_->width, codec_ctx_->height,
                         AV_PIX_FMT_YUV420P, 32);
    if (ret < 0)
    {
        std::fprintf(stderr, "StreamEncoder: failed to allocate frame buffer\n");
        return false;
    }

    // 分配 AVPacket
    av_packet_ = av_packet_alloc();
    if (!av_packet_)
    {
        std::fprintf(stderr, "StreamEncoder: failed to allocate AVPacket\n");
        return false;
    }

    std::printf("StreamEncoder: initialized libaom-av1, %dx%d @ %dfps, "
                "bitrate=%d bps, gop=%d, cpu-used=%d\n",
                config_.width, config_.height, config_.fps,
                config_.bitrate, config_.gop_size, config_.cpu_used);

    return true;
}

void StreamEncoder::cleanup()
{
    if (av_packet_)
    {
        av_packet_free(&av_packet_);
    }
    if (av_frame_)
    {
        if (av_frame_->data[0])
        {
            av_freep(&av_frame_->data[0]);
        }
        av_frame_free(&av_frame_);
    }
    if (codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
    }
    initialized_ = false;
}

bool StreamEncoder::convertMatToFrame(const cv::Mat &gray)
{
    if (gray.empty() || !av_frame_)
    {
        return false;
    }

    // 处理输入: 如果是 BGR 先转灰度
    cv::Mat input_gray;
    if (gray.channels() == 3)
    {
        cv::cvtColor(gray, input_gray, cv::COLOR_BGR2GRAY);
    }
    else if (gray.channels() == 1)
    {
        input_gray = gray;
    }
    else
    {
        std::fprintf(stderr, "StreamEncoder: unsupported channel count: %d\n",
                     gray.channels());
        return false;
    }

    // 尺寸检查
    if (input_gray.cols != config_.width || input_gray.rows != config_.height)
    {
        std::fprintf(stderr, "StreamEncoder: frame size mismatch: got %dx%d, "
                             "expected %dx%d\n",
                     input_gray.cols, input_gray.rows,
                     config_.width, config_.height);
        return false;
    }

    // 灰度 -> YUV420P:
    // Y 平面 = 灰度数据 (直接拷贝)
    // U/V 平面 = 128 (中性色度)
    // 这比 sws_scale 快, 且灰度不需要色度信息
    for (int y = 0; y < config_.height; ++y)
    {
        std::memcpy(av_frame_->data[0] + y * av_frame_->linesize[0],
                    input_gray.ptr<uint8_t>(y),
                    config_.width);
    }

    int uv_height = config_.height / 2;
    std::memset(av_frame_->data[1], 128,
                av_frame_->linesize[1] * uv_height); // U 平面
    std::memset(av_frame_->data[2], 128,
                av_frame_->linesize[2] * uv_height); // V 平面

    av_frame_->pts = static_cast<int64_t>(frame_count_);

    return true;
}

bool StreamEncoder::receivePackets()
{
    while (true)
    {
        int ret = avcodec_receive_packet(codec_ctx_, av_packet_);

        if (ret == AVERROR(EAGAIN))
        {
            // 需要更多输入帧
            return true;
        }
        if (ret == AVERROR_EOF)
        {
            // 编码器已刷新完毕
            return true;
        }
        if (ret < 0)
        {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::fprintf(stderr, "StreamEncoder: receive_packet error: %s\n",
                         err_buf);
            return false;
        }

        // 输出编码数据
        if (callback_ && av_packet_->size > 0)
        {
            bool is_keyframe = (av_packet_->flags & AV_PKT_FLAG_KEY) != 0;
            callback_(av_packet_->data,
                      static_cast<size_t>(av_packet_->size),
                      is_keyframe,
                      frame_count_ > 0 ? frame_count_ - 1 : 0);
        }

        av_packet_unref(av_packet_);
    }
}

bool StreamEncoder::feedFrame(const cv::Mat &gray)
{
    if (!initialized_)
    {
        std::fprintf(stderr, "StreamEncoder: not initialized\n");
        return false;
    }

    if (!convertMatToFrame(gray))
    {
        return false;
    }

    int ret = avcodec_send_frame(codec_ctx_, av_frame_);
    if (ret < 0)
    {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "StreamEncoder: send_frame error: %s\n", err_buf);
        return false;
    }

    ++frame_count_;

    return receivePackets();
}

bool StreamEncoder::flush()
{
    if (!initialized_)
    {
        return true;
    }

    // 发送 NULL 帧表示结束
    int ret = avcodec_send_frame(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF)
    {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "StreamEncoder: flush send error: %s\n", err_buf);
        return false;
    }

    return receivePackets();
}

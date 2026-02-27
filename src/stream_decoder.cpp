#include "stream_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

StreamDecoder::StreamDecoder(const Config &config)
    : config_(config)
{
}

StreamDecoder::~StreamDecoder()
{
    cleanup();
}

bool StreamDecoder::init()
{
    // 查找 AV1 解码器: libdav1d 优先 (更快)
    const AVCodec *codec = nullptr;

    if (config_.prefer_dav1d)
    {
        codec = avcodec_find_decoder_by_name("libdav1d");
        if (codec)
        {
            std::printf("StreamDecoder: using libdav1d decoder\n");
        }
    }

    if (!codec)
    {
        codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
        if (codec)
        {
            std::printf("StreamDecoder: using %s decoder\n", codec->name);
        }
    }

    if (!codec)
    {
        std::fprintf(stderr, "StreamDecoder: AV1 decoder not found\n");
        return false;
    }

    // 分配解码上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_)
    {
        std::fprintf(stderr, "StreamDecoder: failed to allocate codec context\n");
        return false;
    }

    // 解码线程
    if (config_.threads > 0)
    {
        codec_ctx_->thread_count = config_.threads;
    }

    // 打开解码器
    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0)
    {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "StreamDecoder: failed to open codec: %s\n", err_buf);
        cleanup();
        return false;
    }

    // 分配 AVFrame
    av_frame_ = av_frame_alloc();
    if (!av_frame_)
    {
        std::fprintf(stderr, "StreamDecoder: failed to allocate AVFrame\n");
        cleanup();
        return false;
    }

    // 分配 AVPacket
    av_packet_ = av_packet_alloc();
    if (!av_packet_)
    {
        std::fprintf(stderr, "StreamDecoder: failed to allocate AVPacket\n");
        cleanup();
        return false;
    }

    initialized_ = true;
    std::printf("StreamDecoder: initialized (bitshift=%d, reverse=%s)\n",
                config_.bitshift,
                config_.reverse_bitshift ? "yes" : "no");
    return true;
}

void StreamDecoder::reset()
{
    if (initialized_ && codec_ctx_)
    {
        avcodec_flush_buffers(codec_ctx_);
        ++stats_.flushes;
        std::printf("StreamDecoder: flushed decoder buffers\n");
    }
}

bool StreamDecoder::decodeFrame(const uint8_t *data, size_t len, bool is_keyframe,
                                std::vector<cv::Mat> &out_frames)
{
    if (!initialized_ || !data || len == 0)
    {
        return false;
    }

    // 设置 AVPacket 数据 (不拷贝, 引用外部缓冲区)
    av_packet_unref(av_packet_);
    av_packet_->data = const_cast<uint8_t *>(data);
    av_packet_->size = static_cast<int>(len);

    if (is_keyframe)
    {
        av_packet_->flags |= AV_PKT_FLAG_KEY;
    }

    // 送入解码器
    int ret = avcodec_send_packet(codec_ctx_, av_packet_);
    ++stats_.packets_sent;

    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "StreamDecoder: send_packet error: %s\n", err_buf);
        ++stats_.decode_errors;
        return false;
    }

    // 接收解码帧
    return receiveFrames(out_frames);
}

bool StreamDecoder::flush(std::vector<cv::Mat> &out_frames)
{
    if (!initialized_)
    {
        return true;
    }

    // 发送 NULL 表示结束
    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF)
    {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::fprintf(stderr, "StreamDecoder: flush send error: %s\n", err_buf);
        return false;
    }

    return receiveFrames(out_frames);
}

bool StreamDecoder::receiveFrames(std::vector<cv::Mat> &out_frames)
{
    while (true)
    {
        int ret = avcodec_receive_frame(codec_ctx_, av_frame_);

        if (ret == AVERROR(EAGAIN))
        {
            // 需要更多输入
            return true;
        }
        if (ret == AVERROR_EOF)
        {
            return true;
        }
        if (ret < 0)
        {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::fprintf(stderr, "StreamDecoder: receive_frame error: %s\n", err_buf);
            ++stats_.decode_errors;
            return false;
        }

        // 转换为 cv::Mat
        cv::Mat gray = avFrameToGrayMat(av_frame_);
        av_frame_unref(av_frame_);

        if (gray.empty())
        {
            continue;
        }

        // 反转 bitshift
        if (config_.reverse_bitshift && config_.bitshift > 0)
        {
            applyReverseBitshift(gray);
        }

        out_frames.push_back(std::move(gray));
        ++stats_.frames_decoded;
    }
}

cv::Mat StreamDecoder::avFrameToGrayMat(const AVFrame *frame)
{
    if (!frame || !frame->data[0])
    {
        return cv::Mat();
    }

    int width = frame->width;
    int height = frame->height;
    int stride = frame->linesize[0];

    // Y 平面即灰度数据, 直接拷贝
    cv::Mat y_plane(height, width, CV_8UC1, frame->data[0], stride);
    return y_plane.clone(); // 必须 clone, AVFrame 会被复用
}

void StreamDecoder::applyReverseBitshift(cv::Mat &gray)
{
    if (gray.empty() || config_.bitshift <= 0)
    {
        return;
    }

    // 饱和左移: min(pixel << bitshift, 255)
    int shift = config_.bitshift;
    int rows = gray.rows;
    int cols = gray.cols;

    // 连续内存优化
    if (gray.isContinuous())
    {
        cols *= rows;
        rows = 1;
    }

    for (int y = 0; y < rows; ++y)
    {
        uint8_t *ptr = gray.ptr<uint8_t>(y);
        for (int x = 0; x < cols; ++x)
        {
            int val = static_cast<int>(ptr[x]) << shift;
            ptr[x] = static_cast<uint8_t>(std::min(val, 255));
        }
    }
}

void StreamDecoder::cleanup()
{
    if (av_packet_)
    {
        av_packet_free(&av_packet_);
    }
    if (av_frame_)
    {
        av_frame_free(&av_frame_);
    }
    if (codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
    }
    initialized_ = false;
}

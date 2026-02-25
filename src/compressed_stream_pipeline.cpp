#include "compressed_stream_pipeline.h"

#include <cstdio>
#include <opencv2/imgproc.hpp>

CompressedStreamPipeline::CompressedStreamPipeline(const Config &config)
    : config_(config)
{
    // 创建预处理器
    preprocessor_ = std::make_unique<FramePreprocessor>(
        config.blur,
        config.posterize,
        config.gamma,
        config.lut_type,
        config.shadow_threshold,
        config.shadow_gain,
        config.gray,
        config.crop_top_ratio,
        config.bitshift,
        config.edge_enhance,
        config.high_freq_boost,
        config.shadow_floor,
        config.motion_enhance,
        config.motion_threshold,
        config.motion_dilation,
        config.highlight_roof);

    // 创建编码器
    StreamEncoder::Config enc_cfg;
    enc_cfg.width = config.encode_width;
    enc_cfg.height = config.encode_height;
    enc_cfg.fps = config.fps;
    enc_cfg.bitrate = config.bitrate;
    enc_cfg.max_bitrate = config.max_bitrate;
    enc_cfg.buf_size = config.bitrate / 2; // 0.5 秒缓冲
    enc_cfg.gop_size = config.gop_size;
    enc_cfg.cpu_used = config.cpu_used;
    enc_cfg.threads = config.encoder_threads;

    encoder_ = std::make_unique<StreamEncoder>(enc_cfg);

    // 创建分包器
    packetizer_ = std::make_unique<BitstreamPacketizer>();

    // 连接编码器输出 -> 分包器输入
    encoder_->setCallback(
        [this](const uint8_t *data, size_t size,
               bool is_keyframe, uint32_t frame_id)
        {
            packetizer_->pushEncodedData(data, size, frame_id, is_keyframe);
        });

    std::printf("CompressedStreamPipeline: initialized\n"
                "  encode: %dx%d @ %dfps, bitrate=%d bps\n"
                "  packet: %zu bytes @ %d Hz\n"
                "  preprocess: gray=%d, bitshift=%d, gamma=%.1f\n",
                config.encode_width, config.encode_height,
                config.fps, config.bitrate,
                BitstreamPacketizer::PACKET_SIZE,
                BitstreamPacketizer::SEND_RATE_HZ,
                config.gray ? 1 : 0, config.bitshift, config.gamma);
}

CompressedStreamPipeline::~CompressedStreamPipeline() = default;

void CompressedStreamPipeline::setPacketCallback(BitstreamPacketizer::PacketCallback cb)
{
    packetizer_->setCallback(std::move(cb));
}

void CompressedStreamPipeline::setDebugFrameCallback(DebugFrameCallback cb)
{
    debug_frame_cb_ = std::move(cb);
}

bool CompressedStreamPipeline::feedFrame(const cv::Mat &frame)
{
    if (frame.empty())
    {
        std::fprintf(stderr, "CompressedStreamPipeline: empty frame\n");
        return false;
    }

    // 预处理: 裁剪 → 灰度 → 增强 → bitshift
    cv::Mat processed = preprocessor_->processFullPipeline(
        frame, cv::Size(), config_.center_crop_ratio);

    // 调试回调: 输出预处理后的图像
    if (debug_frame_cb_)
    {
        debug_frame_cb_(processed);
    }

    if (processed.empty())
    {
        std::fprintf(stderr, "CompressedStreamPipeline: preprocessing failed\n");
        return false;
    }

    // 缩放到编码分辨率
    cv::Mat resized;
    if (processed.cols != config_.encode_width ||
        processed.rows != config_.encode_height)
    {
        cv::resize(processed, resized,
                   cv::Size(config_.encode_width, config_.encode_height),
                   0, 0, cv::INTER_AREA);
    }
    else
    {
        resized = processed;
    }

    // 如果是 BGR, 转灰度 (编码器期望灰度输入)
    cv::Mat gray;
    if (resized.channels() == 3)
    {
        cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
    }
    else
    {
        gray = resized;
    }

    // 编码 (回调会自动将数据推入分包器)
    return encoder_->feedFrame(gray);
}

BitstreamPacketizer::Packet CompressedStreamPipeline::producePacket()
{
    return packetizer_->producePacket();
}

bool CompressedStreamPipeline::flush()
{
    return encoder_->flush();
}

uint32_t CompressedStreamPipeline::encodedFrames() const
{
    return encoder_->frameCount();
}

uint32_t CompressedStreamPipeline::producedPackets() const
{
    return packetizer_->totalPackets();
}

size_t CompressedStreamPipeline::bufferedBytes() const
{
    return packetizer_->bufferedBytes();
}

size_t CompressedStreamPipeline::droppedBytes() const
{
    return packetizer_->droppedBytes();
}

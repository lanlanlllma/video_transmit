#include "decoder_pipeline.h"

#include <cstdio>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

DecoderPipeline::DecoderPipeline(const Config &config)
    : config_(config),
      reassembler_(config.reassembler),
      decoder_(config.decoder),
      preview_active_(config.show_preview)
{
}

DecoderPipeline::~DecoderPipeline()
{
    close();
}

bool DecoderPipeline::open()
{
    if (!decoder_.init())
    {
        std::fprintf(stderr, "DecoderPipeline: failed to initialize decoder\n");
        return false;
    }

    std::printf("DecoderPipeline: initialized\n"
                "  output: %s\n"
                "  preview: %s\n"
                "  bitshift: %d (reverse: %s)\n",
                config_.output_path.empty() ? "(none)" : config_.output_path.c_str(),
                config_.show_preview ? "yes" : "no",
                config_.decoder.bitshift,
                config_.decoder.reverse_bitshift ? "yes" : "no");

    return true;
}

void DecoderPipeline::close()
{
    // 刷新解码器中缓存的帧
    std::vector<cv::Mat> remaining;
    decoder_.flush(remaining);
    for (const auto &frame : remaining)
    {
        outputFrame(frame);
    }

    if (writer_open_)
    {
        writer_.release();
        writer_open_ = false;
        std::printf("DecoderPipeline: video writer closed (%llu frames)\n",
                    static_cast<unsigned long long>(frames_written_));
    }

    if (preview_active_)
    {
        cv::destroyWindow(config_.preview_window);
        preview_active_ = false;
    }
}

bool DecoderPipeline::pushPacket(const uint8_t *packet_data)
{
    if (!packet_data)
    {
        return false;
    }

    // 重组: 包 → 完整编码帧
    auto frame_opt = reassembler_.pushPacket(packet_data);

    if (!frame_opt)
    {
        // 检查是否发生了丢包 (stats 中 seq_gaps 增加)
        // 如果重组器进入 DROP_UNTIL_KEYFRAME 状态, 需要在恢复时 reset 解码器
        const auto &rstats = reassembler_.stats();
        if (rstats.seq_gaps > 0 && !need_decoder_reset_)
        {
            need_decoder_reset_ = true;
        }
        return true; // 不是错误, 只是还没组装完
    }

    const auto &encoded_frame = *frame_opt;

    // 丢包恢复: 在关键帧到来时重置解码器
    if (need_decoder_reset_ && encoded_frame.keyframe)
    {
        decoder_.reset();
        need_decoder_reset_ = false;
    }

    // 解码: AV1 比特流 → cv::Mat
    std::vector<cv::Mat> decoded;
    bool ok = decoder_.decodeFrame(encoded_frame.data.data(),
                                   encoded_frame.data.size(),
                                   encoded_frame.keyframe,
                                   decoded);

    if (!ok)
    {
        std::fprintf(stderr, "DecoderPipeline: decode failed for frame_id=%u\n",
                     encoded_frame.frame_id);
        // 解码失败不终止管线, 等待下一个关键帧恢复
        need_decoder_reset_ = true;
        return true;
    }

    // 输出解码帧
    for (const auto &gray : decoded)
    {
        outputFrame(gray);
    }

    return true;
}

DecoderPipeline::Stats DecoderPipeline::stats() const
{
    Stats s;
    s.reassembler = reassembler_.stats();
    s.decoder = decoder_.stats();
    s.frames_written = frames_written_;
    s.frames_shown = frames_shown_;
    return s;
}

void DecoderPipeline::outputFrame(const cv::Mat &gray)
{
    if (gray.empty())
    {
        return;
    }

    // 写入视频文件
    if (!config_.output_path.empty())
    {
        if (!writer_open_)
        {
            if (!initWriter(gray.cols, gray.rows))
            {
                std::fprintf(stderr, "DecoderPipeline: failed to init video writer\n");
            }
        }

        if (writer_open_)
        {
            // VideoWriter 需要 BGR, 灰度转 BGR
            cv::Mat bgr;
            cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
            writer_.write(bgr);
            ++frames_written_;
        }
    }

    // 实时预览
    if (preview_active_)
    {
        cv::imshow(config_.preview_window, gray);
        int key = cv::waitKey(config_.preview_wait_ms);
        if (key == 27) // ESC 关闭预览
        {
            preview_active_ = false;
            cv::destroyWindow(config_.preview_window);
            std::printf("DecoderPipeline: preview closed by user\n");
        }
        ++frames_shown_;
    }
}

bool DecoderPipeline::initWriter(int width, int height)
{
    if (config_.output_path.empty())
    {
        return false;
    }

    int fourcc = config_.fourcc;
    if (fourcc == 0)
    {
        // 自动选择: 尝试 H264, 回退到 MPEG4
        fourcc = cv::VideoWriter::fourcc('a', 'v', 'c', '1');
    }

    writer_.open(config_.output_path, fourcc, config_.fps,
                 cv::Size(width, height), true); // isColor=true (BGR)

    if (!writer_.isOpened())
    {
        // 回退到 MPEG4
        fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
        writer_.open(config_.output_path, fourcc, config_.fps,
                     cv::Size(width, height), true);
    }

    if (!writer_.isOpened())
    {
        std::fprintf(stderr, "DecoderPipeline: failed to open video writer: %s\n",
                     config_.output_path.c_str());
        return false;
    }

    writer_open_ = true;
    std::printf("DecoderPipeline: video writer opened: %s (%dx%d @ %.1f fps)\n",
                config_.output_path.c_str(), width, height, config_.fps);
    return true;
}

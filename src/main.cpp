#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "compressed_stream_pipeline.h"
#include "pipeline_config.h"

// 显示用法说明
static void printUsage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s <input_video> [options]\n"
                 "\n"
                 "Options:\n"
                 "  --config <path>         配置文件路径 (默认: ./config.yaml)\n"
                 "  --output <path>         输出文件路径 (默认: output_packets.bin)\n"
                 "  --max-frames <int>      最大编码帧数 (默认: 无限制)\n"
                 "  --realtime              模拟实时 50Hz 包输出\n"
                 "  --debug                 显示预处理后的图像预览窗口\n"
                 "\n"
                 "所有编码/预处理参数从配置文件读取, 与解码器共享\n"
                 "输出: 固定 300 字节/包, 50Hz 发送频率, 总带宽 120kbps\n",
                 prog);
}

// 命令行参数 (仅运行时参数, 编码配置从 YAML 读取)
struct Args
{
    std::string input;
    std::string output = "output_packets.bin";
    std::string config = "config.yaml";
    int max_frames = 0; // 0 = unlimited
    bool realtime = false;
    bool debug = false;
};

static Args parseArgs(int argc, char **argv)
{
    Args args;
    if (argc < 2)
    {
        printUsage(argv[0]);
        std::exit(1);
    }

    args.input = argv[1];

    for (int i = 2; i < argc; ++i)
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
            args.config = nextVal();
        else if (arg == "--output")
            args.output = nextVal();
        else if (arg == "--max-frames")
            args.max_frames = std::atoi(nextVal());
        else if (arg == "--realtime")
            args.realtime = true;
        else if (arg == "--debug")
            args.debug = true;
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    return args;
}

int main(int argc, char **argv)
{
    Args args = parseArgs(argc, argv);

    // 加载共享配置
    PipelineConfig pcfg;
    if (!PipelineConfig::load(args.config, pcfg))
    {
      std::fprintf(csv, "frame,encode_time_ms,bytes\n");
    }
    else
    {
      std::perror("fopen csv");
    }
  }

  try
  {
    // 打开视频源（当前默认使用 videos/test_video1.mp4）
    const std::string input_video = "./videos/test_video1.mp4";
    cv::VideoCapture cap(input_video);
    if (!cap.isOpened())
    {
        std::fprintf(stderr, "Failed to open input video: %s\n", args.input.c_str());
        return 1;
    }

    double src_fps = cap.get(cv::CAP_PROP_FPS);
    if (src_fps <= 0.0)
        src_fps = pcfg.input_fps;
    int total_frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    int src_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int src_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

    // 帧率转换: 使用配置中的 input_fps 作为基准
    double input_fps = pcfg.input_fps;
    double encode_fps = static_cast<double>(pcfg.encode_fps);

    std::printf("Input: %s (%dx%d, %.2f fps, %d frames)\n",
                args.input.c_str(), src_width, src_height, src_fps, total_frames);
    std::printf("Config: input_fps=%.1f, encode_fps=%.0f\n", input_fps, encode_fps);
    // std::printf("Output: %s\n", args.output.c_str());
    std::printf("Encode: %dx%d @ %dfps, bitrate=%d bps, gop=%d\n",
                pcfg.encode_width, pcfg.encode_height, pcfg.encode_fps,
                pcfg.bitrate, pcfg.gop);
    std::printf("Packet: %zu bytes @ %d Hz = %d bps\n",
                BitstreamPacketizer::PACKET_SIZE,
                BitstreamPacketizer::SEND_RATE_HZ,
                static_cast<int>(BitstreamPacketizer::PACKET_SIZE * 8 *
                                 BitstreamPacketizer::SEND_RATE_HZ));

    // 预处理参数，参考 compress_av1.py 注释示例
    FramePreprocessor preprocessor(
        /*blur=*/0,
        /*posterize=*/0,
        /*gamma=*/2.0,
        /*lut_type=*/"shadow",
        /*shadow_threshold=*/50,
        /*shadow_gain=*/8.0,
        /*gray=*/true,
        /*crop_top_ratio=*/0.0,
        /*bitshift=*/4,
        /*edge_enhance=*/1.0,
        /*high_freq_boost=*/1.0,
        /*shadow_floor=*/30,
        /*motion_enhance=*/1.0,
        /*motion_threshold=*/3,
        /*motion_dilation=*/5,
        /*highlight_roof=*/125);

    // AV1 码率设置，参考示例 --bitrate 60000（单位 bit/s）
    int target_bitrate = 60000;
    MatVideoEncoder encoder(codec_name, width, height, target_bitrate, static_cast<int>(target_fps));

    int src_frame_index = 0;
    int encoded_frames = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true)
    {
        cv::Mat bgr;
        if (!cap.read(bgr) || bgr.empty())
        {
            break;
        }

      ++src_frame_index;
      if (src_frame_index % frame_skip != 0)
      {
        continue; // 跳帧以降低帧率
      }

      // 预处理管线：裁剪/灰度/增强等
      // 拆成“bitshift 前”和“bitshift 后”两步，方便 debug 可视化
      double center_ratio = 1.0;
      cv::Mat pre = preprocessor.cropTop(bgr);
      pre = preprocessor.cropCenterRatio(pre, center_ratio);
      pre = preprocessor.toGrayscale(pre);
      pre = preprocessor.process(pre);

      if (debug)
      {
        cv::imshow("preprocessed_before_bitshift", pre);
        // 使用很短的等待时间，避免严重阻塞编码流程
        int key = cv::waitKey(1);
        if (key == 27) // ESC 退出预览但继续编码
        {
          // 用户可按 ESC 停止后续显示
          debug = false;
        }
      }

      cv::Mat processed = preprocessor.applyBitshift(pre);

      // 调整到编码分辨率
      cv::Mat resized;
      if (processed.cols != width || processed.rows != height)
      {
        cv::resize(processed, resized, cv::Size(width, height), 0, 0, cv::INTER_AREA);
      }
      else
      {
        resized = processed;
      }

      // MatVideoEncoder 目前接受 CV_8UC1（灰度）
      cv::Mat gray;
      if (resized.channels() == 3)
      {
        cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
      }
      else
      {
        gray = resized;
      }

      cv::Mat bitstream;
      auto start = std::chrono::high_resolution_clock::now();
      if (!encoder.encode(gray, bitstream))
      {
        std::fprintf(stderr, "encode() failed on frame %d\n", encoded_frames);
        std::fclose(f);
        if (csv)
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            int expected_encode = static_cast<int>(
                std::round(static_cast<double>(total_frames) * encode_fps / input_fps));
            std::printf("Progress: %d/%d frames encoded, %zu packets, "
                        "buffer: %zu bytes, elapsed: %.1fs (%.1f fps)\n",
                        encoded_frames, expected_encode,
                        total_packets_written,
                        pipeline.bufferedBytes(),
                        elapsed,
                        encoded_frames / elapsed);
        }

        if (args.max_frames > 0 && encoded_frames >= args.max_frames)
        {
            break;
        }
    }

    cap.release();

    // 刷新编码器
    std::printf("Flushing encoder...\n");
    pipeline.flush();

    // 排空剩余缓冲区
    while (pipeline.bufferedBytes() > 0)
    {
        pipeline.producePacket();
    }

    std::fclose(out_file);

    auto end_time = std::chrono::steady_clock::now();
    double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();

    std::printf("\n=== Summary ===\n");
    std::printf("Encoded frames: %d (from %d input frames)\n",
                encoded_frames, src_frame_index);
    std::printf("Frame rate: %.1f -> %d fps\n", input_fps, pcfg.encode_fps);
    std::printf("Total packets: %zu\n", total_packets_written);
    std::printf("Dropped bytes: %zu\n", pipeline.droppedBytes());
    std::printf("Total time: %.2f s\n", total_elapsed);
    std::printf("Avg encoding fps: %.1f\n",
                encoded_frames / total_elapsed);
    std::printf("Output file: %s (%zu bytes)\n",
                args.output.c_str(),
                total_packets_written * BitstreamPacketizer::PACKET_SIZE);
    std::printf("Effective bitrate: %.1f kbps\n",
                total_packets_written * BitstreamPacketizer::PACKET_SIZE * 8.0 /
                    total_elapsed / 1000.0);
    std::printf("Overall bitrate (including dropped data): %.1f kbps\n",
                (total_packets_written * BitstreamPacketizer::PACKET_SIZE + pipeline.droppedBytes()) * 8.0 /
                    total_elapsed / 1000.0);

    return 0;
}

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
        std::fprintf(stderr, "Failed to load config: %s\n", args.config.c_str());
        return 1;
    }
    pcfg.print();

    // 确保编码分辨率为偶数 (YUV420P 要求)
    if (pcfg.encode_width % 2 != 0)
        --pcfg.encode_width;
    if (pcfg.encode_height % 2 != 0)
        --pcfg.encode_height;

    // 打开输入视频
    cv::VideoCapture cap(args.input);
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

    // 配置管线
    CompressedStreamPipeline::Config cfg;
    cfg.encode_width = pcfg.encode_width;
    cfg.encode_height = pcfg.encode_height;
    cfg.fps = pcfg.encode_fps;
    cfg.bitrate = pcfg.bitrate;
    cfg.max_bitrate = pcfg.bitrate;
    cfg.gop_size = pcfg.gop;
    cfg.cpu_used = pcfg.cpu_used;
    cfg.encoder_threads = pcfg.encoder_threads;

    cfg.gray = true;
    cfg.bitshift = pcfg.bitshift;
    cfg.gamma = pcfg.gamma;
    cfg.lut_type = (pcfg.shadow_gain > 1.0) ? "shadow" : "none";
    cfg.shadow_gain = pcfg.shadow_gain;
    cfg.shadow_threshold = pcfg.shadow_threshold;
    cfg.shadow_floor = pcfg.shadow_floor;
    cfg.highlight_roof = pcfg.highlight_roof;
    cfg.edge_enhance = pcfg.edge_enhance;
    cfg.high_freq_boost = pcfg.high_freq_boost;
    cfg.motion_enhance = pcfg.motion_enhance;
    cfg.center_crop_ratio = pcfg.center_crop;

    CompressedStreamPipeline pipeline(cfg);

    // 调试模式: 显示预处理后的图像
    bool debug_active = args.debug;
    if (debug_active)
    {
        pipeline.setDebugFrameCallback(
            [&debug_active](const cv::Mat &processed)
            {
                if (!debug_active)
                    return;
                cv::imshow("Preprocessed", processed);
                int key = cv::waitKey(1);
                if (key == 27) // ESC 关闭预览
                {
                    debug_active = false;
                    cv::destroyWindow("Preprocessed");
                }
            });
        std::printf("Debug: preview window enabled (press ESC to close)\n");
    }

    // 打开输出文件
    FILE *out_file = std::fopen(args.output.c_str(), "wb");
    if (!out_file)
    {
        std::perror("fopen output");
        return 1;
    }

    // 设置包输出回调: 写入文件
    size_t total_packets_written = 0;
    pipeline.setPacketCallback(
        [&](const BitstreamPacketizer::Packet &pkt)
        {
            std::fwrite(pkt.data, 1, BitstreamPacketizer::PACKET_SIZE, out_file);
            ++total_packets_written;
        });

    // 帧率转换: 累加器方式
    // 每读取一帧输入, 累加 encode_fps; 当累加值 >= input_fps 时, 编码该帧
    // 这种方式均匀采样, 不会出现整数跳帧的不均匀问题
    double fps_accumulator = 0.0;
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

        // 累加器帧率转换
        fps_accumulator += encode_fps;
        if (fps_accumulator < input_fps)
        {
            continue; // 跳过此帧
        }
        fps_accumulator -= input_fps;

        // 送入管线 (预处理 + 编码)
        if (!pipeline.feedFrame(bgr))
        {
            std::fprintf(stderr, "Pipeline feedFrame failed at frame %d\n",
                         encoded_frames);
            break;
        }

        ++encoded_frames;

        // 产生输出包 (根据帧率和包速率的比例)
        int packets_this_frame = (encoded_frames * BitstreamPacketizer::SEND_RATE_HZ / pcfg.encode_fps) -
                                 ((encoded_frames - 1) * BitstreamPacketizer::SEND_RATE_HZ / pcfg.encode_fps);
        for (int p = 0; p < packets_this_frame; ++p)
        {
            auto pkt = pipeline.producePacket();

            if (args.realtime)
            {
                // 模拟 50Hz 实时发送
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        if (encoded_frames % 30 == 0)
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

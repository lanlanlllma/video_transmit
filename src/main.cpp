#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "bitstream_packetizer.h"
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

    // 打开视频源
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

    double input_fps = pcfg.input_fps;
    double encode_fps = static_cast<double>(pcfg.encode_fps);

    std::printf("Input: %s (%dx%d, %.2f fps, %d frames)\n",
                args.input.c_str(), src_width, src_height, src_fps, total_frames);
    std::printf("Encode: %dx%d @ %dfps, bitrate=%d bps, gop=%d\n",
                pcfg.encode_width, pcfg.encode_height, pcfg.encode_fps,
                pcfg.bitrate, pcfg.gop);
    std::printf("Packet: %zu bytes @ %d Hz = %d bps\n",
                BitstreamPacketizer::PACKET_SIZE,
                BitstreamPacketizer::SEND_RATE_HZ,
                static_cast<int>(BitstreamPacketizer::PACKET_SIZE * 8 *
                                 BitstreamPacketizer::SEND_RATE_HZ));

    // 构建管线配置 (从共享配置映射)
    CompressedStreamPipeline::Config cfg;
    cfg.encode_width = pcfg.encode_width;
    cfg.encode_height = pcfg.encode_height;
    cfg.fps = pcfg.encode_fps;
    cfg.bitrate = pcfg.bitrate;
    cfg.max_bitrate = pcfg.bitrate;
    cfg.gop_size = pcfg.gop;
    cfg.cpu_used = pcfg.cpu_used;
    cfg.encoder_threads = pcfg.encoder_threads;
    cfg.bitshift = pcfg.bitshift;
    cfg.gamma = pcfg.gamma;
    cfg.shadow_gain = pcfg.shadow_gain;
    cfg.shadow_threshold = pcfg.shadow_threshold;
    cfg.shadow_floor = pcfg.shadow_floor;
    cfg.highlight_roof = pcfg.highlight_roof;
    cfg.edge_enhance = pcfg.edge_enhance;
    cfg.high_freq_boost = pcfg.high_freq_boost;
    cfg.motion_enhance = pcfg.motion_enhance;
    cfg.center_crop_ratio = pcfg.center_crop;
    cfg.gray = true;
    // 当 shadow_gain > 1 时启用 shadow LUT
    cfg.lut_type = (pcfg.shadow_gain > 1.0) ? "shadow" : "none";

    CompressedStreamPipeline pipeline(cfg);

    // 打开输出文件
    FILE *out_file = std::fopen(args.output.c_str(), "wb");
    if (!out_file)
    {
        std::fprintf(stderr, "Failed to open output file: %s\n", args.output.c_str());
        return 1;
    }

    size_t total_packets_written = 0;

    // 设置包输出回调: 写入文件
    pipeline.setPacketCallback([&](const BitstreamPacketizer::Packet &pkt) {
        std::fwrite(pkt.data, 1, BitstreamPacketizer::PACKET_SIZE, out_file);
        ++total_packets_written;
    });

    // 设置调试回调
    bool debug = args.debug;
    if (debug)
    {
        pipeline.setDebugFrameCallback([&debug](const cv::Mat &frame) {
            cv::imshow("Preprocessed", frame);
            int key = cv::waitKey(1);
            if (key == 27) // ESC 关闭预览
                debug = false;
        });
    }

    // 帧率转换: 累加器方式, 均匀跳帧
    double acc = 0.0;
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

        // 帧率转换: 跳帧以匹配目标帧率
        acc += encode_fps;
        if (acc < input_fps)
        {
            continue;
        }
        acc -= input_fps;

        // 编码
        if (!pipeline.feedFrame(bgr))
        {
            std::fprintf(stderr, "feedFrame() failed on frame %d\n", encoded_frames);
        }
        ++encoded_frames;

        // 产生对应的输出包
        while (pipeline.bufferedBytes() > 0)
        {
            pipeline.producePacket();
        }

        // 进度报告
        if (encoded_frames % 50 == 0)
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

    return 0;
}

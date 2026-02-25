#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bitstream_packetizer.h"
#include "decoder_pipeline.h"
#include "pipeline_config.h"

// 显示用法说明
static void printUsage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s <input_packets.bin> [options]\n"
                 "\n"
                 "解码 video_compress 产生的固定 300 字节包文件\n"
                 "\n"
                 "Options:\n"
                 "  --config <path>         配置文件路径 (默认: ./config.yaml)\n"
                 "  --output <path>         输出视频路径 (默认: decoded_output.mp4)\n"
                 "  --no-preview            不显示预览窗口\n"
                 "  --no-save               不保存视频文件 (仅预览)\n"
                 "\n"
                 "编码参数 (bitshift/fps/线程等) 从配置文件读取, 与编码器共享\n"
                 "输入: 300 字节/包的二进制文件 (由 video_compress 产生)\n"
                 "输出: 解码后的视频 + 实时预览窗口 (按 ESC 关闭预览)\n",
                 prog);
}

// 命令行参数 (仅运行时参数, 解码配置从 YAML 读取)
struct Args
{
    std::string input;
    std::string output = "decoded_output.mp4";
    std::string config = "config.yaml";
    bool show_preview = true;
    bool save_video = true;
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
        else if (arg == "--no-preview")
            args.show_preview = false;
        else if (arg == "--no-save")
            args.save_video = false;
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    if (!args.save_video && !args.show_preview)
    {
        std::fprintf(stderr, "Error: both --no-save and --no-preview specified, nothing to do\n");
        std::exit(1);
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

    // 打开输入文件
    FILE *in_file = std::fopen(args.input.c_str(), "rb");
    if (!in_file)
    {
        std::fprintf(stderr, "Failed to open input file: %s\n", args.input.c_str());
        return 1;
    }

    // 获取文件大小
    std::fseek(in_file, 0, SEEK_END);
    long file_size = std::ftell(in_file);
    std::fseek(in_file, 0, SEEK_SET);

    if (file_size <= 0)
    {
        std::fprintf(stderr, "Input file is empty\n");
        std::fclose(in_file);
        return 1;
    }

    long total_packets = file_size / BitstreamPacketizer::PACKET_SIZE;
    if (file_size % BitstreamPacketizer::PACKET_SIZE != 0)
    {
        std::fprintf(stderr, "Warning: file size (%ld) is not a multiple of %zu bytes, "
                             "trailing %ld bytes will be ignored\n",
                     file_size, BitstreamPacketizer::PACKET_SIZE,
                     file_size % BitstreamPacketizer::PACKET_SIZE);
    }

    std::printf("Input: %s (%ld bytes, %ld packets)\n",
                args.input.c_str(), file_size, total_packets);

    // 配置解码管线 (从共享配置读取参数)
    DecoderPipeline::Config cfg;

    // 重组器配置
    cfg.reassembler.require_keyframe_after_loss = true;

    // 解码器配置 (从 YAML)
    cfg.decoder.threads = pcfg.decode_threads;
    cfg.decoder.prefer_dav1d = pcfg.prefer_dav1d;
    cfg.decoder.bitshift = pcfg.bitshift;
    cfg.decoder.reverse_bitshift = (pcfg.bitshift > 0);

    // 输出配置 (从 YAML)
    cfg.output_path = args.save_video ? args.output : "";
    cfg.fps = pcfg.output_fps;
    cfg.show_preview = args.show_preview;
    cfg.preview_window = "Decoded";

    DecoderPipeline pipeline(cfg);

    if (!pipeline.open())
    {
        std::fprintf(stderr, "Failed to open decoder pipeline\n");
        std::fclose(in_file);
        return 1;
    }

    // 解码循环
    uint8_t packet_buf[BitstreamPacketizer::PACKET_SIZE];
    long packets_read = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true)
    {
        size_t n = std::fread(packet_buf, 1, BitstreamPacketizer::PACKET_SIZE, in_file);
        if (n < BitstreamPacketizer::PACKET_SIZE)
        {
            break; // EOF 或不完整包
        }

        if (!pipeline.pushPacket(packet_buf))
        {
            std::fprintf(stderr, "Pipeline error at packet %ld\n", packets_read);
            break;
        }

        ++packets_read;

        // 进度报告
        if (packets_read % 500 == 0)
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            auto st = pipeline.stats();
            std::printf("Progress: %ld/%ld packets, "
                        "%llu frames decoded, %llu written, "
                        "%.1fs elapsed\n",
                        packets_read, total_packets,
                        static_cast<unsigned long long>(st.decoder.frames_decoded),
                        static_cast<unsigned long long>(st.frames_written),
                        elapsed);
        }

        // 检查预览是否被关闭 (且不保存视频 → 提前退出)
        if (!args.save_video && !pipeline.previewActive())
        {
            std::printf("Preview closed, stopping\n");
            break;
        }
    }

    std::fclose(in_file);

    // 关闭管线 (刷新缓存帧)
    pipeline.close();

    auto end_time = std::chrono::steady_clock::now();
    double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    auto final_stats = pipeline.stats();

    std::printf("\n=== Decode Summary ===\n");
    std::printf("Packets read: %ld\n", packets_read);
    std::printf("  padding: %llu\n",
                static_cast<unsigned long long>(final_stats.reassembler.packets_padding));
    std::printf("  seq gaps: %llu\n",
                static_cast<unsigned long long>(final_stats.reassembler.seq_gaps));
    std::printf("Frames reassembled: %llu\n",
                static_cast<unsigned long long>(final_stats.reassembler.frames_emitted));
    std::printf("Frames dropped: %llu (missing_end=%llu, seq_gap=%llu, "
                "frag_error=%llu, overflow=%llu)\n",
                static_cast<unsigned long long>(final_stats.reassembler.frames_dropped),
                static_cast<unsigned long long>(final_stats.reassembler.drops_missing_end),
                static_cast<unsigned long long>(final_stats.reassembler.drops_seq_gap),
                static_cast<unsigned long long>(final_stats.reassembler.drops_frag_error),
                static_cast<unsigned long long>(final_stats.reassembler.drops_overflow));
    std::printf("Frames decoded: %llu\n",
                static_cast<unsigned long long>(final_stats.decoder.frames_decoded));
    std::printf("Decode errors: %llu\n",
                static_cast<unsigned long long>(final_stats.decoder.decode_errors));
    std::printf("Frames written: %llu\n",
                static_cast<unsigned long long>(final_stats.frames_written));
    std::printf("Frames shown: %llu\n",
                static_cast<unsigned long long>(final_stats.frames_shown));
    std::printf("Total time: %.2f s\n", total_elapsed);
    if (total_elapsed > 0)
    {
        std::printf("Avg decode speed: %.1f packets/s\n", packets_read / total_elapsed);
    }
    if (args.save_video && final_stats.frames_written > 0)
    {
        std::printf("Output: %s\n", args.output.c_str());
    }

    return 0;
}

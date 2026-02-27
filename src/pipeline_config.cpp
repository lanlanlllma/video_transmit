#include "pipeline_config.h"

#include <cstdio>
#include <yaml-cpp/yaml.h>

template <typename T>
static void readField(const YAML::Node &node, const char *key, T &field)
{
    if (node[key])
    {
        field = node[key].as<T>();
    }
}

bool PipelineConfig::load(const std::string &path, PipelineConfig &config)
{
    YAML::Node root;
    try
    {
        root = YAML::LoadFile(path);
    }
    catch (const YAML::Exception &e)
    {
        std::fprintf(stderr, "PipelineConfig: failed to load %s: %s\n",
                     path.c_str(), e.what());
        return false;
    }

    // 输入
    if (auto n = root["input"])
    {
        readField(n, "fps", config.input_fps);
    }

    // 编码
    if (auto n = root["encode"])
    {
        readField(n, "width", config.encode_width);
        readField(n, "height", config.encode_height);
        readField(n, "fps", config.encode_fps);
        readField(n, "bitrate", config.bitrate);
        readField(n, "gop", config.gop);
        readField(n, "cpu_used", config.cpu_used);
        readField(n, "threads", config.encoder_threads);
    }

    // 预处理
    if (auto n = root["preprocess"])
    {
        readField(n, "bitshift", config.bitshift);
        readField(n, "gamma", config.gamma);
        readField(n, "shadow_gain", config.shadow_gain);
        readField(n, "shadow_threshold", config.shadow_threshold);
        readField(n, "shadow_floor", config.shadow_floor);
        readField(n, "highlight_roof", config.highlight_roof);
        readField(n, "edge_enhance", config.edge_enhance);
        readField(n, "high_freq_boost", config.high_freq_boost);
        readField(n, "motion_enhance", config.motion_enhance);
        readField(n, "center_crop", config.center_crop);
    }

    // 解码
    if (auto n = root["decode"])
    {
        readField(n, "threads", config.decode_threads);
        readField(n, "prefer_dav1d", config.prefer_dav1d);
    }

    // 输出
    if (auto n = root["output"])
    {
        readField(n, "fps", config.output_fps);
    }

    return true;
}

bool PipelineConfig::save(const std::string &path, const PipelineConfig &config)
{
    YAML::Emitter out;
    out << YAML::BeginMap;

    // 输入
    out << YAML::Key << "input" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "fps" << YAML::Value << config.input_fps;
    out << YAML::EndMap;

    // 编码
    out << YAML::Key << "encode" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "width" << YAML::Value << config.encode_width;
    out << YAML::Key << "height" << YAML::Value << config.encode_height;
    out << YAML::Key << "fps" << YAML::Value << config.encode_fps;
    out << YAML::Key << "bitrate" << YAML::Value << config.bitrate;
    out << YAML::Key << "gop" << YAML::Value << config.gop;
    out << YAML::Key << "cpu_used" << YAML::Value << config.cpu_used;
    out << YAML::Key << "threads" << YAML::Value << config.encoder_threads;
    out << YAML::EndMap;

    // 预处理
    out << YAML::Key << "preprocess" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "bitshift" << YAML::Value << config.bitshift;
    out << YAML::Key << "gamma" << YAML::Value << config.gamma;
    out << YAML::Key << "shadow_gain" << YAML::Value << config.shadow_gain;
    out << YAML::Key << "shadow_threshold" << YAML::Value << config.shadow_threshold;
    out << YAML::Key << "shadow_floor" << YAML::Value << config.shadow_floor;
    out << YAML::Key << "highlight_roof" << YAML::Value << config.highlight_roof;
    out << YAML::Key << "edge_enhance" << YAML::Value << config.edge_enhance;
    out << YAML::Key << "high_freq_boost" << YAML::Value << config.high_freq_boost;
    out << YAML::Key << "motion_enhance" << YAML::Value << config.motion_enhance;
    out << YAML::Key << "center_crop" << YAML::Value << config.center_crop;
    out << YAML::EndMap;

    // 解码
    out << YAML::Key << "decode" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "threads" << YAML::Value << config.decode_threads;
    out << YAML::Key << "prefer_dav1d" << YAML::Value << config.prefer_dav1d;
    out << YAML::EndMap;

    // 输出
    out << YAML::Key << "output" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "fps" << YAML::Value << config.output_fps;
    out << YAML::EndMap;

    out << YAML::EndMap;

    FILE *f = std::fopen(path.c_str(), "w");
    if (!f)
    {
        std::fprintf(stderr, "PipelineConfig: failed to save %s\n", path.c_str());
        return false;
    }
    std::fprintf(f, "%s\n", out.c_str());
    std::fclose(f);
    return true;
}

void PipelineConfig::print() const
{
    std::printf("=== Pipeline Config ===\n");
    std::printf("Input:  fps=%.1f\n", input_fps);
    std::printf("Encode: %dx%d @ %dfps, bitrate=%d bps, gop=%d, cpu_used=%d\n",
                encode_width, encode_height, encode_fps, bitrate, gop, cpu_used);
    std::printf("Preprocess: bitshift=%d, gamma=%.1f, shadow_gain=%.1f, "
                "edge_enhance=%.1f, center_crop=%.1f\n",
                bitshift, gamma, shadow_gain, edge_enhance, center_crop);
    std::printf("Decode: threads=%d, prefer_dav1d=%s\n",
                decode_threads, prefer_dav1d ? "yes" : "no");
    std::printf("Output: fps=%.1f\n", output_fps);
}

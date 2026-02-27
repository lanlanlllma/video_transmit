#pragma once

#include <string>

// 共享管线配置
// 编码器和解码器读取同一份 YAML 配置文件, 保证参数一致
//
// 默认配置文件路径: ./config.yaml
// 编码器/解码器均支持 --config <path> 覆盖

struct PipelineConfig
{
    // === 输入参数 ===
    double input_fps = 30.0; // 输入视频帧率 (固定值)

    // === 编码参数 ===
    int encode_width = 480;
    int encode_height = 480;
    int encode_fps = 24;       // 编码目标帧率
    int bitrate = 120000;      // 目标码率 bit/s
    int gop = 48;              // 关键帧间隔
    int cpu_used = 8;          // libaom cpu-used 0-8
    int encoder_threads = 4;   // 编码线程数

    // === 预处理参数 ===
    int bitshift = 4;          // 比特右移 0-7
    double gamma = 1.0;        // Gamma 校正
    double shadow_gain = 1.4;  // 暗部提升倍数
    int shadow_threshold = 64; // 暗部阈值
    int shadow_floor = 0;      // 暗部舍弃阈值
    int highlight_roof = 255;  // 高亮舍弃阈值
    double edge_enhance = 0.0; // 边缘增强 0-1
    double high_freq_boost = 0.0; // 高频增强 0-1
    double motion_enhance = 0.0;  // 运动增强 0-1
    double center_crop = 1.0;  // 中心裁剪比例 0-1

    // === 解码参数 ===
    int decode_threads = 0;     // 解码线程数 (0=自动)
    bool prefer_dav1d = true;   // 优先使用 libdav1d

    // === 输出参数 ===
    double output_fps = 24.0;   // 输出视频帧率 (通常 = encode_fps)

    // 从 YAML 文件加载配置
    // 返回 true 成功, false 失败 (会打印错误信息到 stderr)
    static bool load(const std::string &path, PipelineConfig &config);

    // 保存当前配置到 YAML 文件
    static bool save(const std::string &path, const PipelineConfig &config);

    // 打印当前配置到 stdout
    void print() const;
};

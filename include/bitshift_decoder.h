#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>

// 简单解码+bitshift 还原封装
// 参考 scripts/decompress_hevc.py
class BitshiftVideoDecoder
{
public:
    // bitshift: 压缩时右移的位数，这里用于左移还原（0-7）
    // gray: 是否以灰度方式输出（BGR->Gray->BGR），用于消除绿色偏色
    BitshiftVideoDecoder(const std::string &input_path,
                         const std::string &output_path,
                         int bitshift = 4,
                         bool gray = true);

    // 运行解码流程：
    // 1) 用 OpenCV 读取压缩视频
    // 2) 每帧左移 bitshift，裁剪到 [0,255]
    // 3) 可选转灰度再转回 BGR
    // 4) 使用 H.264/mp4v 写出 mp4
    // 返回: true 表示成功，false 表示失败
    bool run();

private:
    bool openCaptureAndWriter(cv::VideoCapture &cap, cv::VideoWriter &writer,
                              int &fps, int &width, int &height);

private:
    std::string input_path_;
    std::string output_path_;
    int bitshift_;
    bool gray_;
};

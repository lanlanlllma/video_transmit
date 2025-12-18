#pragma once

#include <opencv2/core.hpp>
#include <string>

// 视频帧预处理器（C++ 版）
// 参考 scripts/preprocess.py 中的 FramePreprocessor
class FramePreprocessor
{
public:
    FramePreprocessor(int blur = 0,
                      int posterize = 4,
                      double gamma = 1.0,
                      const std::string &lut_type = "none",
                      int shadow_threshold = 64,
                      double shadow_gain = 1.4,
                      bool gray = true,
                      double crop_top_ratio = 0.0,
                      int bitshift = 0,
                      double edge_enhance = 0.0,
                      double high_freq_boost = 0.0,
                      int shadow_floor = 0,
                      double motion_enhance = 0.0,
                      int motion_threshold = 10,
                      int motion_dilation = 3,
                      int highlight_roof = 255);

    // 裁剪顶部
    cv::Mat cropTop(const cv::Mat &frame) const;

    // 转灰度（BGR -> GRAY -> BGR），与 Python 版保持一致
    cv::Mat toGrayscale(const cv::Mat &frame) const;

    // 兼容接口：缩放，当前不在管线中使用
    cv::Mat resize(const cv::Mat &frame, const cv::Size &size) const;

    // 中心/下半部分裁剪
    cv::Mat cropCenterRatio(const cv::Mat &frame, double center_ratio = 1.0) const;

    // 核心预处理：模糊、量化、LUT、暗部/高亮舍弃、运动/边缘/高频增强
    cv::Mat process(const cv::Mat &frame);

    // 比特右移
    cv::Mat applyBitshift(const cv::Mat &frame) const;

    // 重置运动检测状态
    void resetMotionState();

    // 完整处理管线：裁剪 -> 中心裁剪 -> 灰度 -> 预处理 -> 比特移位
    cv::Mat processFullPipeline(const cv::Mat &frame,
                                const cv::Size &size = cv::Size(),
                                double center_ratio = 1.0);

    // 仅转灰度并预处理（向后兼容）
    cv::Mat processToGray(const cv::Mat &frame);

private:
    cv::Mat buildLut() const;
    cv::Mat protectBrightEdges(const cv::Mat &frame) const;
    cv::Mat highFreqEnhance(const cv::Mat &frame) const;
    cv::Mat detectMotion(const cv::Mat &frame);
    cv::Mat enhanceMotionRegions(const cv::Mat &frame, const cv::Mat &motion_mask) const;

private:
    int blur_;
    int posterize_;
    double gamma_;
    std::string lut_type_;
    int shadow_threshold_;
    double shadow_gain_;
    bool gray_;
    double crop_top_ratio_;
    int bitshift_;
    double edge_enhance_;
    double high_freq_boost_;
    int shadow_floor_;
    double motion_enhance_;
    int motion_threshold_;
    int motion_dilation_;
    int highlight_roof_;

    // 运动检测用的前一帧
    cv::Mat prev_frame_;

    // Gamma + 暗部提升 LUT
    cv::Mat lut_;
};

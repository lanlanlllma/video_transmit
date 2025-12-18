#include "frame_preprocessor.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

FramePreprocessor::FramePreprocessor(int blur,
                                     int posterize,
                                     double gamma,
                                     const std::string &lut_type,
                                     int shadow_threshold,
                                     double shadow_gain,
                                     bool gray,
                                     double crop_top_ratio,
                                     int bitshift,
                                     double edge_enhance,
                                     double high_freq_boost,
                                     int shadow_floor,
                                     double motion_enhance,
                                     int motion_threshold,
                                     int motion_dilation,
                                     int highlight_roof)
    : blur_(std::max(0, blur)),
      posterize_(std::max(0, posterize)),
      gamma_(std::max(0.0, gamma)),
      lut_type_(lut_type),
      shadow_threshold_(shadow_threshold),
      shadow_gain_(shadow_gain),
      gray_(gray),
      crop_top_ratio_(std::max(0.0, std::min(0.9, crop_top_ratio))),
      bitshift_(std::max(0, std::min(7, bitshift))),
      edge_enhance_(std::max(0.0, std::min(1.0, edge_enhance))),
      high_freq_boost_(std::max(0.0, std::min(1.0, high_freq_boost))),
      shadow_floor_(std::max(0, shadow_floor)),
      motion_enhance_(std::max(0.0, std::min(1.0, motion_enhance))),
      motion_threshold_(std::max(1, motion_threshold)),
      motion_dilation_(std::max(1, motion_dilation)),
      highlight_roof_(std::min(255, highlight_roof))
{
    lut_ = buildLut();
}

cv::Mat FramePreprocessor::buildLut() const
{
    cv::Mat lut(1, 256, CV_8UC1);

    for (int i = 0; i < 256; ++i)
    {
        double v = static_cast<double>(i);

        // Gamma 校正
        if (gamma_ > 0.0 && std::abs(gamma_ - 1.0) > 1e-6)
        {
            double x = v / 255.0;
            v = 255.0 * std::pow(x, gamma_);
        }

        // 暗部提升
        if (lut_type_ == "shadow" && shadow_gain_ > 1.0)
        {
            int thr = std::max(0, std::min(255, shadow_threshold_));
            if (v < thr)
            {
                v = std::min<double>(thr, v * shadow_gain_);
            }
        }

        v = std::max(0.0, std::min(255.0, v));
        lut.at<uchar>(i) = static_cast<uchar>(v);
    }

    return lut;
}

cv::Mat FramePreprocessor::cropTop(const cv::Mat &frame) const
{
    if (crop_top_ratio_ <= 0.0 || frame.empty())
    {
        return frame;
    }
    int h = frame.rows;
    int cut = static_cast<int>(h * crop_top_ratio_);
    cut = std::max(0, std::min(h, cut));
    if (cut >= h)
    {
        return frame;
    }
    return frame(cv::Range(cut, h), cv::Range::all()).clone();
}

cv::Mat FramePreprocessor::toGrayscale(const cv::Mat &frame) const
{
    if (!gray_ || frame.empty())
    {
        return frame;
    }

    cv::Mat gray;
    if (frame.channels() == 1)
    {
        gray = frame;
    }
    else
    {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Mat bgr;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
}

cv::Mat FramePreprocessor::resize(const cv::Mat &frame, const cv::Size &size) const
{
    if (frame.empty() || size.width <= 0 || size.height <= 0)
    {
        return frame;
    }
    cv::Mat out;
    cv::resize(frame, out, size, 0, 0, cv::INTER_AREA);
    return out;
}

cv::Mat FramePreprocessor::cropCenterRatio(const cv::Mat &frame, double center_ratio) const
{
    if (frame.empty())
    {
        return frame;
    }

    center_ratio = std::max(0.0, std::min(1.0, center_ratio));
    if (center_ratio >= 0.999)
    {
        return frame;
    }

    int h = frame.rows;
    int w = frame.cols;

    int new_w = static_cast<int>(w * center_ratio);
    int new_h = static_cast<int>(h * center_ratio);

    new_w = std::max(1, std::min(w, new_w));
    new_h = std::max(1, std::min(h, new_h));

    // 水平方向：保留中间 new_w
    int x1 = (w - new_w) / 2;
    int x2 = x1 + new_w;

    // 垂直方向：保留下半部分中间 new_h
    int half_h_start = h / 2;
    int half_h_end = h;
    int half_h = half_h_end - half_h_start;
    int y1;
    if (new_h >= half_h)
    {
        y1 = half_h_start;
    }
    else
    {
        y1 = half_h_start + (half_h - new_h) / 2;
    }
    int y2 = y1 + new_h;

    y1 = std::max(0, std::min(h - 1, y1));
    y2 = std::max(y1 + 1, std::min(h, y2));

    return frame(cv::Range(y1, y2), cv::Range(x1, x2)).clone();
}

cv::Mat FramePreprocessor::process(const cv::Mat &input)
{
    if (input.empty())
    {
        return input;
    }

    cv::Mat frame = input.clone();

    // 高斯模糊
    if (blur_ > 0)
    {
        int ksize = blur_ * 2 + 1;
        cv::GaussianBlur(frame, frame, cv::Size(ksize, ksize), 0);
    }

    // 色彩量化（posterize）
    if (posterize_ > 0)
    {
        int levels = std::max(2, 8 - posterize_);
        int factor = 256 / levels;
        cv::Mat qlut(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
        {
            int v = (i / factor) * factor;
            if (v > 255)
                v = 255;
            qlut.at<uchar>(i) = static_cast<uchar>(v);
        }
        cv::LUT(frame, qlut, frame);
    }

    // LUT 变换（gamma + 暗部提升）
    if (!lut_.empty() && (lut_type_ != "none" || (gamma_ > 0.0 && std::abs(gamma_ - 1.0) > 1e-6)))
    {
        cv::LUT(frame, lut_, frame);
    }

    // 暗部舍弃
    if (shadow_floor_ > 0)
    {
        cv::Mat gray;
        if (frame.channels() == 3)
        {
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        }
        else
        {
            gray = frame;
        }

        cv::Mat mask;
        cv::compare(gray, shadow_floor_, mask, cv::CMP_LT);
        if (frame.channels() == 3)
        {
            frame.setTo(cv::Scalar(0, 0, 0), mask);
        }
        else
        {
            frame.setTo(0, mask);
        }
    }

    // 高亮舍弃
    if (highlight_roof_ < 255)
    {
        cv::Mat gray;
        if (frame.channels() == 3)
        {
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        }
        else
        {
            gray = frame;
        }

        cv::Mat mask;
        cv::compare(gray, highlight_roof_, mask, cv::CMP_GT);
        if (frame.channels() == 3)
        {
            frame.setTo(cv::Scalar(255, 255, 255), mask);
        }
        else
        {
            frame.setTo(255, mask);
        }
    }

    // 运动增强
    if (motion_enhance_ > 0.0)
    {
        cv::Mat motion_mask = detectMotion(frame);
        frame = enhanceMotionRegions(frame, motion_mask);
    }

    // 边缘保护增强
    if (edge_enhance_ > 0.0)
    {
        frame = protectBrightEdges(frame);
    }

    // 高频增强
    if (high_freq_boost_ > 0.0)
    {
        frame = highFreqEnhance(frame);
    }

    return frame;
}

cv::Mat FramePreprocessor::applyBitshift(const cv::Mat &frame) const
{
    if (bitshift_ <= 0 || frame.empty())
    {
        return frame;
    }

    cv::Mat lut(1, 256, CV_8UC1);
    for (int i = 0; i < 256; ++i)
    {
        int v = (i >> bitshift_);
        if (v > 255)
            v = 255;
        lut.at<uchar>(i) = static_cast<uchar>(v);
    }

    cv::Mat out;
    cv::LUT(frame, lut, out);
    return out;
}

cv::Mat FramePreprocessor::protectBrightEdges(const cv::Mat &frame) const
{
    if (edge_enhance_ <= 0.0 || frame.empty())
    {
        return frame;
    }

    cv::Mat gray;
    if (frame.channels() == 3)
    {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    }
    else
    {
        gray = frame;
    }

    cv::Mat grad_x, grad_y;
    cv::Sobel(gray, grad_x, CV_64F, 1, 0, 3);
    cv::Sobel(gray, grad_y, CV_64F, 0, 1, 3);

    cv::Mat mag;
    cv::magnitude(grad_x, grad_y, mag);

    cv::Mat bright_mask;
    cv::compare(gray, 140, bright_mask, cv::CMP_GT);

    cv::Mat edge_mask;
    cv::compare(mag, 20.0, edge_mask, cv::CMP_GT);

    cv::bitwise_and(bright_mask, edge_mask, edge_mask);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::dilate(edge_mask, edge_mask, kernel, cv::Point(-1, -1), 2);
    cv::dilate(edge_mask, edge_mask, kernel, cv::Point(-1, -1), 1);

    double enhance_factor = 1.0 + edge_enhance_ * 0.3;

    cv::Mat enhanced;
    frame.convertTo(enhanced, CV_32F);

    if (frame.channels() == 3)
    {
        for (int y = 0; y < frame.rows; ++y)
        {
            const uchar *mrow = edge_mask.ptr<uchar>(y);
            cv::Vec3f *erow = enhanced.ptr<cv::Vec3f>(y);
            for (int x = 0; x < frame.cols; ++x)
            {
                if (mrow[x])
                {
                    erow[x] *= static_cast<float>(enhance_factor);
                }
            }
        }
    }
    else
    {
        for (int y = 0; y < frame.rows; ++y)
        {
            const uchar *mrow = edge_mask.ptr<uchar>(y);
            float *erow = enhanced.ptr<float>(y);
            for (int x = 0; x < frame.cols; ++x)
            {
                if (mrow[x])
                {
                    erow[x] *= static_cast<float>(enhance_factor);
                }
            }
        }
    }

    cv::Mat out;
    enhanced.convertTo(out, frame.type());
    cv::min(out, 255, out);
    cv::max(out, 0, out);
    return out;
}

cv::Mat FramePreprocessor::highFreqEnhance(const cv::Mat &frame) const
{
    if (high_freq_boost_ <= 0.0 || frame.empty())
    {
        return frame;
    }

    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(5, 5), 0);

    double boost_factor = high_freq_boost_ * 0.5;

    cv::Mat enhanced;
    cv::addWeighted(frame, 1.0 + boost_factor, blurred, -boost_factor, 0, enhanced);
    return enhanced;
}

cv::Mat FramePreprocessor::detectMotion(const cv::Mat &frame)
{
    if (frame.empty())
    {
        return cv::Mat();
    }

    if (prev_frame_.empty() || prev_frame_.size() != frame.size() || prev_frame_.type() != frame.type())
    {
        prev_frame_ = frame.clone();
        return cv::Mat::zeros(frame.rows, frame.cols, CV_8UC1);
    }

    cv::Mat gray, prev_gray;
    if (frame.channels() == 3)
    {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    }
    else
    {
        gray = frame;
    }

    if (prev_frame_.channels() == 3)
    {
        cv::cvtColor(prev_frame_, prev_gray, cv::COLOR_BGR2GRAY);
    }
    else
    {
        prev_gray = prev_frame_;
    }

    cv::Mat motion_mask;
    cv::absdiff(gray, prev_gray, motion_mask);

    cv::Mat bright1, bright2, bright_mask;
    cv::compare(gray, 125, bright1, cv::CMP_GT);
    cv::compare(prev_gray, 125, bright2, cv::CMP_GT);
    cv::bitwise_and(bright1, bright2, bright_mask);

    cv::Mat not_bright;
    cv::bitwise_not(bright_mask, not_bright);
    motion_mask.setTo(0, not_bright);

    prev_frame_ = frame.clone();

    return motion_mask;
}

cv::Mat FramePreprocessor::enhanceMotionRegions(const cv::Mat &frame, const cv::Mat &motion_mask) const
{
    if (motion_enhance_ <= 0.0 || frame.empty() || motion_mask.empty())
    {
        return frame;
    }

    cv::Mat motion_binary;
    cv::threshold(motion_mask, motion_binary, motion_threshold_, 255, cv::THRESH_BINARY);

    if (motion_dilation_ > 0)
    {
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
        cv::dilate(motion_binary, motion_binary, kernel, cv::Point(-1, -1), motion_dilation_);
    }

    cv::Mat motion_mask_f;
    motion_binary.convertTo(motion_mask_f, CV_32F, 1.0 / 255.0);
    cv::GaussianBlur(motion_mask_f, motion_mask_f, cv::Size(5, 5), 0);

    double minVal = 0.0, maxVal = 0.0;
    cv::minMaxLoc(motion_mask_f, &minVal, &maxVal);

    float weight = 0.0f;
    if (maxVal > 0.0)
    {
        cv::Scalar mean_val = cv::mean(motion_mask_f);
        weight = static_cast<float>(mean_val[0]);
    }

    double brightness_boost = 15.0;
    double contrast_factor = 1.0 + motion_enhance_ * 0.4;

    cv::Mat contrast_enhanced;
    cv::addWeighted(frame, contrast_factor, frame, -contrast_factor + 1.0, brightness_boost, contrast_enhanced);

    cv::Mat diff;
    cv::subtract(contrast_enhanced, frame, diff);

    cv::Mat enhanced;
    cv::addWeighted(frame, 1.0, diff, weight, 0.0, enhanced);

    return enhanced;
}

void FramePreprocessor::resetMotionState()
{
    prev_frame_.release();
}

cv::Mat FramePreprocessor::processFullPipeline(const cv::Mat &frame,
                                               const cv::Size & /*size*/,
                                               double center_ratio)
{
    if (frame.empty())
    {
        return frame;
    }

    cv::Mat out = cropTop(frame);
    out = cropCenterRatio(out, center_ratio);
    out = toGrayscale(out);
    out = process(out);
    out = applyBitshift(out);

    return out;
}

cv::Mat FramePreprocessor::processToGray(const cv::Mat &frame)
{
    if (frame.empty())
    {
        return frame;
    }

    cv::Mat out = toGrayscale(frame);
    out = process(out);
    return out;
}

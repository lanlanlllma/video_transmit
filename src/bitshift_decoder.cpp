#include "bitshift_decoder.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <iostream>
#include <algorithm>

BitshiftVideoDecoder::BitshiftVideoDecoder(const std::string &input_path,
                                           const std::string &output_path,
                                           int bitshift,
                                           bool gray)
    : input_path_(input_path),
      output_path_(output_path),
      bitshift_(std::max(0, std::min(7, bitshift))),
      gray_(gray)
{
}

bool BitshiftVideoDecoder::openCaptureAndWriter(cv::VideoCapture &cap,
                                                cv::VideoWriter &writer,
                                                int &fps,
                                                int &width,
                                                int &height)
{
    cap.open(input_path_);
    if (!cap.isOpened())
    {
        std::cerr << "Failed to open input video: " << input_path_ << std::endl;
        return false;
    }

    fps = static_cast<int>(cap.get(cv::CAP_PROP_FPS));
    if (fps <= 0)
    {
        fps = 30;
    }
    width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    int total_frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));

    std::cout << "Compressed video info: " << width << "x" << height
              << ", " << fps << " fps, " << total_frames << " frames" << std::endl;
    std::cout << "Gray mode: " << (gray_ ? "ON" : "OFF")
              << ", bitshift = " << bitshift_ << std::endl;

    // 先尝试 H.264 (avc1)，失败则回退 mp4v
    const std::pair<int, const char *> candidates[] = {
        {cv::VideoWriter::fourcc('a', 'v', 'c', '1'), "H264"},
        {cv::VideoWriter::fourcc('m', 'p', '4', 'v'), "MPEG4 fallback"},
    };

    for (auto &c : candidates)
    {
        int fourcc = c.first;
        writer.open(output_path_, fourcc, static_cast<double>(fps), cv::Size(width, height));
        if (writer.isOpened())
        {
            std::cout << "Using encoder: " << c.second << std::endl;
            return true;
        }
        writer.release();
    }

    std::cerr << "Failed to create VideoWriter for output: " << output_path_ << std::endl;
    return false;
}

bool BitshiftVideoDecoder::run()
{
    cv::VideoCapture cap;
    cv::VideoWriter writer;
    int fps = 0, width = 0, height = 0;

    if (!openCaptureAndWriter(cap, writer, fps, width, height))
    {
        return false;
    }

    int frame_count = 0;
    int written_frames = 0;

    cv::Mat frame;
    while (true)
    {
        if (!cap.read(frame) || frame.empty())
        {
            break; // 视频结束
        }

        ++frame_count;

        // 解压：按压缩时的 bitshift 左移，并裁剪到 [0,255]
        // 转为 16 位，左移恢复色深，并裁剪到 [0,255]
        cv::Mat tmp16;
        frame.convertTo(tmp16, CV_16U);
        tmp16 *= (1 << bitshift_);
        cv::Mat clipped;
        cv::min(tmp16, 255, clipped);
        clipped.convertTo(frame, CV_8U);

        if (gray_)
        {
            cv::Mat g, bgr;
            cv::cvtColor(frame, g, cv::COLOR_BGR2GRAY);
            cv::cvtColor(g, bgr, cv::COLOR_GRAY2BGR);
            frame = bgr;
        }

        writer.write(frame);
        ++written_frames;

        if (written_frames % 30 == 0)
        {
            std::cout << "Decoded " << frame_count << " frames, written "
                      << written_frames << " frames" << std::endl;
        }
    }

    cap.release();
    writer.release();

    std::cout << "Decode finished. Total frames: " << frame_count
              << ", written: " << written_frames << std::endl;

    return true;
}

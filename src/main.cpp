#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <chrono>

#include <opencv2/opencv.hpp>

#include "transcoder.h"
#include "frame_preprocessor.h"

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr,
                 "Usage: %s <output file> <codec name> [width] [height] [frames] [debug]\n",
                 argv[0]);
    std::fprintf(stderr,
                 "Example: %s out.h264 libx264 352 288 25 1\n",
                 argv[0]);
    return 1;
  }

  const char *filename = argv[1];
  std::string codec_name = argv[2];

  int width = 352;
  int height = 288;
  int frames = 250;
  bool debug = false;

  if (argc >= 5)
  {
    width = std::atoi(argv[3]);
    height = std::atoi(argv[4]);
  }
  if (argc >= 6)
  {
    frames = std::atoi(argv[5]);
  }

  if (argc >= 7)
  {
    debug = (std::atoi(argv[6]) != 0);
  }

  std::printf("Encoding up to %d frames at %dx%d using codec %s -> %s (debug=%s)\n",
              frames, width, height, codec_name.c_str(), filename,
              debug ? "true" : "false");

  // 确保 YUV420 分辨率为偶数，避免编码器潜在问题
  if (width % 2 != 0)
  {
    --width;
    std::printf("Adjusted width to even: %d\n", width);
  }
  if (height % 2 != 0)
  {
    --height;
    std::printf("Adjusted height to even: %d\n", height);
  }

  FILE *f = std::fopen(filename, "wb");
  if (!f)
  {
    std::perror("fopen");
    return 1;
  }

  FILE *csv = nullptr;
  if (debug)
  {
    std::string csv_name = std::string(filename) + ".csv";
    csv = std::fopen(csv_name.c_str(), "w");
    if (csv)
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
    const std::string input_video = "/Users/mading/video_transmit/rm_test_video/test_video1.mp4";
    cv::VideoCapture cap(input_video);
    if (!cap.isOpened())
    {
      std::fprintf(stderr, "Failed to open input video: %s\n", input_video.c_str());
      std::fclose(f);
      if (csv)
      {
        std::fclose(csv);
      }
      return 1;
    }

    double src_fps = cap.get(cv::CAP_PROP_FPS);
    if (src_fps <= 0.0)
    {
      src_fps = 24.0;
    }
    int total_src_frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));

    // 目标帧率，参考 Python 脚本示例 (--target-fps 24)
    double target_fps = 24.0;
    if (src_fps < target_fps)
    {
      target_fps = src_fps;
    }

    int frame_skip = 1;
    if (target_fps > 0.0)
    {
      frame_skip = std::max(1, static_cast<int>(std::round(src_fps / target_fps)));
    }

    std::printf("Input video: %s, %d frames, fps=%.2f -> target_fps=%.2f (skip=%d)\n",
                input_video.c_str(), total_src_frames, src_fps, target_fps, frame_skip);

    // 预处理参数，参考 compress_av1.py 注释示例
    FramePreprocessor preprocessor(
        /*blur=*/0,
        /*posterize=*/16,
        /*gamma=*/2.0,
        /*lut_type=*/"shadow",
        /*shadow_threshold=*/50,
        /*shadow_gain=*/8.0,
        /*gray=*/true,
        /*crop_top_ratio=*/0.0,
        /*bitshift=*/0,
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
    while (encoded_frames < frames)
    {
      cv::Mat bgr;
      if (!cap.read(bgr) || bgr.empty())
      {
        break; // 视频读完
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
      if (debug)
      {
        cv::imshow("preprocessed_final_gray", gray);
        // 使用很短的等待时间，避免严重阻塞编码流程
        int key = cv::waitKey(1);
        if (key == 27) // ESC 退出预览但继续编码
        {
          // 用户可按 ESC 停止后续显示
          debug = false;
        }
      }

      cv::Mat bitstream;
      auto start = std::chrono::high_resolution_clock::now();
      if (!encoder.encode(gray, bitstream))
      {
        std::fprintf(stderr, "encode() failed on frame %d\n", encoded_frames);
        std::fclose(f);
        if (csv)
        {
          std::fclose(csv);
        }
        return 1;
      }
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double, std::milli> diff = end - start;
      size_t bytes = bitstream.empty() ? 0 : static_cast<size_t>(bitstream.total());

      if (!bitstream.empty())
      {
        std::fwrite(bitstream.data, 1,
                    static_cast<size_t>(bitstream.total()), f);
        std::printf("Frame %d: encode time = %.3f ms, wrote %zu bytes\n",
                    encoded_frames, diff.count(), bytes);
      }
      else
      {
        std::printf("Frame %d: encode time = %.3f ms, wrote 0 bytes (no output)\n",
                    encoded_frames, diff.count());
      }

      if (csv)
      {
        std::fprintf(csv, "%d,%.6f,%zu\n", encoded_frames, diff.count(), bytes);
      }

      ++encoded_frames;
    }

    cap.release();

    // flush 剩余数据
    cv::Mat tail;
    if (!encoder.flush(tail))
    {
      std::fprintf(stderr, "flush() failed.\n");
      std::fclose(f);
      if (csv)
      {
        std::fclose(csv);
      }
      return 1;
    }
    if (!tail.empty())
    {
      std::fwrite(tail.data, 1,
                  static_cast<size_t>(tail.total()), f);
      std::printf("Flush: wrote %zu bytes\n",
                  static_cast<size_t>(tail.total()));
    }
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "Exception: %s\n", e.what());
    std::fclose(f);
    if (csv)
    {
      std::fclose(csv);
    }
    return 1;
  }

  std::fclose(f);
  if (csv)
  {
    std::fclose(csv);
  }
  std::printf("Done.\n");
  return 0;
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include <opencv2/opencv.hpp>

#include "bitshift_decoder.h"

// 简单示例：参考 scripts/decompress_hevc.py
// Usage:
//   video_decompress <input> <output> [bitshift] [gray]
//   - input: 压缩视频路径（mp4 等容器文件）
//   - output: 解压后输出路径（mp4）
//   - bitshift: 压缩时使用的右移位数，默认 4
//   - gray: 1=灰度输出(默认)，0=保留彩色
int main(int argc, char **argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr,
                 "Usage: %s <input> <output> [bitshift] [gray]\n",
                 argv[0]);
    std::fprintf(stderr,
                 "Example: %s compressed.mp4 decompressed.mp4 4 1\n",
                 argv[0]);
    return 1;
  }

  std::string input = argv[1];
  std::string output = argv[2];

  int bitshift = 4;
  bool gray = true;

  if (argc >= 4)
  {
    bitshift = std::atoi(argv[3]);
  }
  if (argc >= 5)
  {
    gray = (std::atoi(argv[4]) != 0);
  }

  try
  {
    BitshiftVideoDecoder decoder(input, output, bitshift, gray);
    if (!decoder.run())
    {
      std::fprintf(stderr, "Decode failed.\n");
      return 1;
    }
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "Exception: %s\n", e.what());
    return 1;
  }

  std::printf("Done.\n");
  return 0;
}

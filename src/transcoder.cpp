#include "transcoder.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace
{

  static void throw_if_ffmpeg_error(int err, const char *what)
  {
    if (err < 0)
    {
      char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
      av_strerror(err, buf, sizeof(buf));
      std::string msg = std::string(what) + ": " + buf;
      throw std::runtime_error(msg);
    }
  }

} // namespace

MatVideoEncoder::MatVideoEncoder(const std::string &codec_name,
                                 int width,
                                 int height,
                                 int bitrate,
                                 int fps)
    : width_(width), height_(height), fps_(fps)
{
  if (width_ <= 0 || height_ <= 0)
  {
    throw std::invalid_argument("width and height must be positive");
  }

  const AVCodec *codec = avcodec_find_encoder_by_name(codec_name.c_str());
  if (!codec)
  {
    throw std::runtime_error("Codec not found: " + codec_name);
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_)
  {
    throw std::runtime_error("Could not allocate video codec context");
  }

  codec_ctx_->bit_rate = bitrate;
  codec_ctx_->width = width_;
  codec_ctx_->height = height_;
  codec_ctx_->time_base = {1, fps_};
  codec_ctx_->framerate = {fps_, 1};
  codec_ctx_->gop_size = 24;
  codec_ctx_->max_b_frames = 24;
  codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

  if (codec->id == AV_CODEC_ID_H264)
  {
    av_opt_set(codec_ctx_->priv_data, "preset", "slow", 0);
  }

  // libaom-av1 调优参数，参考 Python compress_av1 默认设置
  if (codec->id == AV_CODEC_ID_AV1)
  {
    // 压缩速度/质量折中：4 对应 ffmpeg 中 cpu-used=4
    av_opt_set(codec_ctx_->priv_data, "cpu-used", "3", 0);
    // 开启行并行
    av_opt_set(codec_ctx_->priv_data, "row-mt", "1", 0);
    // 与脚本一致，默认不拆 tile
    av_opt_set(codec_ctx_->priv_data, "tile-columns", "0", 0);
    av_opt_set(codec_ctx_->priv_data, "tile-rows", "0", 0);
    // 适度 lookahead，减小延迟
    av_opt_set(codec_ctx_->priv_data, "lag-in-frames", "10", 0);
    av_opt_set(codec_ctx_->priv_data, "rc_max_rate", "60000", 0);
    av_opt_set(codec_ctx_->priv_data, "rc_buffer_size", "0", 0);
  }

  // 线程数 0 表示让编码器自行决定
  codec_ctx_->thread_count = 16;

  int ret = avcodec_open2(codec_ctx_, codec, nullptr);
  if (ret < 0)
  {
    avcodec_free_context(&codec_ctx_);
    throw_if_ffmpeg_error(ret, "Could not open codec");
  }

  frame_ = av_frame_alloc();
  if (!frame_)
  {
    avcodec_free_context(&codec_ctx_);
    throw std::runtime_error("Could not allocate video frame");
  }
  frame_->format = codec_ctx_->pix_fmt;
  frame_->width = codec_ctx_->width;
  frame_->height = codec_ctx_->height;

  ret = av_frame_get_buffer(frame_, 0);
  if (ret < 0)
  {
    av_frame_free(&frame_);
    avcodec_free_context(&codec_ctx_);
    throw_if_ffmpeg_error(ret, "Could not allocate the video frame data");
  }

  packet_ = av_packet_alloc();
  if (!packet_)
  {
    av_frame_free(&frame_);
    avcodec_free_context(&codec_ctx_);
    throw std::runtime_error("Could not allocate packet");
  }
}

MatVideoEncoder::~MatVideoEncoder()
{
  if (packet_)
  {
    av_packet_free(&packet_);
  }
  if (frame_)
  {
    av_frame_free(&frame_);
  }
  if (codec_ctx_)
  {
    avcodec_free_context(&codec_ctx_);
  }
}

MatVideoEncoder::MatVideoEncoder(MatVideoEncoder &&other) noexcept
{
  codec_ctx_ = other.codec_ctx_;
  frame_ = other.frame_;
  packet_ = other.packet_;
  width_ = other.width_;
  height_ = other.height_;
  fps_ = other.fps_;
  next_pts_ = other.next_pts_;
  flushed_ = other.flushed_;

  other.codec_ctx_ = nullptr;
  other.frame_ = nullptr;
  other.packet_ = nullptr;
}

MatVideoEncoder &MatVideoEncoder::operator=(MatVideoEncoder &&other) noexcept
{
  if (this == &other)
  {
    return *this;
  }

  if (packet_)
  {
    av_packet_free(&packet_);
  }
  if (frame_)
  {
    av_frame_free(&frame_);
  }
  if (codec_ctx_)
  {
    avcodec_free_context(&codec_ctx_);
  }

  codec_ctx_ = other.codec_ctx_;
  frame_ = other.frame_;
  packet_ = other.packet_;
  width_ = other.width_;
  height_ = other.height_;
  fps_ = other.fps_;
  next_pts_ = other.next_pts_;
  flushed_ = other.flushed_;

  other.codec_ctx_ = nullptr;
  other.frame_ = nullptr;
  other.packet_ = nullptr;

  return *this;
}

bool MatVideoEncoder::encode(const cv::Mat &gray, cv::Mat &bitstream)
{
  if (!codec_ctx_ || !frame_ || !packet_)
  {
    return false;
  }
  if (flushed_)
  {
    // 已经 flush 之后不再接受新帧
    return false;
  }
  if (gray.empty())
  {
    return false;
  }
  if (gray.type() != CV_8UC1)
  {
    std::fprintf(stderr, "MatVideoEncoder::encode expects CV_8UC1 input.\n");
    return false;
  }
  if (gray.cols != width_ || gray.rows != height_)
  {
    std::fprintf(stderr, "MatVideoEncoder::encode expects %dx%d input, got %dx%d.\n",
                 width_, height_, gray.cols, gray.rows);
    return false;
  }

  int ret = av_frame_make_writable(frame_);
  if (ret < 0)
  {
    throw_if_ffmpeg_error(ret, "Frame not writable");
  }

  // 将灰度图填充到 Y 分量, U/V 固定为 128 以保持灰度
  for (int y = 0; y < height_; ++y)
  {
    const uint8_t *src_row = gray.ptr<uint8_t>(y);
    uint8_t *dst_row = frame_->data[0] + y * frame_->linesize[0];
    std::memcpy(dst_row, src_row, static_cast<size_t>(width_));
  }

  int chroma_h = height_ / 2;
  int chroma_w = width_ / 2;
  for (int y = 0; y < chroma_h; ++y)
  {
    std::memset(frame_->data[1] + y * frame_->linesize[1], 128, static_cast<size_t>(chroma_w));
    std::memset(frame_->data[2] + y * frame_->linesize[2], 128, static_cast<size_t>(chroma_w));
  }

  frame_->pts = next_pts_++;

  return sendFrameAndReceivePackets(frame_, bitstream);
}

bool MatVideoEncoder::flush(cv::Mat &bitstream)
{
  if (!codec_ctx_ || !packet_)
  {
    return false;
  }
  if (flushed_)
  {
    // 已经 flush 过, 不再重复
    bitstream.release();
    return true;
  }

  bool ok = sendFrameAndReceivePackets(nullptr, bitstream);
  flushed_ = true;
  return ok;
}

bool MatVideoEncoder::sendFrameAndReceivePackets(AVFrame *frame, cv::Mat &bitstream)
{
  bitstream.release();

  int ret = avcodec_send_frame(codec_ctx_, frame);
  if (ret < 0)
  {
    throw_if_ffmpeg_error(ret, "Error sending a frame for encoding");
  }

  std::vector<uint8_t> buffer;

  while (true)
  {
    ret = avcodec_receive_packet(codec_ctx_, packet_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
      break;
    }
    else if (ret < 0)
    {
      throw_if_ffmpeg_error(ret, "Error during encoding");
    }

    size_t old_size = buffer.size();
    buffer.resize(old_size + static_cast<size_t>(packet_->size));
    std::memcpy(buffer.data() + old_size, packet_->data, static_cast<size_t>(packet_->size));

    av_packet_unref(packet_);
  }

  if (!buffer.empty())
  {
    bitstream.create(1, static_cast<int>(buffer.size()), CV_8UC1);
    std::memcpy(bitstream.data, buffer.data(), buffer.size());
  }

  return true;
}

#include "bitstream_packetizer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

BitstreamPacketizer::BitstreamPacketizer()
    : current_frame_id_(0),
      current_is_keyframe_(false),
      current_frame_remaining_(0),
      current_frag_index_(0),
      seq_num_(0),
      dropped_bytes_(0)
{
}

void BitstreamPacketizer::setCallback(PacketCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
}

void BitstreamPacketizer::pushEncodedData(const uint8_t *data, size_t size,
                                          uint32_t frame_id, bool is_keyframe)
{
    if (!data || size == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 溢出检查: 如果缓冲区已满, 丢弃非关键帧数据
    if (buffer_.size() + size > MAX_BUFFER_BYTES)
    {
        if (!is_keyframe)
        {
            // 丢弃非关键帧, 保持缓冲区不溢出
            dropped_bytes_ += size;
            std::fprintf(stderr, "BitstreamPacketizer: buffer overflow, "
                                 "dropping frame %u (%zu bytes, total dropped: %zu)\n",
                         frame_id, size, dropped_bytes_);
            return;
        }
        else
        {
            // 关键帧不能丢, 清空缓冲区为其腾出空间
            size_t cleared = buffer_.size();
            buffer_.clear();
            frame_markers_.clear();
            current_frame_remaining_ = 0;
            current_frag_index_ = 0;
            std::fprintf(stderr, "BitstreamPacketizer: buffer overflow on keyframe, "
                                 "cleared %zu bytes for keyframe %u\n",
                         cleared, frame_id);
        }
    }

    // 记录帧标记
    FrameMarker marker;
    marker.frame_id = frame_id;
    marker.data_size = static_cast<uint32_t>(size);
    marker.is_keyframe = is_keyframe;
    frame_markers_.push_back(marker);

    // 将数据追加到 FIFO
    buffer_.insert(buffer_.end(), data, data + size);
}

BitstreamPacketizer::Packet BitstreamPacketizer::producePacket()
{
    std::lock_guard<std::mutex> lock(mutex_);

    Packet pkt;
    std::memset(&pkt, 0, sizeof(pkt));

    // 检查是否需要切换到下一帧
    while (current_frame_remaining_ == 0 && !frame_markers_.empty())
    {
        const auto &marker = frame_markers_.front();
        current_frame_id_ = marker.frame_id;
        current_frame_remaining_ = marker.data_size;
        current_is_keyframe_ = marker.is_keyframe;
        current_frag_index_ = 0;
        frame_markers_.pop_front();
    }

    if (buffer_.empty() || current_frame_remaining_ == 0)
    {
        // 缓冲区为空, 发送填充包
        PacketHeader hdr;
        hdr.seq_num = seq_num_++;
        hdr.frame_id = static_cast<uint16_t>(current_frame_id_ & 0xFFFF);
        hdr.frag_index = 0;
        hdr.frag_count = 0;
        hdr.flags = FLAG_PADDING;
        hdr.payload_len = 0;

        std::memcpy(pkt.data, &hdr, HEADER_SIZE);
        // payload 已经全部为 0 (memset)

        if (callback_)
        {
            callback_(pkt);
        }
        return pkt;
    }

    // payload_len: 实际载荷字节数 (uint16_t, 0-PAYLOAD_SIZE)
    size_t payload_bytes = std::min(PAYLOAD_SIZE,
                                    static_cast<size_t>(current_frame_remaining_));
    payload_bytes = std::min(payload_bytes, buffer_.size());

    // 判断是否是帧的第一个/最后一个分片
    bool is_frame_start = (current_frag_index_ == 0);
    bool is_frame_end = (payload_bytes >= current_frame_remaining_);

    // 构建包头
    PacketHeader hdr;
    hdr.seq_num = seq_num_++;
    hdr.frame_id = static_cast<uint16_t>(current_frame_id_ & 0xFFFF);
    hdr.frag_index = current_frag_index_;
    hdr.frag_count = 0; // 流式模式下不预知总分片数
    hdr.flags = 0;
    hdr.payload_len = static_cast<uint16_t>(payload_bytes);

    if (current_is_keyframe_)
    {
        hdr.flags |= FLAG_KEYFRAME;
    }
    if (is_frame_start)
    {
        hdr.flags |= FLAG_FRAME_START;
    }
    if (is_frame_end)
    {
        hdr.flags |= FLAG_FRAME_END;
    }

    // 写入包头
    std::memcpy(pkt.data, &hdr, HEADER_SIZE);

    // 从 FIFO 拷贝载荷
    for (size_t i = 0; i < payload_bytes; ++i)
    {
        pkt.data[HEADER_SIZE + i] = buffer_.front();
        buffer_.pop_front();
    }
    // 剩余部分已经是 0 (memset)

    // 更新状态
    current_frame_remaining_ -= static_cast<uint32_t>(payload_bytes);
    ++current_frag_index_;

    if (callback_)
    {
        callback_(pkt);
    }

    return pkt;
}

size_t BitstreamPacketizer::bufferedBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

void BitstreamPacketizer::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    frame_markers_.clear();
    current_frame_id_ = 0;
    current_is_keyframe_ = false;
    current_frame_remaining_ = 0;
    current_frag_index_ = 0;
    seq_num_ = 0;
    dropped_bytes_ = 0;
}

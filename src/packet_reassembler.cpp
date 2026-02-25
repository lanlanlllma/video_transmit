#include "packet_reassembler.h"

#include <cstdio>
#include <cstring>

PacketReassembler::PacketReassembler()
    : PacketReassembler(Config{})
{}

PacketReassembler::PacketReassembler(const Config &config)
    : config_(config)
{
    frame_buf_.reserve(4096); // 预分配常见帧大小
}

std::optional<PacketReassembler::EncodedFrame>
PacketReassembler::pushPacket(const uint8_t packet[BitstreamPacketizer::PACKET_SIZE])
{
    ++stats_.packets_total;

    // 解析包头
    BitstreamPacketizer::PacketHeader hdr;
    std::memcpy(&hdr, packet, BitstreamPacketizer::HEADER_SIZE);
    const uint8_t *payload = packet + BitstreamPacketizer::HEADER_SIZE;

    // 跳过填充包
    if (hdr.flags & BitstreamPacketizer::FLAG_PADDING)
    {
        ++stats_.packets_padding;
        // 填充包也更新序列号追踪
        expected_seq_ = hdr.seq_num + 1;
        have_expected_seq_ = true;
        return std::nullopt;
    }

    // 检查序列号连续性
    bool seq_ok = checkSeqContinuity(hdr.seq_num);

    // 序列号间断处理
    if (!seq_ok)
    {
        ++stats_.seq_gaps;
        if (state_ == State::IN_FRAME)
        {
            // 正在组装的帧已损坏
            dropCurrentFrame(stats_.drops_seq_gap);
        }
        state_ = State::DROP_UNTIL_KEYFRAME;
    }

    // 更新期望序列号
    expected_seq_ = hdr.seq_num + 1;
    have_expected_seq_ = true;

    bool is_frame_start = (hdr.flags & BitstreamPacketizer::FLAG_FRAME_START) != 0;
    bool is_frame_end = (hdr.flags & BitstreamPacketizer::FLAG_FRAME_END) != 0;
    bool is_keyframe = (hdr.flags & BitstreamPacketizer::FLAG_KEYFRAME) != 0;

    // 状态机处理
    switch (state_)
    {
    case State::DROP_UNTIL_KEYFRAME:
    {
        if (!is_frame_start)
        {
            return std::nullopt; // 继续等待
        }
        if (config_.require_keyframe_after_loss && !is_keyframe)
        {
            return std::nullopt; // 等待关键帧
        }
        // 找到可恢复的帧起始, 进入正常流程
        startNewFrame(hdr);
        state_ = State::IN_FRAME;
        break; // 继续下面的 IN_FRAME 处理
    }

    case State::SEEK_START:
    {
        if (!is_frame_start)
        {
            return std::nullopt; // 等待帧起始
        }
        startNewFrame(hdr);
        state_ = State::IN_FRAME;
        break; // 继续下面的 IN_FRAME 处理
    }

    case State::IN_FRAME:
    {
        // 检查是否是新帧的起始 (说明上一帧缺少 FRAME_END)
        if (is_frame_start && hdr.frame_id != current_frame_id_)
        {
            // 上一帧丢失了 FRAME_END
            dropCurrentFrame(stats_.drops_missing_end);
            startNewFrame(hdr);
            // 继续下面的载荷追加
        }
        break;
    }
    }

    // 此时 state_ 应为 IN_FRAME
    if (state_ != State::IN_FRAME)
    {
        return std::nullopt;
    }

    // 验证分片索引连续性
    if (hdr.frag_index != expected_frag_index_)
    {
        std::fprintf(stderr, "PacketReassembler: frag_index mismatch: "
                             "expected %d, got %d (frame_id=%u)\n",
                     expected_frag_index_, hdr.frag_index, hdr.frame_id);
        dropCurrentFrame(stats_.drops_frag_error);
        state_ = State::DROP_UNTIL_KEYFRAME;
        return std::nullopt;
    }

    // 解析载荷长度: uint16_t 直接表示实际字节数 (0 = 空载荷, 一般不会出现)
    size_t payload_len = hdr.payload_len;
    if (payload_len > BitstreamPacketizer::PAYLOAD_SIZE)
    {
        payload_len = BitstreamPacketizer::PAYLOAD_SIZE; // 安全截断
    }

    // 溢出检查
    if (frame_buf_.size() + payload_len > config_.max_frame_bytes)
    {
        std::fprintf(stderr, "PacketReassembler: frame too large "
                             "(>%zu bytes), dropping frame_id=%u\n",
                     config_.max_frame_bytes, current_frame_id_);
        dropCurrentFrame(stats_.drops_overflow);
        state_ = State::DROP_UNTIL_KEYFRAME;
        return std::nullopt;
    }

    frame_buf_.insert(frame_buf_.end(), payload, payload + payload_len);
    ++expected_frag_index_;

    // 检查帧是否完整
    if (is_frame_end)
    {
        EncodedFrame frame;
        frame.frame_id = current_frame_id_;
        frame.keyframe = current_keyframe_;
        frame.data = std::move(frame_buf_);

        frame_buf_.clear();
        frame_buf_.reserve(4096);
        ++stats_.frames_emitted;

        state_ = State::SEEK_START;
        return frame;
    }

    return std::nullopt;
}

void PacketReassembler::reset()
{
    state_ = State::SEEK_START;
    have_expected_seq_ = false;
    expected_seq_ = 0;
    current_frame_id_ = 0;
    current_keyframe_ = false;
    expected_frag_index_ = 0;
    frame_buf_.clear();
    stats_ = Stats{};
}

bool PacketReassembler::checkSeqContinuity(uint16_t seq)
{
    if (!have_expected_seq_)
    {
        return true; // 第一个包, 无法判断
    }

    // 处理 wrap-around: uint16_t 自动取模
    return seq == expected_seq_;
}

void PacketReassembler::dropCurrentFrame(uint64_t &stats_field_ref)
{
    ++stats_.frames_dropped;
    ++stats_field_ref;
    frame_buf_.clear();
    expected_frag_index_ = 0;
}

void PacketReassembler::startNewFrame(const BitstreamPacketizer::PacketHeader &hdr)
{
    current_frame_id_ = hdr.frame_id;
    current_keyframe_ = (hdr.flags & BitstreamPacketizer::FLAG_KEYFRAME) != 0;
    expected_frag_index_ = hdr.frag_index; // 应为 0
    frame_buf_.clear();
}

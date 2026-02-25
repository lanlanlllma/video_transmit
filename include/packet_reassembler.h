#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "bitstream_packetizer.h"

// 数据包重组器
// 将固定 300 字节分片包重新组装为完整编码帧
// 与 BitstreamPacketizer 对称: 分包器拆帧, 重组器组帧
//
// 状态机:
//   SEEK_START → 等待 FRAME_START 标志
//   IN_FRAME   → 累积分片载荷, 直到 FRAME_END
//   DROP_UNTIL_KEYFRAME → 丢包后等待关键帧恢复
//
// 丢包处理:
//   - seq_num 间断 → 丢弃当前帧, 等待下一个关键帧 FRAME_START
//   - frag_index 不连续 → 同上
//   - FRAME_END 丢失 → 下一个 FRAME_START 时丢弃旧帧
//   - 填充包 (FLAG_PADDING) → 直接跳过

class PacketReassembler
{
public:
    struct Config
    {
        size_t max_frame_bytes = 2 * 1024 * 1024; // 单帧最大字节数
        bool require_keyframe_after_loss = true;   // 丢包后是否要求关键帧恢复
    };

    struct Stats
    {
        uint64_t packets_total = 0;    // 总接收包数
        uint64_t packets_padding = 0;  // 填充包数
        uint64_t seq_gaps = 0;         // 序列号间断次数
        uint64_t frames_emitted = 0;   // 成功输出帧数
        uint64_t frames_dropped = 0;   // 丢弃帧数 (总计)
        uint64_t drops_missing_end = 0;   // 因缺少 FRAME_END 丢弃
        uint64_t drops_seq_gap = 0;       // 因序列号间断丢弃
        uint64_t drops_frag_error = 0;    // 因分片索引错误丢弃
        uint64_t drops_overflow = 0;      // 因超过最大帧大小丢弃
    };

    // 重组后的完整编码帧
    struct EncodedFrame
    {
        uint16_t frame_id = 0;
        bool keyframe = false;
        std::vector<uint8_t> data; // 完整 AV1 OBU 比特流
    };

    PacketReassembler();
    explicit PacketReassembler(const Config &config);
    ~PacketReassembler() = default;

    // 不允许拷贝
    PacketReassembler(const PacketReassembler &) = delete;
    PacketReassembler &operator=(const PacketReassembler &) = delete;

    // 送入一个 300 字节数据包
    // 如果组装出完整帧, 返回 EncodedFrame; 否则返回 nullopt
    std::optional<EncodedFrame> pushPacket(const uint8_t packet[BitstreamPacketizer::PACKET_SIZE]);

    // 重置状态
    void reset();

    // 获取统计信息
    const Stats &stats() const { return stats_; }

private:
    enum class State
    {
        SEEK_START,         // 等待帧起始
        IN_FRAME,           // 正在接收帧分片
        DROP_UNTIL_KEYFRAME // 丢包后等待关键帧
    };

    // 检查序列号是否连续 (处理 wrap-around)
    bool checkSeqContinuity(uint16_t seq);

    // 丢弃当前正在组装的帧
    void dropCurrentFrame(uint64_t &stats_field_ref);

    // 开始新帧
    void startNewFrame(const BitstreamPacketizer::PacketHeader &hdr);

    Config config_;
    Stats stats_;
    State state_ = State::SEEK_START;

    // 序列号追踪
    bool have_expected_seq_ = false;
    uint16_t expected_seq_ = 0;

    // 当前正在组装的帧
    uint16_t current_frame_id_ = 0;
    bool current_keyframe_ = false;
    uint8_t expected_frag_index_ = 0;
    std::vector<uint8_t> frame_buf_;
};

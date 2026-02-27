#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

// 固定大小数据包分片器
// 将变长编码数据切分为固定 300 字节 (2400 bit) 的数据包
// 设计用于 50Hz 发送频率, 总带宽 120kbps
//
// 数据包格式 (300 bytes total):
//   [Header: 9 bytes] [Payload: 291 bytes]
//
// Header layout (packed, no padding):
//   uint16_t seq_num;       // 包序列号 (全局递增)
//   uint16_t frame_id;      // 编码帧 ID (wrap around at 65536)
//   uint8_t  frag_index;    // 当前帧的分片索引 (从 0 开始)
//   uint8_t  frag_count;    // 当前帧的总分片数 (0 = 未知/流式)
//   uint8_t  flags;         // 标志位
//   uint16_t payload_len;   // 有效载荷长度 (实际字节数, 0-291)
//
// Flags:
//   bit 0: KEYFRAME       - 该分片属于关键帧
//   bit 1: FRAME_START    - 帧的第一个分片
//   bit 2: FRAME_END      - 帧的最后一个分片
//   bit 3: PADDING        - 填充包 (无有效数据)

class BitstreamPacketizer
{
public:
    static constexpr size_t PACKET_SIZE = 300;    // 总包大小 (bytes)
    static constexpr size_t HEADER_SIZE = 9;      // 包头大小 (bytes)
    static constexpr size_t PAYLOAD_SIZE = 291;   // 有效载荷大小 (bytes)
    static constexpr int SEND_RATE_HZ = 50;       // 发送频率
    static constexpr size_t MAX_BUFFER_BYTES = 70000; // FIFO 最大缓冲 (bytes)

    // 标志位定义
    enum Flags : uint8_t
    {
        FLAG_KEYFRAME    = 0x01,
        FLAG_FRAME_START = 0x02,
        FLAG_FRAME_END   = 0x04,
        FLAG_PADDING     = 0x08,
    };

    // 包头结构 (packed to exactly 9 bytes)
#pragma pack(push, 1)
    struct PacketHeader
    {
        uint16_t seq_num;
        uint16_t frame_id;
        uint8_t frag_index;
        uint8_t frag_count;
        uint8_t flags;
        uint16_t payload_len;   // 有效载荷长度 (实际字节数, 0-291)
    };
#pragma pack(pop)
    static_assert(sizeof(PacketHeader) == HEADER_SIZE, "PacketHeader must be 9 bytes");

    // 固定大小输出包
    struct Packet
    {
        uint8_t data[PACKET_SIZE];

        PacketHeader header() const
        {
            PacketHeader h;
            std::memcpy(&h, data, HEADER_SIZE);
            return h;
        }

        const uint8_t *payload() const { return data + HEADER_SIZE; }
    };
    static_assert(sizeof(Packet) == PACKET_SIZE, "Packet must be 300 bytes");

    // 输出回调: 每次产生一个固定大小包时调用
    using PacketCallback = std::function<void(const Packet &packet)>;

    BitstreamPacketizer();
    ~BitstreamPacketizer() = default;

    // 不允许拷贝
    BitstreamPacketizer(const BitstreamPacketizer &) = delete;
    BitstreamPacketizer &operator=(const BitstreamPacketizer &) = delete;

    // 设置输出回调
    void setCallback(PacketCallback cb);

    // 将编码帧数据推入 FIFO 缓冲区
    // data: 编码后的原始数据
    // size: 数据大小
    // frame_id: 帧编号
    // is_keyframe: 是否为关键帧
    void pushEncodedData(const uint8_t *data, size_t size,
                         uint32_t frame_id, bool is_keyframe);

    // 产生一个输出包 (由定时器或主循环调用, 50Hz)
    // 如果缓冲区有数据, 输出编码数据包
    // 如果缓冲区为空, 输出填充包
    // 返回产生的包
    Packet producePacket();

    // 获取当前缓冲区中的字节数
    size_t bufferedBytes() const;

    // 获取已产生的总包数
    uint32_t totalPackets() const { return seq_num_; }

    // 获取丢弃的字节数 (由于溢出)
    size_t droppedBytes() const { return dropped_bytes_; }

    // 清空缓冲区
    void reset();

private:
    // 内部帧元数据, 嵌入到 FIFO 流中
    struct FrameMarker
    {
        uint32_t frame_id;
        uint32_t data_size;   // 该帧的总数据大小
        bool is_keyframe;
    };

    // FIFO 缓冲区 (字节流)
    std::deque<uint8_t> buffer_;

    // 帧标记队列: 记录每帧在 buffer_ 中的位置信息
    std::deque<FrameMarker> frame_markers_;

    // 当前正在分片的帧信息
    uint32_t current_frame_id_ = 0;
    bool current_is_keyframe_ = false;
    uint32_t current_frame_remaining_ = 0; // 当前帧还剩多少字节
    uint8_t current_frag_index_ = 0;

    // 全局状态
    uint32_t seq_num_ = 0;
    size_t dropped_bytes_ = 0;

    PacketCallback callback_;
    mutable std::mutex mutex_;
};

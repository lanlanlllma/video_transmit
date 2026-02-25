# WebSocket 流式传输示例

## 概述

演示实际用例: **视频源 → AV1 编码 → WebSocket 传输 → AV1 解码 → 实时显示**

```
┌─────────────┐    300B/pkt @ 50Hz     ┌──────────────┐
│  ws_sender   │ ──── WebSocket ────→  │  ws_receiver  │
│              │                        │               │
│ 视频/摄像头   │    120kbps 固定带宽     │ 实时预览+保存  │
│ → 预处理     │                        │ AV1解码       │
│ → AV1编码    │                        │ → bitshift反转│
│ → 300B分包   │                        │ → 显示/写文件  │
└─────────────┘                        └──────────────┘
```

两端共用同一份 `config.yaml`, 保证编解码参数一致。

## 文件说明

| 文件 | 说明 |
|------|------|
| `ws_sender.cpp` | 发送端: 读取视频源 → 编码 → WebSocket 服务端, 广播包给所有客户端 |
| `ws_receiver.cpp` | 接收端: WebSocket 客户端 → 解码 → 实时预览 + 可选保存视频 |

## WebSocket 库选择

示例代码支持两种模式:

### 1. IXWebSocket (推荐)

```bash
# Ubuntu/Debian
sudo apt install libixwebsocket-dev

# 或 vcpkg
vcpkg install ixwebsocket
```

编译 sender 时加 `-DUSE_IXWEBSOCKET`:
```bash
g++ -std=c++17 -O2 -DUSE_IXWEBSOCKET ws_sender.cpp \
    ../src/compressed_stream_pipeline.cpp ../src/stream_encoder.cpp \
    ../src/bitstream_packetizer.cpp ../src/frame_preprocessor.cpp \
    ../src/pipeline_config.cpp \
    -I../include $(pkg-config --cflags --libs libavcodec libavutil libavformat libswscale) \
    $(pkg-config --cflags --libs opencv4) -lixwebsocket -lyaml-cpp -lpthread -lz -lssl -lcrypto \
    -o ws_sender
```

编译 receiver:
```bash
g++ -std=c++17 -O2 -DUSE_IXWEBSOCKET ws_receiver.cpp \
    ../src/decoder_pipeline.cpp ../src/stream_decoder.cpp \
    ../src/packet_reassembler.cpp ../src/bitstream_packetizer.cpp \
    ../src/pipeline_config.cpp \
    -I../include $(pkg-config --cflags --libs libavcodec libavutil libavformat libswscale) \
    $(pkg-config --cflags --libs opencv4) -lixwebsocket -lyaml-cpp -lpthread -lz -lssl -lcrypto \
    -o ws_receiver
```

### 2. TCP Stub (无需额外依赖)

不定义 `USE_IXWEBSOCKET` 时, 自动使用裸 TCP 传输:
```bash
# sender
g++ -std=c++17 -O2 ws_sender.cpp \
    ../src/compressed_stream_pipeline.cpp ../src/stream_encoder.cpp \
    ../src/bitstream_packetizer.cpp ../src/frame_preprocessor.cpp \
    ../src/pipeline_config.cpp \
    -I../include $(pkg-config --cflags --libs libavcodec libavutil libavformat libswscale) \
    $(pkg-config --cflags --libs opencv4) -lyaml-cpp -lpthread \
    -o ws_sender

# receiver
g++ -std=c++17 -O2 ws_receiver.cpp \
    ../src/decoder_pipeline.cpp ../src/stream_decoder.cpp \
    ../src/packet_reassembler.cpp ../src/bitstream_packetizer.cpp \
    ../src/pipeline_config.cpp \
    -I../include $(pkg-config --cflags --libs libavcodec libavutil libavformat libswscale) \
    $(pkg-config --cflags --libs opencv4) -lyaml-cpp -lpthread \
    -o ws_receiver
```

TCP 模式直接发送裸 300 字节包, 仅用于本机测试。生产环境请使用 WebSocket。

## 快速测试

```bash
# 终端 1: 启动发送端 (读取本地视频, 模拟实时帧率)
./ws_sender ../videos/test_video1.mp4 --config ../config.yaml --port 9002

# 终端 2: 启动接收端 (连接发送端, 实时预览)
./ws_receiver --config ../config.yaml --url ws://127.0.0.1:9002

# 接收端同时保存视频
./ws_receiver --config ../config.yaml --url ws://127.0.0.1:9002 --output received.mp4
```

### 摄像头输入

```bash
# USB 摄像头 (index 0)
./ws_sender 0 --config ../config.yaml

# V4L2 设备
./ws_sender /dev/video0 --config ../config.yaml

# RTSP 网络摄像头
./ws_sender rtsp://admin:pass@192.168.1.100:554/stream --config ../config.yaml
```

## 集成到你的项目

核心代码量极少, 关键调用:

### 发送端 (编码 + 发包)

```cpp
#include "compressed_stream_pipeline.h"
#include "pipeline_config.h"

// 1. 加载配置
PipelineConfig pcfg;
PipelineConfig::load("config.yaml", pcfg);

// 2. 创建编码管线
CompressedStreamPipeline::Config cfg;
cfg.encode_width = pcfg.encode_width;
cfg.encode_height = pcfg.encode_height;
cfg.fps = pcfg.encode_fps;
cfg.bitrate = pcfg.bitrate;
cfg.bitshift = pcfg.bitshift;
// ... 其他参数从 pcfg 映射

CompressedStreamPipeline pipeline(cfg);

// 3. 设置包回调 → 发到你的 WebSocket
pipeline.setPacketCallback([&ws](const auto &pkt) {
    ws.sendBinary(pkt.data, 300);
});

// 4. 定频喂帧
pipeline.feedFrame(bgr_frame);

// 5. 按 50Hz 比例产生包
pipeline.producePacket();  // 内部触发回调
```

### 接收端 (收包 + 解码)

```cpp
#include "decoder_pipeline.h"
#include "pipeline_config.h"

// 1. 加载配置
PipelineConfig pcfg;
PipelineConfig::load("config.yaml", pcfg);

// 2. 创建解码管线
DecoderPipeline::Config cfg;
cfg.decoder.bitshift = pcfg.bitshift;
cfg.decoder.reverse_bitshift = true;
cfg.decoder.prefer_dav1d = pcfg.prefer_dav1d;
cfg.fps = pcfg.output_fps;
cfg.show_preview = true;

DecoderPipeline pipeline(cfg);
pipeline.open();

// 3. WebSocket 收到包后直接推入管线
ws.onMessage([&](const uint8_t *data, size_t len) {
    if (len == 300)
        pipeline.pushPacket(data);
});
```

## 时序说明

```
发送端时序:
  input_fps=30 → cap.read() 每 33ms 一帧
  encode_fps=24 → 累加器跳帧, 24/30 帧被编码
  50Hz 包速率  → 每帧产生 50/24≈2.08 包 (整数分配: 2或3包/帧)
  每 20ms 一个包通过 WebSocket 发出

接收端时序:
  WebSocket 收到包 → 入队列
  解码线程: 出队列 → pushPacket → 重组 → 解码 → 显示
  队列满时丢弃旧包, 保持实时性
```

# video_compress

### 1. 项目简介
video_compress 是一个专门为超低码率设计的 AV1 视频压缩流管线, 包含了编码端和解码端。本项目的核心目标是实现固定的 300 字节数据包传输。在 50Hz 的发送频率下, 总带宽严格控制在 120kbps (2400bit/pack × 50Hz)。系统底层通过 FFmpeg libavcodec 调用 libaom-av1 进行编码, 并使用 libdav1d 或 libaom 进行解码。

### 2. 系统架构
全流程管线如下所示:
```
编码端: 视频帧 → 帧率转换 → 预处理(灰度/bitshift/gamma) → AV1编码(CBR) → 固定分包(300B) → .bin文件
解码端: .bin文件 → 包重组 → AV1解码 → bitshift反转 → 保存视频 + 实时预览
```

### 3. 项目结构
```
config.yaml                  - 共享配置文件 (编码器/解码器共用)
include/
  pipeline_config.h          - 配置文件加载/保存 (YAML)
  bitstream_packetizer.h     - 固定大小分包器 (300字节/包)
  compressed_stream_pipeline.h - 编码管线 (预处理→编码→分包)
  frame_preprocessor.h       - 帧预处理器 (灰度/bitshift/gamma/暗部增强)
  stream_encoder.h           - AV1 流式编码器 (libaom-av1 CBR)
  packet_reassembler.h       - 包重组器 (状态机: SEEK_START→IN_FRAME→DROP_UNTIL_KEYFRAME)
  stream_decoder.h           - AV1 流式解码器 (libdav1d/libaom)
  decoder_pipeline.h         - 解码管线 (重组→解码→输出)
src/
  main.cpp                   - 编码器 CLI
  decode_main.cpp            - 解码器 CLI
  pipeline_config.cpp        - 配置文件实现
scripts/
  compress_av1.py            - Python 参考实现
  preprocess.py              - Python 预处理参考
```

### 4. 数据包格式
每个数据包总长度固定为 300 字节, 结构如下:
```
[Header: 9 bytes][Payload: 291 bytes] = 300 bytes
```
Header 部分 (紧凑排列, 无对齐填充):
- uint16_t seq_num: 包序列号, 全局递增。
- uint16_t frame_id: 帧 ID。
- uint8_t frag_index: 分片索引, 从 0 开始。
- uint8_t frag_count: 分片总数, 0 表示流式传输。
- uint8_t flags: 标志位 (KEYFRAME|FRAME_START|FRAME_END|PADDING)。
- uint16_t payload_len: 有效载荷字节数, 范围为 0 到 291。

### 5. 依赖
- CMake >= 3.10
- 支持 C++17 的编译器 (GCC/Clang)
- FFmpeg (包含 libavcodec, libavutil, libavformat, libswscale), 需支持 libaom-av1 和 libdav1d
- OpenCV 4.x
- yaml-cpp >= 0.6
- pkg-config

### 6. 编译
使用以下命令进行构建:
```bash
cmake -B build -G Ninja .
cmake --build build
```
编译完成后会生成两个二进制文件: `video_compress` (编码器) 和 `decode_packets` (解码器)。

### 7. 配置文件
编码器和解码器共用一份 YAML 配置文件 `config.yaml`, 确保编解码参数一致。默认路径为 `./config.yaml`, 可通过 `--config <path>` 指定。

配置项分为五个部分:
```yaml
input:
  fps: 30.0              # 输入视频帧率 (固定值, 用于帧率转换)

encode:
  width: 480             # 编码宽度
  height: 480            # 编码高度
  fps: 24                # 编码目标帧率
  bitrate: 120000        # 目标码率 (bit/s)
  gop: 48                # 关键帧间隔
  cpu_used: 8            # libaom 速度等级 (0-8)
  threads: 4             # 编码线程数

preprocess:
  bitshift: 4            # 比特右移位数 (0-7)
  gamma: 1.0             # Gamma 校正
  shadow_gain: 1.4       # 暗部提升倍数
  shadow_threshold: 64   # 暗部阈值
  shadow_floor: 0        # 暗部舍弃阈值
  highlight_roof: 255    # 高亮截断阈值
  edge_enhance: 0.0      # 边缘增强 (0-1)
  high_freq_boost: 0.0   # 高频增强 (0-1)
  motion_enhance: 0.0    # 运动增强 (0-1)
  center_crop: 1.0       # 中心裁剪比例 (0-1)

decode:
  threads: 0             # 解码线程数 (0=自动)
  prefer_dav1d: true     # 优先使用 libdav1d 解码器

output:
  fps: 24.0              # 输出视频帧率
```

帧率转换说明: 编码器使用累加器方式进行帧率转换。当 `input.fps` (如 30) 大于 `encode.fps` (如 24) 时, 会均匀跳帧以匹配目标帧率, 避免整数跳帧导致的不均匀问题。

### 8. 使用方法
编码器示例:
```bash
# 使用默认配置 (./config.yaml)
./build/video_compress input.mp4 --output output.bin --max-frames 100

# 指定配置文件, 开启调试预览
./build/video_compress input.mp4 --config my_config.yaml --output output.bin --debug
```
编码器选项:
| 选项 | 说明 |
|------|------|
| `--config <path>` | 配置文件路径 (默认: ./config.yaml) |
| `--output <path>` | 输出文件路径 (默认: output_packets.bin) |
| `--max-frames <int>` | 最大编码帧数 (默认: 无限制) |
| `--realtime` | 模拟 50Hz 实时包输出 |
| `--debug` | 显示预处理后的图像预览窗口 |

解码器示例:
```bash
# 使用默认配置
./build/decode_packets output.bin --output decoded.mp4

# 仅预览不保存
./build/decode_packets output.bin --no-save

# 仅保存不预览
./build/decode_packets output.bin --output decoded.mp4 --no-preview
```
解码器选项:
| 选项 | 说明 |
|------|------|
| `--config <path>` | 配置文件路径 (默认: ./config.yaml) |
| `--output <path>` | 输出视频路径 (默认: decoded_output.mp4) |
| `--no-preview` | 不显示预览窗口 |
| `--no-save` | 不保存视频文件 (仅预览) |

### 9. 编码参数说明
- bitshift: 将像素值右移 N 位以压缩动态范围, 解码时会自动左移恢复。
- gamma: 执行 Gamma 校正。
- shadow-gain/threshold/floor: 用于暗部区域的增强处理。
- highlight-roof: 对高亮区域进行截断。
- edge-enhance: 增强边缘锐度。
- gop: 设置关键帧间隔, 直接影响丢包后的恢复延迟。
- cpu-used: libaom 的速度等级, 0 表示最慢但压缩效果最好, 8 表示实时速度。

### 10. 丢包恢复策略
包重组器通过内部状态机处理网络丢包引起的异常:
1. 当检测到 seq_num 不连续时, 系统会丢弃当前正在组装的帧。
2. 程序会进入等待状态, 直到接收到下一个带有 KEYFRAME 且包含 FRAME_START 标志的数据包。
3. 如果组帧过程中丢失了 FRAME_END 包, 该帧会在收到下一个 FRAME_START 时被判定为无效并丢弃。
4. 系统的最大恢复延迟取决于 GOP 设置。以默认的 48 帧 GOP 为例, 在 24fps 速率下, 恢复正常显示最多需要 2 秒。

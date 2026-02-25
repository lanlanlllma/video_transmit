cmake_minimum_required(VERSION 3.10)
project(video_compress CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libswscale)

find_package(OpenCV REQUIRED)
find_package(yaml-cpp REQUIRED)

add_executable(video_compress
  src/main.cpp
  src/stream_encoder.cpp
  src/bitstream_packetizer.cpp
  src/compressed_stream_pipeline.cpp
  src/frame_preprocessor.cpp
  src/pipeline_config.cpp
)

target_include_directories(video_compress PRIVATE
  ${OpenCV_INCLUDE_DIRS}
  ${FFMPEG_INCLUDE_DIRS}
  include
)

target_link_libraries(video_compress PRIVATE
  PkgConfig::FFMPEG
  ${OpenCV_LIBS}
  yaml-cpp::yaml-cpp
)

# 解码器工具
add_executable(decode_packets
  src/decode_main.cpp
  src/decoder_pipeline.cpp
  src/stream_decoder.cpp
  src/packet_reassembler.cpp
  src/bitstream_packetizer.cpp
  src/pipeline_config.cpp
)

target_include_directories(decode_packets PRIVATE
  ${OpenCV_INCLUDE_DIRS}
  ${FFMPEG_INCLUDE_DIRS}
  include
)

target_link_libraries(decode_packets PRIVATE
  PkgConfig::FFMPEG
  ${OpenCV_LIBS}
  yaml-cpp::yaml-cpp
)

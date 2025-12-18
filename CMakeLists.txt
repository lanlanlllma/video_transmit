cmake_minimum_required(VERSION 3.10)
project(video_compress CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libswscale)

find_package(OpenCV REQUIRED)

add_executable(video_compress
  src/main.cpp
  src/transcoder.cpp
  src/frame_preprocessor.cpp
 )

add_executable(video_decompress
  src/decode_main.cpp
  src/bitshift_decoder.cpp
 )

target_include_directories(video_compress PRIVATE ${OpenCV_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS} include)
target_include_directories(video_decompress PRIVATE ${OpenCV_INCLUDE_DIRS} include)

target_link_libraries(video_compress PRIVATE PkgConfig::FFMPEG ${OpenCV_LIBS})
target_link_libraries(video_decompress PRIVATE ${OpenCV_LIBS})
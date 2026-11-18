add_library(cd_media STATIC PngEncoder.cpp GifEncoder.cpp CaptureQueue.cpp Mp4Encoder.cpp AvSync.cpp)
target_link_libraries(cd_media PUBLIC cd_core)
cd_core_target(cd_media)

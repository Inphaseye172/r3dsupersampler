add_library(r3dss_codec_cdng STATIC src/tiff.cpp src/ljpeg.cpp src/wav.cpp src/cdng_codec.cpp)
add_library(r3dss::codec_cdng ALIAS r3dss_codec_cdng)
target_include_directories(r3dss_codec_cdng PUBLIC include)
target_link_libraries(r3dss_codec_cdng PUBLIC r3dss::codec)

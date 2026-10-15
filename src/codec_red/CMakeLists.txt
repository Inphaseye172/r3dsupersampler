# RED chunked container (.R3D, R3D NE, NEV): chunk walker, header strings, source detection and a
# container-only reader used when the RED R3D SDK is not available. No SDK code here.
add_library(r3dss_codec_red STATIC src/red_container.cpp src/red_detect.cpp src/red_codec.cpp)
add_library(r3dss::codec_red ALIAS r3dss_codec_red)
target_include_directories(r3dss_codec_red PUBLIC include)
target_link_libraries(r3dss_codec_red PUBLIC r3dss::codec)

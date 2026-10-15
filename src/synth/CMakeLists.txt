add_library(r3dss_synth STATIC src/synth.cpp)
add_library(r3dss::synth ALIAS r3dss_synth)
target_include_directories(r3dss_synth PUBLIC include)
target_link_libraries(r3dss_synth PUBLIC r3dss::codec r3dss::codec_cdng)

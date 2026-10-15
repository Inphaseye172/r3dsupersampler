add_library(r3dss_codec STATIC src/registry.cpp src/memory_codec.cpp)
add_library(r3dss::codec ALIAS r3dss_codec)
target_include_directories(r3dss_codec PUBLIC include)
target_link_libraries(r3dss_codec PUBLIC r3dss::core)

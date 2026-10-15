add_library(r3dss_ratecontrol STATIC src/ratecontrol.cpp)
add_library(r3dss::ratecontrol ALIAS r3dss_ratecontrol)
target_include_directories(r3dss_ratecontrol PUBLIC include)
target_link_libraries(r3dss_ratecontrol PUBLIC r3dss::core r3dss::codec)

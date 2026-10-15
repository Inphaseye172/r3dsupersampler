add_library(r3dss_resample STATIC src/taps.cpp src/cpu_resampler.cpp src/size_planner.cpp)
add_library(r3dss::resample ALIAS r3dss_resample)
target_include_directories(r3dss_resample PUBLIC include)
target_link_libraries(r3dss_resample PUBLIC r3dss::core)

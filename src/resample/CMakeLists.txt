add_library(r3dss_resample STATIC src/taps.cpp src/cpu_resampler.cpp src/size_planner.cpp)
add_library(r3dss::resample ALIAS r3dss_resample)
target_include_directories(r3dss_resample PUBLIC include)
target_link_libraries(r3dss_resample PUBLIC r3dss::core)
# The clipping rule compares a float32 result against white - 1 (docs/RESAMPLING.md §4). Keep every multiply and
# add separately rounded (no fused multiply-add), exactly like the CUDA kernels' __fmul_rn/__fadd_rn, so CPU and
# GPU take the same branch at the threshold. MSVC's default /fp:precise does not contract.
if(NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(r3dss_resample PRIVATE -ffp-contract=off)
endif()

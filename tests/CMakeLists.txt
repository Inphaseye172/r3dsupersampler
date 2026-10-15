find_package(Catch2 3 CONFIG REQUIRED)
add_executable(r3dss-tests
  test_core.cpp
  test_taps_resampler.cpp
  test_ratecontrol.cpp
  test_ljpeg_tiff_wav.cpp
  test_cdng.cpp
  test_pipeline.cpp
  test_cuda.cpp
)
target_link_libraries(r3dss-tests PRIVATE r3dss::pipeline r3dss::synth Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(r3dss-tests PROPERTIES SKIP_RETURN_CODE 4)

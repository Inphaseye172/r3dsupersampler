find_package(Catch2 3 CONFIG REQUIRED)
add_executable(r3dss-tests
  test_core.cpp
  test_taps_resampler.cpp
  test_ratecontrol.cpp
  test_ljpeg_tiff_wav.cpp
  test_cdng.cpp
  test_cdng_reader.cpp
  test_cdng_golden.cpp
  test_pipeline.cpp
  test_cuda.cpp
  test_red_container.cpp
  test_samples.cpp
  test_r3dsdk.cpp
)
target_link_libraries(r3dss-tests PRIVATE r3dss::pipeline r3dss::synth Catch2::Catch2WithMain)
target_include_directories(r3dss-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(r3dss-tests PRIVATE R3DSS_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
include(Catch)
catch_discover_tests(r3dss-tests PROPERTIES SKIP_RETURN_CODE 4)

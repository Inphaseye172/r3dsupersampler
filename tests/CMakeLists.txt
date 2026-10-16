find_package(Catch2 3 CONFIG REQUIRED)
add_executable(r3dss-tests
  test_core.cpp
  test_taps_resampler.cpp
  test_ratecontrol.cpp
  test_ljpeg_tiff_wav.cpp
  test_cdng.cpp
  test_pipeline.cpp
  test_pipeline_safety.cpp
  test_cuda.cpp
  test_red_container.cpp
  test_samples.cpp
  test_r3dsdk.cpp
)
target_link_libraries(r3dss-tests PRIVATE r3dss::pipeline r3dss::synth Catch2::Catch2WithMain)
target_include_directories(r3dss-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
include(Catch)
catch_discover_tests(r3dss-tests PROPERTIES SKIP_RETURN_CODE 4)

# I6: the GUI's headless runner converts a scanned job through the same engine path as the window.
if(R3DSS_HAVE_GUI)
  add_test(NAME "GUI --run-job converts a scanned job headlessly (I6)"
    COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:r3dss-cli> -DGUI=$<TARGET_FILE:r3dss-gui>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/gui_run_job -P ${CMAKE_CURRENT_SOURCE_DIR}/gui_run_job.cmake)
  set_tests_properties("GUI --run-job converts a scanned job headlessly (I6)" PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

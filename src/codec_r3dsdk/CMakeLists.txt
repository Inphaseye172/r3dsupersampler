# RED R3D SDK decoder module. The SDK is proprietary and never lives in this repository: point
# R3DSDK_ROOT at the unpacked SDK (Include/, Lib/, Redistributable/). Without it the module still
# builds and reports "SDK not found" so the rest of the product keeps working.
if(R3DSS_HAVE_R3DSDK)
  add_library(r3dss_codec_r3dsdk STATIC src/red_family_factory.cpp src/r3dsdk_runtime.cpp src/r3dsdk_reader.cpp)
  target_link_libraries(r3dss_codec_r3dsdk PUBLIC r3dss::codec_red PRIVATE R3DSDK::R3DSDK)
  target_compile_definitions(r3dss_codec_r3dsdk PUBLIC R3DSS_HAVE_R3DSDK=1
    PRIVATE R3DSS_R3DSDK_LIBDIR="${R3DSDK_RUNTIME_DIR}")
  if(NOT WIN32)
    target_link_libraries(r3dss_codec_r3dsdk PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  endif()
  # Runtime libraries go next to the executables under redsdk/ (also what the installer bundles).
  install(DIRECTORY "${R3DSDK_RUNTIME_DIR}/" DESTINATION bin/redsdk)
else()
  add_library(r3dss_codec_r3dsdk STATIC src/red_family_factory.cpp src/r3dsdk_unavailable.cpp)
  target_link_libraries(r3dss_codec_r3dsdk PUBLIC r3dss::codec_red)
endif()
add_library(r3dss::codec_r3dsdk ALIAS r3dss_codec_r3dsdk)
target_include_directories(r3dss_codec_r3dsdk PUBLIC include)
target_link_libraries(r3dss_codec_r3dsdk PUBLIC r3dss::core)

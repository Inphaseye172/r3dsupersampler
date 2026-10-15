add_library(r3dss_pipeline STATIC
  src/builtin_codecs.cpp
  src/job.cpp
  src/naming.cpp
  src/folder_scanner.cpp
  src/archive_copier.cpp
  src/verifier.cpp
  src/clip_converter.cpp
  src/scheduler.cpp
)
add_library(r3dss::pipeline ALIAS r3dss_pipeline)
target_include_directories(r3dss_pipeline PUBLIC include)
target_link_libraries(r3dss_pipeline PUBLIC r3dss::core r3dss::codec r3dss::codec_cdng r3dss::codec_red
  r3dss::codec_r3dsdk r3dss::resample r3dss::ratecontrol nlohmann_json::nlohmann_json)
if(R3DSS_HAVE_CUDA)
  target_link_libraries(r3dss_pipeline PUBLIC r3dss::resample_cuda)
  target_compile_definitions(r3dss_pipeline PUBLIC R3DSS_HAVE_CUDA=1)
endif()

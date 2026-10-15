add_library(r3dss_core STATIC
  src/logger.cpp
  src/sha256.cpp
  src/timecode.cpp
  src/types.cpp
  src/hardware.cpp
  src/pool.cpp
)
add_library(r3dss::core ALIAS r3dss_core)
target_include_directories(r3dss_core PUBLIC include)
target_link_libraries(r3dss_core PUBLIC Threads::Threads r3dss::warnings)
target_compile_features(r3dss_core PUBLIC cxx_std_20)

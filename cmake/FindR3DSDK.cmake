# Locates the RED R3D SDK (decode only). Download it from RED after accepting their license and
# set R3DSDK_ROOT (cache or environment) to the unpacked folder containing Include/, Lib/ and
# Redistributable/. Headers and libraries are never committed to this repository.
#
# Provides: R3DSDK::R3DSDK (imported static library), R3DSDK_RUNTIME_DIR (shared libraries that
# must sit next to the executable; passed in full to InitializeSdk()).
set(R3DSDK_ROOT "$ENV{R3DSDK_ROOT}" CACHE PATH "RED R3D SDK root")
# Forward slashes only: the runtime folder is baked into the code as a C string literal, where
# Windows backslashes would become escape sequences.
file(TO_CMAKE_PATH "${R3DSDK_ROOT}" R3DSDK_ROOT)
find_path(R3DSDK_INCLUDE_DIR NAMES R3DSDK.h PATHS "${R3DSDK_ROOT}/Include" NO_DEFAULT_PATH)
if(WIN32)
  find_library(R3DSDK_LIBRARY NAMES R3DSDK-2017MD PATHS "${R3DSDK_ROOT}/Lib/win64" NO_DEFAULT_PATH)
  find_library(R3DSDK_LIBRARY_DEBUG NAMES R3DSDK-2017MDd PATHS "${R3DSDK_ROOT}/Lib/win64" NO_DEFAULT_PATH)
  set(R3DSDK_RUNTIME_DIR "${R3DSDK_ROOT}/Redistributable/win")
elseif(APPLE)
  find_library(R3DSDK_LIBRARY NAMES R3DSDK PATHS "${R3DSDK_ROOT}/Lib/mac64" NO_DEFAULT_PATH)
  set(R3DSDK_RUNTIME_DIR "${R3DSDK_ROOT}/Redistributable/mac")
else()
  find_library(R3DSDK_LIBRARY NAMES R3DSDKPIC-cpp11 PATHS "${R3DSDK_ROOT}/Lib/linux64" NO_DEFAULT_PATH)
  set(R3DSDK_RUNTIME_DIR "${R3DSDK_ROOT}/Redistributable/linux")
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(R3DSDK DEFAULT_MSG R3DSDK_LIBRARY R3DSDK_INCLUDE_DIR)
if(R3DSDK_FOUND AND NOT TARGET R3DSDK::R3DSDK)
  add_library(R3DSDK::R3DSDK UNKNOWN IMPORTED)
  set_target_properties(R3DSDK::R3DSDK PROPERTIES IMPORTED_LOCATION "${R3DSDK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${R3DSDK_INCLUDE_DIR}")
  # Windows: the SDK library is built against the dynamic CRT (/MD); the project selects the
  # matching runtime through CMAKE_MSVC_RUNTIME_LIBRARY. A /MDd build needs the MDd library.
  if(R3DSDK_LIBRARY_DEBUG)
    set_target_properties(R3DSDK::R3DSDK PROPERTIES IMPORTED_LOCATION_DEBUG "${R3DSDK_LIBRARY_DEBUG}")
  endif()
endif()

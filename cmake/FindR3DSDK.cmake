# Locates the RED R3D SDK (decode-only, used for probing / QC, never on the mosaic path).
# Download it from RED after accepting their license; set R3DSDK_ROOT to the extracted folder
# that contains Include/ and Lib/. Redistribution of the headers is prohibited by RED, so they
# are never committed here.
set(R3DSDK_ROOT "$ENV{R3DSDK_ROOT}" CACHE PATH "RED R3D SDK root")
find_path(R3DSDK_INCLUDE_DIR NAMES R3DSDK.h PATHS "${R3DSDK_ROOT}/Include" NO_DEFAULT_PATH)
if(WIN32)
  find_library(R3DSDK_LIBRARY NAMES R3DSDK-2017MT R3DSDK-2017MTd R3DSDK PATHS "${R3DSDK_ROOT}/Lib/win64" NO_DEFAULT_PATH)
else()
  find_library(R3DSDK_LIBRARY NAMES R3DSDKPIC R3DSDK PATHS "${R3DSDK_ROOT}/Lib/linux64" NO_DEFAULT_PATH)
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(R3DSDK DEFAULT_MSG R3DSDK_LIBRARY R3DSDK_INCLUDE_DIR)
if(R3DSDK_FOUND AND NOT TARGET R3DSDK::R3DSDK)
  add_library(R3DSDK::R3DSDK UNKNOWN IMPORTED)
  set_target_properties(R3DSDK::R3DSDK PROPERTIES IMPORTED_LOCATION "${R3DSDK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${R3DSDK_INCLUDE_DIR}")
endif()

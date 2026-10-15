# Locates the intoPIX FastTicoRAW SDK. The SDK is commercial and is NOT part of this
# repository. Point TICORAW_ROOT (cache or env) at the SDK install directory that
# contains include/ and lib/. Header and library names below are placeholders and
# must be adjusted to the real SDK layout once it is in hand (see docs/FEASIBILITY.md).
set(TICORAW_ROOT "$ENV{TICORAW_ROOT}" CACHE PATH "intoPIX FastTicoRAW SDK root")
find_path(TicoRAW_INCLUDE_DIR NAMES ticoraw.h PATHS "${TICORAW_ROOT}/include" NO_DEFAULT_PATH)
find_library(TicoRAW_LIBRARY NAMES ticoraw fastticoraw PATHS "${TICORAW_ROOT}/lib" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TicoRAW DEFAULT_MSG TicoRAW_LIBRARY TicoRAW_INCLUDE_DIR)
if(TicoRAW_FOUND AND NOT TARGET TicoRAW::TicoRAW)
  add_library(TicoRAW::TicoRAW UNKNOWN IMPORTED)
  set_target_properties(TicoRAW::TicoRAW PROPERTIES IMPORTED_LOCATION "${TicoRAW_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TicoRAW_INCLUDE_DIR}")
endif()

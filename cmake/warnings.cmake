# Common warning flags applied to every r3dss target through r3dss::warnings.
add_library(r3dss_warnings INTERFACE)
add_library(r3dss::warnings ALIAS r3dss_warnings)
if(MSVC)
  target_compile_options(r3dss_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  target_compile_definitions(r3dss_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(r3dss_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

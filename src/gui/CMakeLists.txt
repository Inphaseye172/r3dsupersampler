set(CMAKE_AUTOMOC ON)
# Everything but main() in a library, so tests/ can drive the batch tab and the headless runner.
add_library(r3dss_gui_lib STATIC
  app_settings.hpp
  batch_model.hpp batch_model.cpp
  engine_bridge.hpp engine_bridge.cpp
  batch_tab.hpp batch_tab.cpp
  settings_dialog.hpp settings_dialog.cpp
  main_window.hpp main_window.cpp
  headless_run.hpp headless_run.cpp
  qt_paths.hpp
  # --run-job handles Ctrl-C / SIGTERM exactly like r3dss-cli.
  ${PROJECT_SOURCE_DIR}/src/cli/interrupt.hpp ${PROJECT_SOURCE_DIR}/src/cli/interrupt.cpp
)
target_include_directories(r3dss_gui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/src/cli)
target_link_libraries(r3dss_gui_lib PUBLIC r3dss::pipeline r3dss::synth Qt6::Widgets)
qt_add_executable(r3dss-gui WIN32 main.cpp)
target_link_libraries(r3dss-gui PRIVATE r3dss_gui_lib)
# Product name not chosen yet (docs/IMPLEMENTATION_PLAN.md D7): neutral binary name.
set_target_properties(r3dss-gui PROPERTIES OUTPUT_NAME "r3dss-gui")
install(TARGETS r3dss-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
if(WIN32)
  # Deploy the Qt runtime next to the executable on install (windeployqt ships with Qt). A missing or
  # failing windeployqt is fatal: a package without Qt cannot start the GUI. The MSVC runtime is
  # installed app-local by the root CMakeLists.txt (InstallRequiredSystemLibraries), so windeployqt's
  # vc_redist installer is not shipped.
  find_program(R3DSS_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
  if(NOT R3DSS_WINDEPLOYQT)
    message(FATAL_ERROR "windeployqt not found next to Qt (${Qt6_DIR}); the GUI could not be packaged")
  endif()
  install(CODE "
    execute_process(COMMAND \"${R3DSS_WINDEPLOYQT}\" --no-translations --no-system-d3d-compiler --no-opengl-sw
                            --no-compiler-runtime \"\${CMAKE_INSTALL_PREFIX}/bin/r3dss-gui.exe\" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR \"windeployqt failed (\${_rc})\")
    endif()
  ")
endif()

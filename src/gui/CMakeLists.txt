set(CMAKE_AUTOMOC ON)
qt_add_executable(r3dss-gui WIN32
  main.cpp
  app_settings.hpp
  batch_model.hpp batch_model.cpp
  engine_bridge.hpp engine_bridge.cpp
  batch_tab.hpp batch_tab.cpp
  settings_dialog.hpp settings_dialog.cpp
  main_window.hpp main_window.cpp
)
target_link_libraries(r3dss-gui PRIVATE r3dss::pipeline r3dss::synth Qt6::Widgets)
# Product name not chosen yet (docs/IMPLEMENTATION_PLAN.md D7): neutral binary name.
set_target_properties(r3dss-gui PROPERTIES OUTPUT_NAME "r3dss-gui")
install(TARGETS r3dss-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
if(WIN32)
  # Deploy Qt runtime next to the executable on install (windeployqt ships with Qt).
  find_program(R3DSS_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
  if(R3DSS_WINDEPLOYQT)
    install(CODE "execute_process(COMMAND \"${R3DSS_WINDEPLOYQT}\" --no-translations --no-system-d3d-compiler --no-opengl-sw \"\${CMAKE_INSTALL_PREFIX}/bin/r3dss-gui.exe\")")
  endif()
endif()

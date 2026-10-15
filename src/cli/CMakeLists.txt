add_executable(r3dss-cli main.cpp)
target_link_libraries(r3dss-cli PRIVATE r3dss::pipeline r3dss::synth CLI11::CLI11 nlohmann_json::nlohmann_json)
install(TARGETS r3dss-cli RUNTIME DESTINATION bin)

add_executable(r3dss-bench bench.cpp)
target_link_libraries(r3dss-bench PRIVATE r3dss::pipeline r3dss::synth CLI11::CLI11 nlohmann_json::nlohmann_json)
install(TARGETS r3dss-bench RUNTIME DESTINATION bin)

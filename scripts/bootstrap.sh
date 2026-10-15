#!/usr/bin/env bash
# Ubuntu 24.04 dependencies for the Linux build (engine, CLI, tests, optional GUI).
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build catch2 nlohmann-json3-dev libcli11-dev qt6-base-dev libgl1-mesa-dev
echo "Now: cmake --preset linux-ci && cmake --build --preset linux-ci && ctest --preset linux-ci"

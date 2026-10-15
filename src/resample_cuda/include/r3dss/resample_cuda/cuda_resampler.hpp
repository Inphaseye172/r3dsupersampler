// CUDA implementation of IResampler: same tap tables and math as the CPU path.
#pragma once
#include <memory>
#include <string>
#include "r3dss/resample/resampler.hpp"

namespace r3dss::resample {

int cudaDeviceCount();                       // 0 when no usable device
std::string cudaDeviceName(int device = 0);
uint64_t cudaDeviceMemory(int device = 0);
// Returns nullptr when no device is available.
std::unique_ptr<IResampler> makeCudaResampler(int device = 0);

} // namespace r3dss::resample

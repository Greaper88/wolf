#pragma once
#include "device.hpp"
#include <filesystem>
namespace wolf::gpu {
struct DiscoveryPaths {
  std::filesystem::path dri = "/dev/dri";
  std::filesystem::path drm_class = "/sys/class/drm";
};
std::vector<Device> discover(const DiscoveryPaths &paths = {});
void sample_nvidia(Device &device);
} // namespace wolf::gpu

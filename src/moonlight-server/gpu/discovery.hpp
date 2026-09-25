#pragma once
#include "selector.hpp"
#include <filesystem>

namespace wolf::gpu {
struct DiscoveryPaths {
  std::filesystem::path dri = "/dev/dri";
  std::filesystem::path drm_class = "/sys/class/drm";
  std::filesystem::path amdgpu_ids = "/usr/share/libdrm/amdgpu.ids";
  std::filesystem::path pci_ids = "/usr/share/hwdata/pci.ids";
};
// Supply cached startup verification for this actual node, not merely vendor/plugin presence.
// Launch-time discovery must not run test encodes; CapabilityCache::usable is the intended callback.
// Without a probe, devices remain hardware_encoder=false and cannot be admitted.
using CapabilityProbe = std::function<bool(const Device &)>;
std::vector<Device> discover(const DiscoveryPaths &paths = {}, const CapabilityProbe &probe = {});
void sample_nvidia(Device &device);
} // namespace wolf::gpu

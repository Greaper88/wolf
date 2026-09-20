#pragma once
#include "device.hpp"
#include <array>
#include <state/serialised_config.hpp>
namespace wolf::gpu {
struct Capabilities {
  Device device;
  // H.264, HEVC and AV1, currently SDR 8-bit 4:2:0 only.
  std::array<std::string, 3> pipelines;
  std::optional<unsigned int> cuda_device;
  std::string producer_caps;
};
std::vector<Capabilities> probe_inventory(const wolf::config::GstVideoCfg &config);
} // namespace wolf::gpu

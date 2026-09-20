#pragma once
#include "device.hpp"

namespace wolf::gpu {
enum class Codec {
  h264,
  hevc,
  av1
};
struct EncoderBinding {
  std::string factory;
  std::string plugin;
  Codec codec = Codec::h264;
  std::string render_node;
  std::optional<unsigned int> cuda_device;
  std::string converter;
  std::string producer_caps;
};
struct EncoderProbeResult {
  std::optional<EncoderBinding> encoder;
  std::vector<std::string> failures;
};

} // namespace wolf::gpu

#pragma once
#include "selector.hpp"

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
  // Present only after compositor DMA-BUF -> device-bound VA conversion -> encode succeeds.
  std::optional<std::string> zero_copy_postproc = std::nullopt;
};
inline constexpr const char *zero_copy_caps = "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=XR24";
struct EncoderProbeResult {
  std::optional<EncoderBinding> encoder;
  std::vector<std::string> failures;
};

} // namespace wolf::gpu

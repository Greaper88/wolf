#pragma once
#include "encoder_types.hpp"

namespace wolf::gpu {
struct PipelineTemplate {
  std::string plugin;
  std::string pipeline;
};
// Select a converter bound to the encoder GPU and require GPU memory at both ends.
// Called inside the bounded probe child; no ordinary-memory fallback is permitted.
bool bind_zero_copy(EncoderBinding &binding);
// Reuse the configured encoder properties with the verified device and GPU-memory path.
std::string bind_pipeline(const EncoderBinding &binding,
                          const std::vector<PipelineTemplate> &templates,
                          const std::string &source,
                          const std::string &sink);
} // namespace wolf::gpu

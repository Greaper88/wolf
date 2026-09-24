#pragma once
#include "encoder_types.hpp"

namespace wolf::gpu {
struct PipelineTemplate {
  std::string plugin;
  std::string pipeline;
};
// Reuse configured encoder properties, but only a template whose leading factory belongs to
// the verified codec/implementation. Zero-copy requires a separately verified VA conversion path.
std::string bind_pipeline(const EncoderBinding &binding,
                          const std::vector<PipelineTemplate> &templates,
                          const std::string &source,
                          const std::string &sink,
                          bool zero_copy = false);
} // namespace wolf::gpu

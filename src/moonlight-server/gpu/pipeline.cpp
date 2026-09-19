#include "pipeline.hpp"
#include <stdexcept>

namespace wolf::gpu {
std::string bind_pipeline(const EncoderBinding &binding,
                          const std::vector<PipelineTemplate> &templates,
                          const std::string &source,
                          const std::string &sink) {
  if (binding.plugin != "va")
    throw std::runtime_error("Automatic session pipelines currently require a verified VA encoder");
  const std::string codec = binding.codec == Codec::h264 ? "h264" : binding.codec == Codec::hevc ? "h265" : "av1";
  const bool low_power = binding.factory.ends_with(codec + "lpenc");
  const std::string generic = "va" + codec + (low_power ? "lpenc" : "enc");
  if (!binding.factory.starts_with("va") || !binding.factory.ends_with(codec + (low_power ? "lpenc" : "enc")) ||
      binding.factory.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
          std::string::npos)
    throw std::runtime_error("Invalid verified encoder factory");
  for (const auto &entry : templates) {
    if (entry.plugin != binding.plugin)
      continue;
    auto first = entry.pipeline.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    auto end = entry.pipeline.find_first_of(" \t\r\n!", first);
    auto factory = entry.pipeline.substr(first, end - first);
    if (factory != generic && factory != binding.factory)
      continue;
    auto encoder = entry.pipeline;
    encoder.replace(first, factory.size(), binding.factory);
    return source +
           " ! videoconvertscale add-borders=true ! "
           "video/x-raw,format=NV12,width={width},height={height},pixel-aspect-ratio=1/1,"
           "chroma-site={color_range},colorimetry={color_space} ! " +
           encoder + " ! " + sink;
  }
  throw std::runtime_error("No configured pipeline template matches the verified encoder " + binding.factory);
}
} // namespace wolf::gpu

#include "pipeline.hpp"
#include <stdexcept>

namespace wolf::gpu {
std::string bind_pipeline(const EncoderBinding &binding,
                          const std::vector<PipelineTemplate> &templates,
                          const std::string &source,
                          const std::string &sink,
                          bool zero_copy) {
  if (binding.plugin == "nvcodec") {
    const std::string codec = binding.codec == Codec::h264 ? "h264" : binding.codec == Codec::hevc ? "h265" : "av1";
    if (!binding.cuda_device ||
        binding.factory.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos)
      throw std::runtime_error("NVENC requires a verified CUDA device and encoder factory");
    const auto generic = "nv" + codec + "enc";
    const auto specific = "nv" + codec + "device" + std::to_string(*binding.cuda_device) + "enc";
    if (binding.factory != (*binding.cuda_device == 0 ? generic : specific))
      throw std::runtime_error("NVENC factory does not match the selected CUDA device");
    if (zero_copy && binding.zero_copy_postproc != "cudaconvertscale")
      throw std::runtime_error("NVENC zero-copy requires startup verification of CUDA conversion");
    for (const auto &entry : templates) {
      if (entry.plugin != "nvcodec")
        continue;
      auto first = entry.pipeline.find_first_not_of(" \t\r\n");
      auto end = entry.pipeline.find_first_of(" \t\r\n!", first);
      auto factory = first == std::string::npos ? std::string{} : entry.pipeline.substr(first, end - first);
      if (factory != generic && factory != binding.factory)
        continue;
      auto encoder = entry.pipeline;
      encoder.replace(first, factory.size(), binding.factory);
      const auto selected = std::to_string(*binding.cuda_device);
      auto conversion = zero_copy ? std::string{} : "cudaupload cuda-device-id=" + selected + " ! ";
      conversion +=
          "cudaconvertscale cuda-device-id=" + selected +
          " add-borders=true ! video/x-raw(memory:CUDAMemory),format=NV12,width={width},height={height},"
          "pixel-aspect-ratio=1/1,interlace-mode=progressive,chroma-site={color_range},colorimetry={color_space}";
      return source + " ! " + conversion + " ! " + encoder + " ! " + sink;
    }
    throw std::runtime_error("No configured NVENC template matches the verified codec");
  }
  if (binding.plugin != "va")
    throw std::runtime_error("Automatic session pipelines currently require a verified VA encoder");
  std::string conversion = "videoconvertscale add-borders=true ! video/x-raw";
  if (zero_copy) {
    const auto &postproc = binding.zero_copy_postproc;
    if (!postproc || !postproc->starts_with("va") || !postproc->ends_with("postproc") ||
        postproc->find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos)
      throw std::runtime_error("Zero-copy requires a verified device-bound VA converter");
    conversion = std::string(zero_copy_caps) + " ! " + *postproc + " add-borders=true ! video/x-raw(memory:VAMemory)";
  }
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
    // Saved configurations may contain a VA factory resolved for another GPU.
    // Rebind only the same codec and normal/low-power family; retain its properties.
    const std::string prefix = "varenderD";
    const std::string suffix = codec + (low_power ? "lpenc" : "enc");
    bool same_family = false;
    if (factory.starts_with(prefix) && factory.ends_with(suffix) && factory.size() > prefix.size() + suffix.size()) {
      const auto node = factory.substr(prefix.size(), factory.size() - prefix.size() - suffix.size());
      same_family = node.find_first_not_of("0123456789") == std::string::npos;
    }
    if (factory != generic && factory != binding.factory && !same_family)
      continue;
    auto encoder = entry.pipeline;
    encoder.replace(first, factory.size(), binding.factory);
    return source + " ! " + conversion +
           ",format=NV12,width={width},height={height},pixel-aspect-ratio=1/1,"
           "chroma-site={color_range},colorimetry={color_space} ! " +
           encoder + " ! " + sink;
  }
  throw std::runtime_error("No configured pipeline template matches the verified encoder " + binding.factory);
}
} // namespace wolf::gpu

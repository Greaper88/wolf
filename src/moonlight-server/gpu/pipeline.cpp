#include "pipeline.hpp"
#include "encoder_probe.hpp"
#include <core/gstreamer.hpp>
#include <gst-video-context.hpp>
#include <stdexcept>

namespace wolf::gpu {
bool bind_zero_copy(EncoderBinding &binding) {
  if (binding.plugin == "nvcodec") {
    if (!binding.cuda_device || !gst_video_context::supports_cuda() || !gst_video_context::init())
      return false;
    auto converter = gst_element_factory_make("cudaconvertscale", nullptr);
    if (!converter)
      return false;
    gst_object_unref(converter);
    binding.converter = "cudaconvertscale";
    binding.producer_caps = "video/x-raw(memory:CUDAMemory),pixel-aspect-ratio=1/1";
    return true;
  }
  if (binding.plugin != "va")
    return false;
  auto features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  for (auto entry = features; entry; entry = entry->next) {
    auto feature = GST_PLUGIN_FEATURE(entry->data);
    const char *plugin = gst_plugin_feature_get_plugin_name(feature);
    std::string name = gst_plugin_feature_get_name(feature);
    if (!plugin || std::string(plugin) != "va" || !name.ends_with("postproc"))
      continue;
    auto converter = gst_element_factory_make(name.c_str(), nullptr);
    if (!converter)
      continue;
    auto property = g_object_class_find_property(G_OBJECT_GET_CLASS(converter), "device-path");
    gchar *node = nullptr;
    if (property && G_PARAM_SPEC_VALUE_TYPE(property) == G_TYPE_STRING && (property->flags & G_PARAM_READABLE) &&
        !(property->flags & G_PARAM_WRITABLE))
      g_object_get(converter, "device-path", &node, nullptr);
    bool matches = node && same_render_device(node, binding.render_node);
    g_free(node);
    gst_object_unref(converter);
    if (!matches)
      continue;
    std::string formats;
    for (const auto &format : wolf::core::gstreamer::get_dma_caps(name)) {
      if (format.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:_") !=
              std::string::npos ||
          format.find("P010") != std::string::npos || format.find("AR30") != std::string::npos)
        continue;
      if (!formats.empty())
        formats += ",";
      formats += format;
    }
    if (formats.empty())
      continue;
    binding.converter = name;
    binding.producer_caps = "video/x-raw(memory:DMABuf),format=DMA_DRM,pixel-aspect-ratio=1/1,drm-format={" + formats +
                            "}";
    break;
  }
  gst_plugin_feature_list_free(features);
  return !binding.producer_caps.empty();
}

std::string bind_pipeline(const EncoderBinding &binding,
                          const std::vector<PipelineTemplate> &templates,
                          const std::string &source,
                          const std::string &sink) {
  if (binding.plugin != "va" && binding.plugin != "nvcodec")
    throw std::runtime_error("Manual session pipelines require a verified VA or NVIDIA encoder");
  const std::string codec = binding.codec == Codec::h264 ? "h264" : binding.codec == Codec::hevc ? "h265" : "av1";
  const bool low_power = binding.factory.ends_with(codec + "lpenc");
  const std::string generic = binding.plugin == "va" ? "va" + codec + (low_power ? "lpenc" : "enc")
                                                     : "nv" + codec + "enc";
  bool matches = binding.plugin == "va"
                     ? binding.factory.starts_with("va") &&
                           binding.factory.ends_with(codec + (low_power ? "lpenc" : "enc"))
                     : binding.cuda_device.has_value() && binding.factory.starts_with("nv" + codec) &&
                           binding.factory.ends_with("enc");
  if (!matches || binding.factory.find_first_not_of(
                      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
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
    if (binding.converter.empty() || binding.producer_caps.empty())
      throw std::runtime_error("No verified GPU-memory conversion path for the selected device");
    const auto memory = binding.plugin == "va" ? "VAMemory" : "CUDAMemory";
    const auto input = binding.plugin == "va" ? "DMABuf" : "CUDAMemory";
    return source + " ! video/x-raw(memory:" + input + ") ! " + binding.converter +
           " add-borders=true ! video/x-raw(memory:" + memory +
           "),format=NV12,width={width},height={height},pixel-aspect-ratio=1/1 ! " + encoder + " ! " + sink;
  }
  throw std::runtime_error("No configured pipeline template matches the verified encoder " + binding.factory);
}
} // namespace wolf::gpu

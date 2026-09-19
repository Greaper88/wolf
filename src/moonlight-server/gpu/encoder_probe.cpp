#include "encoder_probe.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <regex>
#ifdef __linux__
#include <dlfcn.h>
#endif

namespace wolf::gpu {
namespace {
const char *media_type(Codec codec) {
  switch (codec) {
  case Codec::h264:
    return "video/x-h264";
  case Codec::hevc:
    return "video/x-h265";
  case Codec::av1:
    return "video/x-av1";
  }
  return "";
}
bool fixed_property(GstElement *element, const char *name, GType type) {
  auto property = g_object_class_find_property(G_OBJECT_GET_CLASS(element), name);
  return property && G_PARAM_SPEC_VALUE_TYPE(property) == type && (property->flags & G_PARAM_READABLE) &&
         !(property->flags & G_PARAM_WRITABLE);
}
bool outputs_codec(GstElementFactory *factory, Codec codec) {
  auto caps = gst_caps_new_empty_simple(media_type(codec));
  bool matches = gst_element_factory_can_src_any_caps(factory, caps);
  gst_caps_unref(caps);
  return matches;
}
void count_buffer(GstElement *, GstBuffer *buffer, GstPad *, gpointer data) {
  if (gst_buffer_get_size(buffer) > 0)
    ++*static_cast<std::atomic<unsigned int> *>(data);
}
// Takes ownership of the encoder. No shared contexts or environment changes.
bool trial_encode(GstElement *encoder, Codec codec, std::chrono::milliseconds timeout) {
  auto pipeline = gst_pipeline_new(nullptr);
  auto source = gst_element_factory_make("videotestsrc", nullptr);
  auto filter = gst_element_factory_make("capsfilter", nullptr);
  auto sink = gst_element_factory_make("fakesink", nullptr);
  auto encoded_filter = gst_element_factory_make("capsfilter", nullptr);
  if (!pipeline || !source || !filter || !sink || !encoded_filter) {
    for (auto element : {pipeline, source, filter, sink, encoded_filter, encoder})
      if (element)
        gst_object_unref(element);
    return false;
  }
  std::atomic<unsigned int> buffers{0};
  auto caps = gst_caps_from_string("video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  g_object_set(source, "num-buffers", 8, nullptr);
  g_object_set(filter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  auto encoded_caps = gst_caps_new_empty_simple(media_type(codec));
  g_object_set(encoded_filter, "caps", encoded_caps, nullptr);
  gst_caps_unref(encoded_caps);
  g_object_set(sink, "sync", FALSE, "signal-handoffs", TRUE, nullptr);
  auto handler = g_signal_connect(sink, "handoff", G_CALLBACK(count_buffer), &buffers);
  gst_bin_add_many(GST_BIN(pipeline), source, filter, encoder, encoded_filter, sink, nullptr);
  bool success = false;
  if (gst_element_link_many(source, filter, encoder, encoded_filter, sink, nullptr) &&
      gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
    auto bus = gst_element_get_bus(pipeline);
    auto message = gst_bus_timed_pop_filtered(bus,
                                              static_cast<GstClockTime>(timeout.count()) * GST_MSECOND,
                                              static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    success = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && buffers.load() > 0;
    if (message)
      gst_message_unref(message);
    gst_object_unref(bus);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  g_signal_handler_disconnect(sink, handler);
  gst_object_unref(pipeline);
  return success;
}
} // namespace

bool same_render_device(const std::string &left, const std::string &right) {
  if (left.empty() || right.empty())
    return false;
  std::error_code error;
  auto first = std::filesystem::canonical(left, error);
  if (error)
    return false;
  auto second = std::filesystem::canonical(right, error);
  return !error && first == second;
}

std::optional<unsigned int> cuda_device_for_pci(const std::string &pci_id) {
#ifdef __linux__
  // The selected identity comes from DRM discovery, not a guessed CUDA ordinal.
  static const std::regex bdf("[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]");
  if (!std::regex_match(pci_id, bdf))
    return std::nullopt;
  auto close_library = [](void *handle) { dlclose(handle); };
  std::unique_ptr<void, decltype(close_library)> library(dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL), close_library);
  if (!library)
    return std::nullopt;
  auto init = reinterpret_cast<int (*)(unsigned int)>(dlsym(library.get(), "cuInit"));
  auto get = reinterpret_cast<int (*)(int *, const char *)>(dlsym(library.get(), "cuDeviceGetByPCIBusId"));
  auto pci = reinterpret_cast<int (*)(char *, int, int)>(dlsym(library.get(), "cuDeviceGetPCIBusId"));
  int device = -1;
  char actual[32]{};
  if (!init || !get || !pci || init(0) != 0 || get(&device, pci_id.c_str()) != 0 || device < 0 ||
      pci(actual, sizeof(actual), device) != 0 || normalize_identifier(actual) != normalize_identifier(pci_id))
    return std::nullopt;
  return static_cast<unsigned int>(device);
#else
  return std::nullopt;
#endif
}

std::optional<EncoderBinding> encoder_binding(GstElement *element, const Device &device, Codec codec) {
  if (!element)
    return std::nullopt;
  auto factory = gst_element_get_factory(element);
  if (!factory)
    return std::nullopt;
  const char *plugin_name = gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory));
  const char *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
  if (!plugin_name || !klass || std::string(klass).find("Encoder/Video/Hardware") == std::string::npos ||
      !outputs_codec(factory, codec))
    return std::nullopt;
  EncoderBinding result{.factory = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)),
                        .plugin = plugin_name,
                        .codec = codec,
                        .render_node = device.render_node,
                        .cuda_device = std::nullopt};
  if (result.plugin == "va" && fixed_property(element, "device-path", G_TYPE_STRING)) {
    gchar *node = nullptr;
    g_object_get(element, "device-path", &node, nullptr);
    bool matches = node && same_render_device(node, device.render_node);
    g_free(node);
    if (matches)
      return result;
  } else if (result.plugin == "nvcodec" && device.driver == "nvidia" &&
             fixed_property(element, "cuda-device-id", G_TYPE_UINT)) {
    auto selected = cuda_device_for_pci(device.id);
    guint actual = 0;
    g_object_get(element, "cuda-device-id", &actual, nullptr);
    if (selected && actual == *selected) {
      result.cuda_device = actual;
      return result;
    }
  }
  // QSV, software, writable/unknown binding and other-device factories cannot authorize auto selection.
  return std::nullopt;
}

EncoderProbeResult probe_encoder(const Device &device, Codec codec, std::chrono::milliseconds timeout) {
  EncoderProbeResult result;
  if (!device.accessible || !same_render_device(device.render_node, device.render_node)) {
    result.failures.emplace_back("Render device is inaccessible");
    return result;
  }
  if (timeout.count() <= 0 || timeout > std::chrono::seconds(30)) {
    result.failures.emplace_back("Probe timeout must be between 1 and 30000 milliseconds");
    return result;
  }
  auto features = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_VIDEO_ENCODER, GST_RANK_NONE);
  std::vector<std::string> factories;
  for (auto entry = features; entry; entry = entry->next) {
    auto factory = GST_ELEMENT_FACTORY(entry->data);
    const char *plugin = gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory));
    if (plugin && (std::string(plugin) == "va" || std::string(plugin) == "nvcodec") && outputs_codec(factory, codec))
      factories.emplace_back(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)));
  }
  gst_plugin_feature_list_free(features);
  std::sort(factories.begin(), factories.end());
  for (const auto &name : factories) {
    auto element = gst_element_factory_make(name.c_str(), nullptr);
    auto binding = encoder_binding(element, device, codec);
    if (!binding) {
      if (element)
        gst_object_unref(element);
      continue;
    }
    if (trial_encode(element, codec, timeout)) {
      result.encoder = std::move(binding);
      return result;
    }
    result.failures.emplace_back(name + ": hardware test encode failed or timed out");
  }
  if (result.failures.empty())
    result.failures.emplace_back("No hardware encoder bound to this GPU for the requested codec");
  return result;
}
} // namespace wolf::gpu

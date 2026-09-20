#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <gpu/discovery.hpp>
#include <gpu/encoder_probe.hpp>
#include <gpu/pipeline.hpp>
#include <gst/gst.h>
#include <iostream>
#include <thread>
struct Check {
  const char *label;
  const char *feature;
  std::atomic<unsigned> buffers{0};
  std::atomic<bool> valid{true};
};
static GstPadProbeReturn inspect(GstPad *pad, GstPadProbeInfo *info, gpointer opaque) {
  auto &check = *static_cast<Check *>(opaque);
  auto buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  auto caps = gst_pad_get_current_caps(pad);
  bool valid = caps && gst_caps_get_size(caps) > 0 &&
               gst_caps_features_contains(gst_caps_get_features(caps, 0), check.feature) &&
               gst_buffer_n_memory(buffer) > 0;
  valid = valid && gst_memory_is_type(gst_buffer_peek_memory(buffer, 0),
                                      std::string(check.feature) == "memory:DMABuf" ? "dmabuf" : "VAMemory");
  check.valid.store(check.valid.load() && valid);
  if (check.buffers.fetch_add(1) == 0) {
    char *text = caps ? gst_caps_to_string(caps) : nullptr;
    std::cout << check.label << ": " << (text ? text : "missing caps") << " allocator="
              << (gst_buffer_n_memory(buffer) ? gst_buffer_peek_memory(buffer, 0)->allocator->mem_type : "none")
              << std::endl;
    g_free(text);
  }
  if (caps)
    gst_caps_unref(caps);
  return GST_PAD_PROBE_OK;
}
int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  if (argc != 2)
    return 2;
  std::string node = argv[1];
  auto devices = wolf::gpu::discover();
  auto device = std::find_if(devices.begin(), devices.end(), [&](const auto &gpu) {
    return gpu.render_node == "/dev/dri/" + node;
  });
  if (device == devices.end())
    return 2;
  auto capability = wolf::gpu::probe_encoder(*device);
  if (!capability.encoder || !wolf::gpu::bind_zero_copy(*capability.encoder)) {
    std::cerr << "No verified GPU-memory path for " << node << std::endl;
    return 2;
  }
  auto binding = *capability.encoder;
  // This smoke test exercises the VA production path. NVIDIA also needs shared CUDA-context wiring.
  if (binding.plugin != "va")
    return 2;
  auto producer_text =
      "waylanddisplaysrc name=producer render_node=/dev/dri/" + node + " ! " + binding.producer_caps +
      ",width=1280,height=720,framerate=60/1 ! interpipesink name=manual_gpu_test sync=true async=false max-buffers=1";
  auto consumer_template = wolf::gpu::bind_pipeline(
      binding,
      {{"va", binding.factory + " name=encoder bitrate=5000 key-int-max=60"}},
      "interpipesrc listen-to=manual_gpu_test is-live=true stream-sync=restart-ts",
      "fakesink name=encoded sync=false signal-handoffs=true");
  auto consumer_text = fmt::format(fmt::runtime(consumer_template), fmt::arg("width", 1280), fmt::arg("height", 720));
  GError *error = nullptr;
  auto producer = gst_parse_launch(producer_text.c_str(), &error);
  if (error) {
    std::cerr << error->message << std::endl;
    return 3;
  }
  auto consumer = gst_parse_launch(consumer_text.c_str(), &error);
  if (error) {
    std::cerr << error->message << std::endl;
    return 3;
  }
  Check source{"producer", "memory:DMABuf"}, input{"encoder input", "memory:VAMemory"};
  auto attach = [](GstElement *pipeline, const char *element_name, const char *pad_name, Check *check) {
    auto element = gst_bin_get_by_name(GST_BIN(pipeline), element_name);
    auto pad = gst_element_get_static_pad(element, pad_name);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, inspect, check, nullptr);
    gst_object_unref(pad);
    gst_object_unref(element);
  };
  attach(producer, "producer", "src", &source);
  attach(consumer, "encoder", "sink", &input);
  std::atomic<unsigned> encoded{0};
  auto sink = gst_bin_get_by_name(GST_BIN(consumer), "encoded");
  g_signal_connect(
      sink,
      "handoff",
      G_CALLBACK(+[](GstElement *, GstBuffer *, GstPad *, gpointer p) { ++*static_cast<std::atomic<unsigned> *>(p); }),
      &encoded);
  gst_object_unref(sink);
  bool ok = gst_element_set_state(producer, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE &&
            gst_element_set_state(consumer, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (ok && encoded < 60 && std::chrono::steady_clock::now() < deadline) {
    for (auto pipeline : {producer, consumer}) {
      auto bus = gst_element_get_bus(pipeline);
      auto message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      if (message) {
        GError *err = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &err, &debug);
        std::cerr << err->message << " " << (debug ? debug : "") << std::endl;
        g_error_free(err);
        g_free(debug);
        gst_message_unref(message);
        ok = false;
      }
      gst_object_unref(bus);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  gst_element_set_state(consumer, GST_STATE_NULL);
  gst_element_set_state(producer, GST_STATE_NULL);
  gst_object_unref(consumer);
  gst_object_unref(producer);
  ok = ok && encoded >= 60 && source.buffers > 0 && input.buffers > 0 && source.valid && input.valid;
  std::cout << node << " encoded=" << encoded << " producer_buffers=" << source.buffers
            << " encoder_buffers=" << input.buffers << " result=" << (ok ? "PASS" : "FAIL") << std::endl;
  return ok ? 0 : 1;
}

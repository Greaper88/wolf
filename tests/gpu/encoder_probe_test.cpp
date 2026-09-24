#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gpu/encoder_probe.hpp>
#include <iostream>
#include <unistd.h>

using namespace wolf::gpu;
int checks = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
  ++checks;
}

// A test-only VA factory supplies real GObject metadata/properties, but no working encoder.
// It must match a device binding and then fail the actual encode; factory presence is not capability.
struct TestEncoder {
  GstElement parent;
  GstPad *src;
  GstPad *sink;
};
static int encode_mode = 0;
struct TestEncoderClass {
  GstElementClass parent;
};
G_DEFINE_TYPE(TestEncoder, test_encoder, GST_TYPE_ELEMENT)
static std::string fixture_node;
static guint fixture_cuda = 7;
static void get_property(GObject *, guint id, GValue *value, GParamSpec *) {
  if (id == 1)
    g_value_set_string(value, fixture_node.c_str());
  if (id == 2)
    g_value_set_uint(value, fixture_cuda);
}
static void test_encoder_class_init(TestEncoderClass *klass) {
  auto object = G_OBJECT_CLASS(klass);
  object->get_property = get_property;
  g_object_class_install_property(
      object,
      1,
      g_param_spec_string("device-path", "Device", "Test device", nullptr, G_PARAM_READABLE));
  g_object_class_install_property(
      object,
      2,
      g_param_spec_uint("cuda-device-id", "CUDA device", "Test ordinal", 0, G_MAXUINT, 0, G_PARAM_READABLE));
  auto element = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(element,
                                        "Mock hardware encoder",
                                        "Codec/Encoder/Video/Hardware",
                                        "Fixture",
                                        "Wolf tests");
  auto caps = gst_caps_from_string("video/x-h264");
  gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
  caps = gst_caps_from_string("video/x-raw");
  gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
}
static GstFlowReturn chain(GstPad *, GstObject *parent, GstBuffer *buffer) {
  auto self = reinterpret_cast<TestEncoder *>(parent);
  gst_buffer_unref(buffer);
  if (encode_mode == 0)
    return GST_FLOW_ERROR;
  if (encode_mode == 2)
    return GST_FLOW_OK;
  return gst_pad_push(self->src, gst_buffer_new_allocate(nullptr, 4, nullptr));
}
static gboolean event(GstPad *, GstObject *parent, GstEvent *evt) {
  auto self = reinterpret_cast<TestEncoder *>(parent);
  if (GST_EVENT_TYPE(evt) == GST_EVENT_CAPS) {
    gst_event_unref(evt);
    auto caps = gst_caps_new_empty_simple("video/x-h264");
    auto result = gst_pad_push_event(self->src, gst_event_new_caps(caps));
    gst_caps_unref(caps);
    return result;
  }
  if (GST_EVENT_TYPE(evt) == GST_EVENT_EOS && encode_mode == 3) {
    gst_event_unref(evt);
    return TRUE;
  }
  return gst_pad_push_event(self->src, evt);
}
static void test_encoder_init(TestEncoder *self) {
  auto klass = GST_ELEMENT_GET_CLASS(self);
  self->src = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "src"), "src");
  self->sink = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "sink"), "sink");
  gst_pad_set_chain_function(self->sink, chain);
  gst_pad_set_event_function(self->sink, event);
  gst_element_add_pad(GST_ELEMENT(self), self->src);
  gst_element_add_pad(GST_ELEMENT(self), self->sink);
}
static gboolean plugin_init(GstPlugin *plugin) {
  return gst_element_register(plugin, "wolf-test-va-encoder", GST_RANK_NONE, test_encoder_get_type());
}

struct TestNvEncoder {
  TestEncoder parent;
};
struct TestNvEncoderClass {
  TestEncoderClass parent;
};
G_DEFINE_TYPE(TestNvEncoder, test_nv_encoder, test_encoder_get_type())
static void test_nv_encoder_class_init(TestNvEncoderClass *) {}
static void test_nv_encoder_init(TestNvEncoder *) {}
static gboolean nv_plugin_init(GstPlugin *plugin) {
  return gst_element_register(plugin, "wolf-test-nv-encoder", GST_RANK_NONE, test_nv_encoder_get_type());
}
int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  for (const auto &filename : {"libgstcoreelements.so", "libgstvideotestsrc.so"}) {
    auto plugin = gst_plugin_load_file((std::string(GPU_TEST_PLUGIN_DIR) + "/" + filename).c_str(), nullptr);
    check(plugin != nullptr, "load controlled test pipeline dependencies");
    gst_object_unref(plugin);
  }
  auto dir = std::filesystem::temp_directory_path() / ("wolf-encoder-test-" + std::to_string(getpid()));
  std::filesystem::create_directory(dir);
  auto node = dir / "renderD129";
  std::ofstream(node).put('x');
  std::filesystem::create_symlink(node, dir / "pci-render");
  check(same_render_device(node, (dir / "pci-render").string()), "by-path resolves to same device");
  check(!same_render_device(node, (dir / "missing").string()), "missing device is not equivalent");
  check(!same_render_device("", ""), "empty identities never match");
  check(!same_render_device("/missing", "/missing"), "identical missing paths never match");
  check(cuda_device_for_pci("0000:02:00.0") == 7, "CUDA ordinal comes from driver, not PCI ordering");
  check(cuda_device_for_pci("0000:03:00.0") == 0, "zero is allowed only when explicitly resolved");
  check(!cuda_device_for_pci("0000:04:00.0"), "invisible device cannot fall back to zero");
  check(!cuda_device_for_pci("renderD128"), "non-PCI identity cannot select CUDA device");
  setenv("WOLF_TEST_CUDA_INIT_FAIL", "1", 1);
  check(!cuda_device_for_pci("0000:02:00.0"), "CUDA initialization failure rejects");
  unsetenv("WOLF_TEST_CUDA_INIT_FAIL");
  setenv("WOLF_TEST_CUDA_MISMATCH", "1", 1);
  check(!cuda_device_for_pci("0000:02:00.0"), "CUDA PCI round-trip mismatch rejects");
  unsetenv("WOLF_TEST_CUDA_MISMATCH");
  check(gst_plugin_register_static(GST_VERSION_MAJOR,
                                   GST_VERSION_MINOR,
                                   "va",
                                   "Test-only VA plugin",
                                   plugin_init,
                                   "1.0",
                                   "LGPL",
                                   "wolf-tests",
                                   "wolf-tests",
                                   "https://example.invalid"),
        "register test VA plugin");
  check(gst_plugin_register_static(GST_VERSION_MAJOR,
                                   GST_VERSION_MINOR,
                                   "nvcodec",
                                   "Test-only NV plugin",
                                   nv_plugin_init,
                                   "1.0",
                                   "LGPL",
                                   "wolf-tests",
                                   "wolf-tests",
                                   "https://example.invalid"),
        "register test NV plugin");
  fixture_node = node;
  Device d;
  d.id = "0000:02:00.0";
  d.render_node = (dir / "pci-render").string();
  d.driver = "amdgpu";
  d.accessible = true;
  auto encoder = gst_element_factory_make("wolf-test-va-encoder", nullptr);
  auto binding = encoder_binding(encoder, d, Codec::h264);
  check(binding && binding->factory == "wolf-test-va-encoder", "VA read-only device binding matches canonical alias");
  check(!encoder_binding(encoder, d, Codec::hevc), "H264 factory cannot promise HEVC");
  d.render_node = (dir / "missing").string();
  check(!encoder_binding(encoder, d, Codec::h264), "VA encoder for other node rejected");
  d.render_node = node;
  gst_object_unref(encoder);
  encoder = gst_element_factory_make("wolf-test-nv-encoder", nullptr);
  check(!encoder_binding(encoder, d, Codec::h264), "NV encoder cannot bind AMD GPU");
  d.driver = "nvidia";
  binding = encoder_binding(encoder, d, Codec::h264);
  check(binding && binding->cuda_device == 7, "NV factory matches driver-mapped CUDA ordinal");
  fixture_cuda = 0;
  check(!encoder_binding(encoder, d, Codec::h264), "default NV factory cannot bind another CUDA device");
  fixture_cuda = 7;
  d.driver = "amdgpu";
  gst_object_unref(encoder);
  auto software = gst_element_factory_make("identity", nullptr);
  check(!encoder_binding(software, d, Codec::h264), "software element cannot qualify as hardware encoder");
  gst_object_unref(software);
  check(!encoder_binding(nullptr, d, Codec::h264), "missing factory rejected");
  check(!probe_encoder(d).encoder, "instantiable matching factory must actually encode frames");
  encode_mode = 1;
  auto working = probe_encoder(d);
  check(working.encoder.has_value(), "completed fixture encode with output is accepted");
  check(!probe_zero_copy(d, *working.encoder), "working encoder alone cannot authorize zero-copy");
  encode_mode = 2;
  check(!probe_encoder(d).encoder, "EOS with no encoded output is rejected");
  encode_mode = 3;
  auto started = std::chrono::steady_clock::now();
  check(!probe_encoder(d, Codec::h264, std::chrono::milliseconds(20)).encoder, "stalled stream times out");
  check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "bus timeout returns promptly");
  encode_mode = 1;
  check(probe_encoder(d).encoder.has_value(), "probe works again after error and timeout cleanup");
  check(!probe_encoder(d, Codec::h264, std::chrono::milliseconds(0)).encoder, "zero probe timeout rejected");
  d.accessible = false;
  check(!probe_encoder(d).encoder, "inaccessible render node rejected before probe");
  std::filesystem::remove_all(dir);
  std::cout << checks << " encoder checks passed\n";
}

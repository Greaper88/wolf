#include "discovery.hpp"
#include "encoder_probe.hpp"
#include "probe_process.hpp"
#include <filesystem>
#ifdef __linux__
#include <unistd.h>
#endif
namespace wolf::gpu {
int probe_child(int argc, char **argv) {
#ifdef __linux__
  if (argc != 6 && argc != 7)
    return 2;
  std::string codec_arg = argv[5];
  if (codec_arg != "0" && codec_arg != "1" && codec_arg != "2")
    return 2;
  gst_init(nullptr, nullptr);
  for (const auto &device : discover()) {
    if (device.id != argv[3] || device.driver != argv[4] || !same_render_device(device.render_node, argv[2]))
      continue;
    EncoderProbeResult result;
    const auto codec = static_cast<Codec>(codec_arg[0] - '0');
    if (argc == 7) {
      auto element = gst_element_factory_make(argv[6], nullptr);
      result.encoder = encoder_binding(element, device, codec);
      if (element)
        gst_object_unref(element);
      if (!result.encoder)
        return 2;
      // The test compositor must not share the live server's Wayland sockets.
      char directory[] = "/tmp/wolf-gpu-probe-XXXXXX";
      if (!mkdtemp(directory))
        return 2;
      setenv("XDG_RUNTIME_DIR", directory, 1);
      result.encoder->zero_copy_postproc = probe_zero_copy(device, *result.encoder);
      std::error_code error;
      std::filesystem::remove_all(directory, error);
      if (!result.encoder->zero_copy_postproc)
        return 2;
    } else {
      result = probe_encoder(device, codec);
    }
    if (!result.encoder)
      return 2;
    const auto &encoder = *result.encoder;
    auto reply = "WOLF_GPU_V2 " + encoder.factory + " " + encoder.plugin + " " +
                 (encoder.cuda_device ? std::to_string(*encoder.cuda_device) : "-1") + " " +
                 encoder.zero_copy_postproc.value_or("-") + "\n";
    return write(3, reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()) ? 0 : 2;
  }
#endif
  return 2;
}
} // namespace wolf::gpu

#include "discovery.hpp"
#include "encoder_probe.hpp"
#include "probe_process.hpp"
#ifdef __linux__
#include <unistd.h>
#endif
namespace wolf::gpu {
int probe_child(int argc, char **argv) {
#ifdef __linux__
  if (argc != 6)
    return 2;
  std::string codec_arg = argv[5];
  if (codec_arg != "0" && codec_arg != "1" && codec_arg != "2")
    return 2;
  gst_init(nullptr, nullptr);
  for (const auto &device : discover()) {
    if (device.id != argv[3] || device.driver != argv[4] || !same_render_device(device.render_node, argv[2]))
      continue;
    auto result = probe_encoder(device, static_cast<Codec>(codec_arg[0] - '0'));
    if (!result.encoder)
      return 2;
    const auto &encoder = *result.encoder;
    auto reply = "WOLF_GPU_V1 " + encoder.factory + " " + encoder.plugin + " " +
                 (encoder.cuda_device ? std::to_string(*encoder.cuda_device) : "-1") + "\n";
    return write(3, reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()) ? 0 : 2;
  }
#endif
  return 2;
}
} // namespace wolf::gpu

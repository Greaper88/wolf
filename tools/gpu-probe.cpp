#include <gpu/discovery.hpp>
#include <gpu/encoder_probe.hpp>
#include <gpu/probe_process.hpp>
#include <iostream>

// Standalone diagnostic; no sessions, containers or process-wide GPU environment changes.
int main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--wolf-gpu-probe")
    return wolf::gpu::probe_child(argc, argv);
  gst_init(&argc, &argv);
  if (argc != 2) {
    std::cerr << "Usage: wolf-gpu-probe /dev/dri/renderD129 (or a by-path alias)\n";
    return 1;
  }
  for (const auto &device : wolf::gpu::discover()) {
    if (!wolf::gpu::same_render_device(argv[1], device.render_node))
      continue;
    std::cout << device.id << " (" << device.driver << ")\n";
    bool h264 = false;
    for (auto codec : {wolf::gpu::Codec::h264, wolf::gpu::Codec::hevc, wolf::gpu::Codec::av1}) {
      const char *name = codec == wolf::gpu::Codec::h264 ? "H.264" : codec == wolf::gpu::Codec::hevc ? "HEVC" : "AV1";
      auto result = wolf::gpu::isolated_probe("/proc/self/exe", device, codec);
      std::cout << name << ": ";
      if (result.encoder) {
        std::cout << result.encoder->factory << " verified\n";
        if (codec == wolf::gpu::Codec::h264)
          h264 = true;
      } else {
        std::cout << "unavailable\n";
        for (const auto &reason : result.failures)
          std::cout << "  " << reason << '\n';
      }
    }
    return h264 ? 0 : 2;
  }
  std::cerr << "Render device not found; check /dev/dri and readable /sys/class/drm mounts\n";
  return 2;
}

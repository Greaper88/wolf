#include "inventory.hpp"
#include "discovery.hpp"
#include "pipeline.hpp"
#include "probe_process.hpp"
#include <helpers/logger.hpp>
namespace wolf::gpu {
std::vector<Capabilities> probe_inventory(const wolf::config::GstVideoCfg &config) {
  std::vector<Capabilities> result;
  const std::array encoders{config.h264_encoders, config.hevc_encoders, config.av1_encoders};
  for (const auto &device : discover()) {
    Capabilities gpu{.device = device};
    for (int codec = 0; device.accessible && codec < 3; ++codec) {
      auto probe = isolated_probe("/proc/self/exe", device, static_cast<Codec>(codec), {});
      if (!probe.encoder)
        continue;
      std::vector<PipelineTemplate> templates;
      for (const auto &encoder : encoders[codec])
        templates.push_back({encoder.plugin_name, encoder.encoder_pipeline});
      try {
        gpu.pipelines[codec] = bind_pipeline(*probe.encoder, templates, config.default_source, config.default_sink);
        gpu.cuda_device = probe.encoder->cuda_device;
        gpu.producer_caps = probe.encoder->producer_caps;
      } catch (const std::exception &error) {
        logs::log(logs::warning, "[GPU] {}: {}", device.render_node, error.what());
      }
    }
    result.push_back(std::move(gpu));
  }
  return result;
}
} // namespace wolf::gpu

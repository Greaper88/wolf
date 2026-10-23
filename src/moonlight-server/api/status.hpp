#pragma once

#include <gpu/cpu_topology.hpp>
#include <state/data-structures.hpp>

namespace wolf::api {

// Monitoring DTOs intentionally omit credentials, addresses, environment and runner configuration.
struct StatusGpu {
  std::string id;
  std::string name;
  std::string render_node;
  bool accessible;
  std::optional<std::uint64_t> vram_bytes;
  std::optional<double> vram_percent;
  std::optional<double> gpu_percent;
  std::optional<double> encoder_percent;
  std::string capability_status = "unknown";
  std::vector<std::string> codecs;
  std::vector<int> cpu_ids;
  std::optional<int> numa_node;
  std::vector<unsigned> local_cpus;
};
// Eligible OS CPU IDs captured from an applied worker affinity or accepted container cpuset.
// A missing placement is unknown/unrestricted, rather than an inference from the selected GPU.
struct StatusPlacement {
  std::vector<unsigned> cpus;
  std::vector<int> cpu_ids;
  std::optional<int> numa_node;
};
struct StatusSession {
  std::string id;
  std::optional<std::string> profile_id;
  std::string app;
  std::optional<bool> connected;
  bool streaming = false;
  std::optional<std::string> gpu_id;
  std::optional<std::string> codec;
  std::optional<long> bitrate_kbps;
  std::optional<int> width;
  std::optional<int> height;
  std::optional<int> fps;
  std::optional<StatusPlacement> placement;
};
struct StatusContainer {
  std::string id;
  std::string name;
  std::string image;
  std::string app;
  std::optional<std::string> profile_id;
  std::optional<std::string> session_id;
  std::optional<std::string> gpu_id;
  std::string render_node;
  std::optional<StatusPlacement> placement;
};
struct StatusProfile {
  std::string id;
  std::string name;
};
struct StatusResponse {
  bool success = true;
  int version = 1;
  std::vector<StatusGpu> gpus;
  std::vector<StatusSession> sessions;
  std::vector<StatusContainer> containers;
  std::vector<StatusProfile> profiles;
  std::vector<wolf::numa::CpuSocket> cpus;
};

// Static topology is independent of NUMA mode and does not sample CPU load.
struct StatusTopology {
  wolf::numa::Topology host;
  std::map<std::string, wolf::numa::GpuLocality> gpus;
};
StatusTopology discover_status_topology(const std::vector<wolf::gpu::Device> &devices,
                                        const wolf::numa::TopologyPaths &paths = {});
StatusResponse make_status(const state::AppState &state, const std::vector<wolf::gpu::Device> &devices);
StatusResponse make_status(const state::AppState &state,
                           const std::vector<wolf::gpu::Device> &devices,
                           const StatusTopology &topology);

} // namespace wolf::api

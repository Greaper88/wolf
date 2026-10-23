#include <api/status.hpp>

namespace wolf::api {

StatusTopology discover_status_topology(const std::vector<wolf::gpu::Device> &devices,
                                        const wolf::numa::TopologyPaths &paths) {
  StatusTopology result{.host = wolf::numa::discover_topology(paths)};
  for (const auto &device : devices)
    result.gpus.emplace(device.id, wolf::numa::gpu_locality(device.render_node, result.host, paths));
  return result;
}

StatusResponse make_status(const state::AppState &state, const std::vector<wolf::gpu::Device> &devices) {
  // Host identity/topology is stable for this Wolf process; no background work or per-view sysfs scan.
  static const auto topology = discover_status_topology(devices);
  return make_status(state, devices, topology);
}

StatusResponse make_status(const state::AppState &state,
                           const std::vector<wolf::gpu::Device> &devices,
                           const StatusTopology &topology) {
  StatusResponse result;
  result.cpus = topology.host.cpus;
  auto placement = [&](const std::vector<unsigned> &cpus) -> std::optional<StatusPlacement> {
    if (cpus.empty())
      return {};
    return StatusPlacement{.cpus = cpus,
                           .cpu_ids = wolf::numa::socket_ids(cpus, topology.host),
                           .numa_node = wolf::numa::single_numa_node(cpus, topology.host)};
  };
  for (const auto &device : devices) {
    result.gpus.push_back({.id = device.id,
                           .name = device.name,
                           .render_node = device.render_node,
                           .accessible = device.accessible,
                           .vram_bytes = device.vram_bytes,
                           .vram_percent = device.vram_percent,
                           .gpu_percent = device.gpu_percent,
                           .encoder_percent = device.encoder_percent});
    if (const auto locality = topology.gpus.find(device.id); locality != topology.gpus.end()) {
      auto &gpu = result.gpus.back();
      gpu.cpu_ids = locality->second.cpu_ids;
      gpu.numa_node = locality->second.numa_node;
      gpu.local_cpus = locality->second.cpus;
    }
    if (state.gpu_runtime) {
      if (auto cached = state.gpu_runtime->capabilities(device)) {
        auto &gpu = result.gpus.back();
        using Status = wolf::gpu::CapabilityCache::Status;
        gpu.capability_status = cached->status == Status::ready     ? "ready"
                                : cached->status == Status::pending ? "pending"
                                                                    : "unavailable";
        const std::array<std::string, 3> names{"H.264", "HEVC", "AV1"};
        for (std::size_t i = 0; i < names.size(); ++i)
          if (cached->status == Status::ready && cached->codecs[i].encoder)
            gpu.codecs.push_back(names[i]);
      }
    }
  }

  auto sessions = state.running_sessions->load();
  auto lobbies = state.lobbies->load();
  for (const auto &profile : *state.config->profiles->load())
    result.profiles.push_back({.id = profile->id, .name = profile->name});

  for (const auto &session : *sessions) {
    StatusSession status{.id = std::to_string(session.session_id), .app = session.app->base.title};
    status.connected = session.connected->load();
    if (auto profile = session.monitoring->profile_id.load())
      status.profile_id = *profile;
    if (auto route = session.gpu_route) {
      if (auto profile = route->profile_id.load(); profile && !status.profile_id)
        status.profile_id = *profile;
      if (auto target = route->target.load(); target && target->launch)
        status.gpu_id = target->launch->device.id;
    }
    // A successful video worker, rather than negotiation or admission, supplies active settings.
    // The control connection gates retiring workers so disconnects cannot display stale streams.
    if (auto video = session.monitoring->video.load(); video && *status.connected) {
      status.streaming = true;
      if (!video->codec.empty())
        status.codec = video->codec;
      status.bitrate_kbps = video->bitrate_kbps;
      status.width = video->width;
      status.height = video->height;
      status.fps = video->fps;
      status.placement = placement(video->cpus);
      status.gpu_id = video->gpu_id;
      if (!status.gpu_id)
        for (const auto &device : devices)
          if (device.render_node == video->render_node || device.by_path == video->render_node)
            status.gpu_id = device.id;
    }
    for (const auto &lobby : *lobbies)
      for (const auto &viewer : *lobby.connected_sessions->load())
        if (*viewer == status.id)
          status.app = lobby.name;
    result.sessions.push_back(std::move(status));
  }

  for (const auto &[id, container] : *state.active_containers->load()) {
    StatusContainer status{.id = id,
                           .name = container.name,
                           .image = container.image,
                           .app = container.name,
                           .render_node = container.render_node};
    status.placement = placement(container.cpus);
    for (const auto &device : devices)
      if (device.render_node == container.render_node || device.by_path == container.render_node)
        status.gpu_id = device.id;
    for (const auto &session : result.sessions)
      if (session.id == container.session_id) {
        status.session_id = session.id;
        status.profile_id = session.profile_id;
        // A launcher stays on its original GPU while its viewer joins other apps.
        for (const auto &original : *sessions)
          if (std::to_string(original.session_id) == session.id) {
            if (original.app->runner) {
              const auto runner = original.app->runner->serialize();
              if (rfl::holds_alternative<wolf::config::AppDocker>(runner.variant()) &&
                  container.name == rfl::get<wolf::config::AppDocker>(runner.variant()).name + "_" + session.id)
                status.app = original.app->base.title;
            }
            if (!status.gpu_id && original.gpu_launch)
              status.gpu_id = original.gpu_launch->device.id;
          }
      }
    for (const auto &lobby : *lobbies)
      if (lobby.id == container.session_id) {
        status.profile_id = lobby.started_by_profile_id;
        status.app = lobby.name;
        if (!status.gpu_id && lobby.gpu_target && lobby.gpu_target->launch)
          status.gpu_id = lobby.gpu_target->launch->device.id;
      }
    result.containers.push_back(std::move(status));
  }
  return result;
}

} // namespace wolf::api

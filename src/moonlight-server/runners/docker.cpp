#include <cctype>
#include <fstream>
#include <gpu/app_isolation.hpp>
#include <runners/docker.hpp>

namespace wolf::core::docker {

#ifdef __linux__
namespace {
wolf::numa::Placement permitted_guest_resources() {
  wolf::numa::Placement placement;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with("Cpus_allowed_list:"))
      placement.allowed_cpus = wolf::numa::parse_list(std::string_view(line).substr(18));
    if (line.starts_with("Mems_allowed_list:"))
      placement.allowed_memory_nodes = wolf::numa::parse_list(std::string_view(line).substr(18));
  }
  if (placement.allowed_cpus.empty() || placement.allowed_memory_nodes.empty())
    throw std::runtime_error("Cannot determine the resources permitted to Wolf for an app container");
  placement.cpus = placement.allowed_cpus;
  return placement;
}
} // namespace
#endif

boost::json::object with_numa_placement(boost::json::object options, const wolf::numa::Placement &placement) {
  auto &host_value = options["HostConfig"];
  if (host_value.is_null())
    host_value = boost::json::object{};
  auto &host = host_value.as_object();
  auto intersect_local = [&](const char *field, const std::vector<unsigned> &local) {
    auto selected = local;
    if (const auto *configured = host.if_contains(field)) {
      const auto list = std::string_view(configured->as_string());
      if (!list.empty())
        selected = wolf::numa::intersect_lists(local, wolf::numa::parse_list(list));
    }
    if (selected.empty())
      throw std::runtime_error(std::string(field) + " excludes the NUMA policy's permitted resources");
    return wolf::numa::format_list(selected);
  };
  // Validate both before changing either. Empty explicit Docker masks mean unrestricted.
  const auto cpus = intersect_local("CpusetCpus", placement.cpus);
  // Keep all permitted RAM nodes available: the exec launcher prefers local RAM with fallback.
  const auto mems = intersect_local("CpusetMems", placement.allowed_memory_nodes);
  host["CpusetCpus"] = cpus;
  host["CpusetMems"] = mems;
  return options;
}

NumaGuestOptions
with_numa_guest_policy(boost::json::object options, const wolf::numa::Placement &placement, wolf::numa::Mode mode) {
  if (mode != wolf::numa::Mode::off) {
    try {
      return {with_numa_placement(options, placement), true};
    } catch (const std::exception &) {
      if (mode != wolf::numa::Mode::automatic)
        throw;
    }
  }
  // Docker creates a sibling cgroup: fallback must still intersect the app's
  // restrictions with Wolf's full captured allowance, never the daemon's limits.
  auto permitted = placement;
  permitted.cpus = permitted.allowed_cpus;
  return {with_numa_placement(std::move(options), permitted), false};
}

boost::json::object with_numa_bootstrap(boost::json::object options,
                                        const boost::json::object &image_config,
                                        const wolf::numa::Placement &placement) {
  auto array = [](const boost::json::value *value) {
    if (!value || value->is_null())
      return boost::json::array{};
    if (value->is_string())
      return boost::json::array{*value}; // Older Docker APIs accept one literal argument as a string.
    auto result = value->as_array();
    for (const auto &argument : result)
      argument.as_string();
    return result;
  };
  const auto *custom_entry = options.if_contains("Entrypoint");
  auto entry = array(custom_entry);
  auto command = array(options.if_contains("Cmd"));
  // Match Docker's image/default merge before replacing Entrypoint with the launcher.
  if (entry.empty()) {
    if (command.empty())
      command = array(image_config.if_contains("Cmd"));
    if (!custom_entry || custom_entry->is_null())
      entry = array(image_config.if_contains("Entrypoint"));
  }
  if (entry.size() == 1 && entry.front().as_string().empty())
    entry.clear();
  if (entry.empty() && command.empty())
    throw std::runtime_error("NUMA launcher requires an image or app command");
  boost::json::array launcher{"/run/wolf-numa-run",
                              std::to_string(placement.node),
                              wolf::numa::format_list(placement.cpus),
                              "--"};
  launcher.insert(launcher.end(), entry.begin(), entry.end());
  options["Entrypoint"] = std::move(launcher);
  options["Cmd"] = std::move(command);

  auto &host = options.at("HostConfig").as_object();
  auto matches = [](const auto &cap) {
    auto name = std::string(cap.as_string());
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
    return name == "ALL" || name == "CAP_ALL" || name == "SYS_NICE" || name == "CAP_SYS_NICE";
  };
  const auto *dropped = host.if_contains("CapDrop");
  if (dropped && std::any_of(dropped->as_array().begin(), dropped->as_array().end(), matches)) {
    logs::log(logs::warning, "[NUMA] App drops SYS_NICE: the launcher may be unable to prefer local RAM");
  } else {
    auto &caps = host["CapAdd"];
    if (caps.is_null())
      caps = boost::json::array{};
    if (!std::any_of(caps.as_array().begin(), caps.as_array().end(), matches))
      caps.as_array().push_back("SYS_NICE");
  }
  return options;
}

boost::json::object with_hotplug_capabilities(boost::json::object options) {
  auto &host = options["HostConfig"];
  if (host.is_null())
    host = boost::json::object{};
  auto &caps = host.as_object()["CapAdd"];
  if (caps.is_null())
    caps = boost::json::array{};

  // Custom app templates can omit the permissions in Wolf's default templates.
  // fake-udev needs NET_ADMIN to multicast udev events; device nodes need MKNOD.
  for (const auto *required : {"NET_ADMIN", "MKNOD"}) {
    auto matches = [required](const auto &cap) {
      auto name = std::string(cap.as_string());
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
      return name == "ALL" || name == "CAP_ALL" || name == required || name == std::string("CAP_") + required;
    };
    if (std::any_of(caps.as_array().begin(), caps.as_array().end(), matches))
      continue;
    // Preserve explicit restrictions instead of silently overriding a hardened app.
    if (auto dropped = host.as_object().if_contains("CapDrop");
        dropped && dropped->is_array() &&
        std::any_of(dropped->as_array().begin(), dropped->as_array().end(), matches)) {
      logs::log(logs::warning, "[DOCKER] Controller hotplug requires {}, but the app explicitly drops it", required);
      continue;
    }
    caps.as_array().push_back(required);
  }
  return options;
}

std::vector<std::string>
hotplug_command(const std::map<std::string, std::string> &udev_event, const std::string &encoded_event, bool remove) {
  auto node = udev_event.find("DEVNAME");
  if (node == udev_event.end())
    return {"fake-udev", "-m", encoded_event};

  if (remove) {
    // Cleanup must still run when notification fails, without hiding that failure.
    return {"/bin/bash",
            "-c",
            "status=0; fake-udev -m \"$2\" || status=$?; rm -f -- \"$1\" || exit $?; exit \"$status\"",
            "wolf-unplug",
            node->second,
            encoded_event};
  }

  // A previous failed unplug may have left this node behind. Reuse a matching
  // character device, replace stale nodes, and always send the new add event.
  // Pass values as arguments so paths and event contents are never shell code.
  return {"/bin/bash",
          "-c",
          "mkdir -p -- \"$(dirname -- \"$1\")\" && { "
          "if [ ! -c \"$1\" ] || [ \"$(stat -c '%t:%T' -- \"$1\")\" != \"$(printf '%x:%x' \"$2\" \"$3\")\" ]; then "
          "rm -f -- \"$1\" && mknod -- \"$1\" c \"$2\" \"$3\" || exit $?; fi; "
          "chmod 777 -- \"$1\" && fake-udev -m \"$4\"; }",
          "wolf-plug",
          node->second,
          udev_event.at("MAJOR"),
          udev_event.at("MINOR"),
          encoded_event};
}

void create_udev_hw_files(std::filesystem::path base_hw_db_path,
                          std::vector<std::pair<std::string, std::vector<std::string>>> udev_hw_db_entries) {
  for (const auto &[filename, content] : udev_hw_db_entries) {
    auto host_file_path = (base_hw_db_path / filename).string();
    logs::log(logs::debug, "[DOCKER] Writing hwdb file: {}", host_file_path);
    std::ofstream host_file(host_file_path);
    host_file << utils::join(content, "\n");
    host_file.close();
  }
}

/**
 * @brief returns the major number associated with the requested device type (if found)
 * Internally will read /proc/devices line by line and return the first match
 */
static std::optional<std::string> get_device_major(std::string_view type) {
  std::ifstream devices_file("/proc/devices");
  std::string line;
  while (std::getline(devices_file, line)) {
    if (line.find(type) != std::string::npos) {
      // Example line: "244 hidraw" or " 13 input" (note the leading space)
      line.erase(line.begin(),
                 std::find_if(line.begin(), line.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }));
      return line.substr(0, line.find(' '));
    }
  }
  return std::nullopt;
}

void RunDocker::run(std::string_view session_id,
                    std::string_view app_state_folder,
                    std::string_view host_xdg_runtime_dir,
                    std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                    const immer::array<std::string> &virtual_inputs,
                    const immer::array<std::pair<std::string, std::string>> &paths,
                    const immer::map<std::string, std::string> &env_variables,
                    std::string_view render_node) {

  const auto numa_mode = wolf::numa::read_mode();
  const auto locality = wolf::numa::resolve(render_node, {}, numa_mode);
  if (locality.conflict) {
    logs::log(logs::error, "[NUMA] Refusing app {} on {}: {}", session_id, render_node, locality.reason);
    return;
  }
  if (numa_mode != wolf::numa::Mode::off && !locality.placement)
    logs::log(logs::warning, "[NUMA] App {} on {}: {}", session_id, render_node, locality.reason);

  const bool isolate_mesa = env_variables.find("DRI_PRIME") != nullptr;
  const auto nvidia_uuid = env_variables.find("WOLF_NVIDIA_GPU_UUID");
  const bool isolate_nvidia = nvidia_uuid != nullptr;
  std::vector<std::string> full_env;
  full_env.insert(full_env.end(), this->container.env.begin(), this->container.env.end());
  if (isolate_mesa || isolate_nvidia)
    std::erase_if(full_env, [](const auto &entry) {
      return wolf::gpu::mesa_selection_env(entry) || wolf::gpu::nvidia_selection_env(entry);
    });
  for (const auto &env_var : env_variables) {
    full_env.push_back(fmt::format("{}={}", env_var.first, env_var.second));
  }

  std::vector<Device> devices;
  devices.insert(devices.end(), this->container.devices.begin(), this->container.devices.end());
  for (const auto &v_input : virtual_inputs) {
    devices.push_back(Device{.path_on_host = to_string(v_input),
                             .path_in_container = to_string(v_input),
                             .cgroup_permission = "mrw"});
  }

  std::vector<MountPoint> mounts;
  mounts.insert(mounts.end(), this->container.mounts.begin(), this->container.mounts.end());
  for (const auto &path : paths) {
    mounts.insert(
        mounts.end(),
        MountPoint{.source = path.first,
                   .destination = path.second,
                   .mode = path.second == "/usr/nvidia" || path.second == "/run/wolf-numa-run" ? "ro" : "rw"});
  }

  // Fake udev
  auto udev_base_path = std::filesystem::path(app_state_folder) / "udev";
  auto hw_db_path = udev_base_path / "data";
  auto fake_udev_cli_path = std::string(utils::get_env("WOLF_DOCKER_FAKE_UDEV_PATH", ""));
  bool use_fake_udev = !fake_udev_cli_path.empty() || std::filesystem::exists(fake_udev_cli_path);
  if (use_fake_udev) {
    logs::log(logs::debug, "[DOCKER] Using fake-udev, creating {}", hw_db_path.string());
    std::filesystem::create_directories(hw_db_path);

    // Check if /run/udev/control exists
    auto udev_ctrl_path = udev_base_path / "control";
    if (!std::filesystem::exists(udev_ctrl_path)) {
      if (auto control_file = std::ofstream(udev_ctrl_path)) {
        control_file.close();
        std::filesystem::permissions(udev_ctrl_path, std::filesystem::perms::all); // set 777
      }
    }
  } else {
    logs::log(logs::warning,
              "[DOCKER] Unable to use fake-udev, check the env variable WOLF_DOCKER_FAKE_UDEV_PATH and the file at {}",
              fake_udev_cli_path);
  }

  auto final_json_opts = this->base_create_json;
  if (isolate_nvidia) {
    if (!nvidia_uuid->starts_with("GPU-") || nvidia_uuid->size() != 40 ||
        nvidia_uuid->substr(4).find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos)
      throw std::runtime_error("Invalid NVIDIA GPU UUID");
    auto options = utils::parse_json(final_json_opts).as_object();
    if (!options.if_contains("HostConfig"))
      options["HostConfig"] = json::object{};
    auto &host = options["HostConfig"].as_object();
    auto volume = utils::get_env("NVIDIA_DRIVER_VOLUME_NAME");
    if (!volume || !*volume) {
      host["DeviceRequests"] =
          json::array{json::object{{"Driver", "cdi"}, {"DeviceIDs", json::array{"nvidia.com/gpu=" + *nvidia_uuid}}}};
      logs::log(logs::info, "[GPU] App {} requests only CDI GPU {}", session_id, *nvidia_uuid);
    } else {
      host.erase("DeviceRequests");
    }
    host["Runtime"] = "runc";
    if (auto env = options.if_contains("Env")) {
      json::array filtered;
      for (const auto &entry : env->as_array()) {
        auto value = std::string_view(entry.as_string());
        if (!wolf::gpu::nvidia_selection_env(value) && !wolf::gpu::mesa_selection_env(value))
          filtered.push_back(entry);
      }
      options["Env"] = std::move(filtered);
    }
    // Native CDI supplies libraries and visibility; suppress legacy runtime injection.
    full_env.push_back("NVIDIA_VISIBLE_DEVICES=void");
    final_json_opts = json::serialize(options);
  }

  { // Setup Wolf socket path (if the runner needs it, and it hasn't been overridden via ENV)
    auto socket_path_container_env = std::find_if(full_env.begin(), full_env.end(), [](const std::string &env) {
      return env.find("WOLF_SOCKET_PATH") != std::string::npos;
    });
    if (!get_env("WOLF_SOCKET_PATH") && socket_path_container_env != full_env.end()) {
      // Change the associated mount point to pick up the right path from the host
      for (auto &mount : mounts) {
        if (mount.destination.find("wolf.sock") != std::string::npos) {
          mount.source = std::filesystem::path(host_xdg_runtime_dir) / "wolf.sock";
          break;
        }
      }
    }
  }

  // when creating a virtual DualSense device we need to also mount a `/dev/hidraw*` device.
  // unfortunately hidraw devices use dynamically assigned major numbers rather than static ones
  // so we'll get the major number from reading `/proc/devices` for `hidraw` and `input`
  // and set the right entries in `DeviceCgroupRules`
  {
    auto hidraw_major = get_device_major("hidraw");
    auto input_major = get_device_major("input");
    if (hidraw_major && input_major) {
      logs::log(logs::debug,
                "[DOCKER] Setting DeviceCgroupRules for hidraw:{} and input:{}",
                *hidraw_major,
                *input_major);
      auto parsed_json = utils::parse_json(final_json_opts).as_object();
      if (auto host_config_ptr = parsed_json.if_contains("HostConfig")) {
        auto host_config = host_config_ptr->as_object();
        auto &rules = host_config["DeviceCgroupRules"];
        if (rules.is_null())
          rules = json::array{};
        for (const auto &rule : {fmt::format("c {}:* rwm", *hidraw_major), fmt::format("c {}:* rwm", *input_major)})
          if (std::find(rules.as_array().begin(), rules.as_array().end(), json::value(rule)) == rules.as_array().end())
            rules.as_array().push_back(json::value(rule));
        parsed_json["HostConfig"] = host_config;
      } else {
        parsed_json["HostConfig"] = json::object{
            {"DeviceCgroupRules",
             json::array{
                 fmt::format("c {}:* rwm", *hidraw_major),
                 fmt::format("c {}:* rwm", *input_major),
             }},
        };
      }
      final_json_opts = boost::json::serialize(parsed_json);
    } else {
      logs::log(logs::warning, "[DOCKER] Failed to get major numbers for hidraw and input");
    }
  }

  if (isolate_mesa || isolate_nvidia) {
    try {
      // Device nodes are the enforcement boundary; Mesa settings keep normal apps'
      // enumeration aligned. Do not silently accept overrides that expose other GPUs.
      auto options = utils::parse_json(final_json_opts).as_object();
      const auto allowed = linked_devices(render_node);
      auto error = [&]() -> std::string {
        if (allowed.empty())
          return "selected GPU has no device nodes";
        for (const auto &device : devices)
          if (!wolf::gpu::allowed_graphics_device(device.path_on_host, device.path_in_container, allowed))
            return "extra or remapped GPU device: " + device.path_on_host;
        for (const auto &mount : mounts)
          if (wolf::gpu::graphics_mount(mount.source) || wolf::gpu::graphics_mount(mount.destination))
            return "GPU device bind mount: " + mount.source;
        if (auto config = options.if_contains("HostConfig")) {
          const auto &host = config->as_object();
          if (auto privileged = host.if_contains("Privileged"); privileged && privileged->as_bool())
            return "privileged mode exposes all GPUs";
          for (const auto *key : {"VolumesFrom"})
            if (auto value = host.if_contains(key); value && !value->as_array().empty())
              return std::string(key) + " can bypass per-app GPU device selection";
          if (auto requests = host.if_contains("DeviceRequests"); requests && !requests->as_array().empty()) {
            if (!isolate_nvidia || requests->as_array().size() != 1 || requests->as_array()[0].at("Driver") != "cdi" ||
                requests->as_array()[0].at("DeviceIDs") != json::array{"nvidia.com/gpu=" + *nvidia_uuid})
              return "GPU request does not match the assigned NVIDIA UUID";
          }
          if (auto extra = host.if_contains("Devices"))
            for (const auto &entry : extra->as_array()) {
              const auto &device = entry.as_object();
              if (!wolf::gpu::allowed_graphics_device(device.at("PathOnHost").as_string().c_str(),
                                                      device.at("PathInContainer").as_string().c_str(),
                                                      allowed))
                return "additional GPU device in HostConfig.Devices";
            }
          if (auto binds = host.if_contains("Binds"))
            for (const auto &entry : binds->as_array()) {
              auto parts = utils::split(entry.as_string().c_str(), ':');
              if (parts.size() < 2 || wolf::gpu::graphics_mount(parts[0]) || wolf::gpu::graphics_mount(parts[1]))
                return "GPU device bind mount in HostConfig.Binds";
            }
          if (auto mounts = host.if_contains("Mounts"))
            for (const auto &entry : mounts->as_array()) {
              const auto &mount = entry.as_object();
              for (const auto *key : {"Source", "Target"})
                if (auto value = mount.if_contains(key); value && wolf::gpu::graphics_mount(value->as_string().c_str()))
                  return "GPU device bind mount in HostConfig.Mounts";
            }
          if (auto rules = host.if_contains("DeviceCgroupRules"))
            for (const auto &entry : rules->as_array())
              if (wolf::gpu::broad_graphics_rule(entry.as_string().c_str()))
                return "GPU-wide device cgroup rule";
        }
        return {};
      }();
      if (!error.empty()) {
        logs::log(logs::error, "[GPU] Refusing app {}: {}. Expose only the selected GPU nodes.", session_id, error);
        return;
      }
      if (auto env = options.if_contains("Env")) {
        json::array filtered;
        for (const auto &value : env->as_array())
          if (!wolf::gpu::mesa_selection_env(value.as_string().c_str()))
            filtered.push_back(value);
        options["Env"] = std::move(filtered);
      }
      final_json_opts = json::serialize(options);
    } catch (const std::exception &error) {
      logs::log(logs::error, "[GPU] Refusing app {}: invalid isolation configuration: {}", session_id, error.what());
      return;
    }
  }

  if (use_fake_udev)
    final_json_opts = boost::json::serialize(with_hotplug_capabilities(utils::parse_json(final_json_opts).as_object()));

#ifdef __linux__
  if (!locality.placement) {
    // The daemon launches siblings, so they do not inherit Wolf's cpuset. Even
    // when local placement is unavailable, preserve its operator resource limits.
    try {
      final_json_opts = boost::json::serialize(
          with_numa_placement(utils::parse_json(final_json_opts).as_object(), permitted_guest_resources()));
    } catch (const std::exception &error) {
      logs::log(logs::error, "[NUMA] Refusing app {}: {}", session_id, error.what());
      return;
    }
  }
#endif

  if (locality.placement) {
    try {
      auto policy =
          with_numa_guest_policy(utils::parse_json(final_json_opts).as_object(), *locality.placement, numa_mode);
      auto options = std::move(policy.options);
      if (policy.placed) {
        const auto helper = std::find_if(mounts.begin(), mounts.end(), [](const auto &mount) {
          return mount.destination == "/run/wolf-numa-run";
        });
        if (helper == mounts.end())
          throw std::runtime_error("Missing wolf-numa-run mount; install the NUMA launcher in Wolf's state folder");
        auto image = docker_api.inspect_image(this->container.image);
        if (!image && docker_api.pull_image(this->container.image))
          image = docker_api.inspect_image(this->container.image);
        if (!image)
          throw std::runtime_error("Cannot inspect app image to preserve its original startup command");
        options = with_numa_bootstrap(std::move(options),
                                      utils::parse_json(*image).at("Config").as_object(),
                                      *locality.placement);
      }
      final_json_opts = boost::json::serialize(options);
      const auto &host = options.at("HostConfig").as_object();
      if (policy.placed) {
        logs::log(logs::info,
                  "[NUMA] App {} GPU {}: mode {}, local node {}, local CPUs {}, permitted memory nodes {}",
                  session_id,
                  render_node,
                  wolf::numa::mode_name(numa_mode),
                  locality.placement->node,
                  std::string_view(host.at("CpusetCpus").as_string()),
                  std::string_view(host.at("CpusetMems").as_string()));
      } else {
        logs::log(logs::warning,
                  "[NUMA] App {} GPU {}: auto fallback because app resource limits exclude local placement; "
                  "permitted CPUs {}, permitted memory nodes {}",
                  session_id,
                  render_node,
                  std::string_view(host.at("CpusetCpus").as_string()),
                  std::string_view(host.at("CpusetMems").as_string()));
      }
    } catch (const std::exception &error) {
      logs::log(logs::error, "[NUMA] Refusing app {}: {}", session_id, error.what());
      return;
    }
  }

  logs::log(logs::debug, "[DOCKER] Container options: {}", final_json_opts);

  Container new_container = {.id = "",
                             .name = fmt::format("{}_{}", this->container.name, session_id),
                             .image = this->container.image,
                             .status = CREATED,
                             .ports = this->container.ports,
                             .mounts = mounts,
                             .devices = devices,
                             .env = full_env};

  if (auto docker_container = docker_api.create(new_container, final_json_opts)) {
    auto container_id = docker_container->id;
    docker_api.start_by_id(container_id);

    logs::log(logs::info, "[DOCKER] Starting container: {}", docker_container->name);
    logs::log(logs::debug, "[DOCKER] Starting container: {}", *docker_container);

    std::string inspected_hostname;
    if (auto inspected = docker_api.get_by_id(container_id); inspected && inspected->status == RUNNING) {
      inspected_hostname = inspected->hostname;
      this->ev_bus->fire_event(immer::box<events::DockerContainerCreated>{events::DockerContainerCreated{
          .container_id = container_id,
          .hostname = inspected->hostname,
          .session_id = std::string(session_id),
          .name = new_container.name,
          .image = new_container.image,
          .render_node = std::string(render_node),
      }});
    } else {
      logs::log(logs::warning, "[DOCKER] Unable to inspect container {} for hostname", container_id);
    }

    auto terminate_handler = this->ev_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, container_id, this](const immer::box<events::StopStreamEvent> &terminate_ev) {
          if (std::to_string(terminate_ev->session_id) == session_id) {
            docker_api.stop_by_id(container_id);
          }
        });

    auto terminate_lobby_handler = this->ev_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, container_id, this](const immer::box<events::StopLobbyEvent> &terminate_ev) {
          if (terminate_ev->lobby_id == session_id) {
            docker_api.stop_by_id(container_id);
          }
        });

    auto unplug_device_handler = this->ev_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
        [session_id, container_id, hw_db_path, this](const immer::box<events::UnplugDeviceEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[DOCKER] Received UnplugDeviceEvent for session: {}", ev->session_id);
            for (const auto &[filename, content] : ev->udev_hw_db_entries) {
              try {
                std::filesystem::remove(hw_db_path / filename);
              } catch (const std::filesystem::filesystem_error &e) {
                logs::log(logs::warning, "[DOCKER] Failed to remove udev hwdb entry: {}", e.what());
              }
            }

            for (auto udev_ev : ev->udev_events) {
              udev_ev["ACTION"] = "remove";
              std::string udev_msg = base64_encode(map_to_string(udev_ev));
              auto cmd = hotplug_command(udev_ev, udev_msg, true);
              logs::log(logs::debug, "[DOCKER] Executing command: {}", utils::join(cmd, " "));
              docker_api.exec(container_id, std::vector<std::string_view>(cmd.begin(), cmd.end()), "root");
            }
          }
        });

    do {
      // Plug all devices that are waiting in the queue
      while (auto device_ev = plugged_devices_queue->pop(50ms)) {
        if (device_ev->get().session_id == session_id) {
          logs::log(logs::debug, "[DOCKER] Plugging device from queue in session: {}", session_id);
          if (use_fake_udev) {
            create_udev_hw_files(hw_db_path, device_ev->get().udev_hw_db_entries);
          }

          for (auto udev_ev : device_ev->get().udev_events) {
            std::string udev_msg = base64_encode(map_to_string(udev_ev));
            auto cmd = hotplug_command(udev_ev, udev_msg, false);
            logs::log(logs::debug, "[DOCKER] Executing command: {}", utils::join(cmd, " "));
            docker_api.exec(container_id, std::vector<std::string_view>(cmd.begin(), cmd.end()), "root");
          }
        }
      }

      std::this_thread::sleep_for(500ms);

      // value_or() avoids dereferencing nullopt when the container was removed mid-session.
    } while (docker_api.get_by_id(container_id).value_or(Container{.status = EXITED}).status == RUNNING);

    logs::log(logs::debug, "[DOCKER] Container logs: \n{}", docker_api.get_logs(container_id));
    logs::log(logs::debug, "[DOCKER] Stopping container: {}", docker_container->name);

    this->ev_bus->fire_event(immer::box<events::DockerContainerStopped>{events::DockerContainerStopped{
        .container_id = container_id,
        .hostname = inspected_hostname,
        .session_id = std::string(session_id),
    }});

    if (const auto env = utils::get_env("WOLF_STOP_CONTAINER_ON_EXIT")) {
      if (std::string(env) == "TRUE") {
        docker_api.stop_by_id(container_id);
        docker_api.remove_by_id(container_id);
      }
    }

    logs::log(logs::info, "Stopped container: {}", docker_container->name);
    try {
      std::filesystem::remove_all(udev_base_path);
    } catch (const std::filesystem::filesystem_error &e) {
      logs::log(logs::warning, "Failed to remove udev base path: {}", e.what());
    }
  }
}

} // namespace wolf::core::docker

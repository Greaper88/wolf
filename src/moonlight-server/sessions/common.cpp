#include <gpu/app_isolation.hpp>
#include <gpu/discovery.hpp>
#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <platforms/hw.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>

namespace wolf::core::sessions {

void start_runner(std::shared_ptr<events::Runner> runner,
                  std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                  immer::box<RunnerArgs> args) {
  /* Setup devices paths */
  auto all_devices = immer::array_transient<std::string>();

  /* Setup mounted paths */
  immer::array_transient<std::pair<std::string, std::string>> mounted_paths;

  /* Setup environment paths */
  immer::map_transient<std::string, std::string> full_env;
  full_env.set("XDG_RUNTIME_DIR", args->xdg_runtime_dir);
  full_env.set("WOLF_SESSION_ID", args->session_id);

  auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, args->session_id);
  auto audio_server_name = args->audio_server ? audio::get_server_name(args->audio_server->server) : "";
  // TODO: properly separate XDG_RUNTIME_DIR from <pulse socket path>
  // for example on my dev machine it's ${XDG_RUNTIME_DIR}/pulse/native
  // but we know that on our images it's ${XDG_RUNTIME_DIR}/pulse-socket so this should be fine..
  auto audio_server_on_host = std::filesystem::path(args->host->host_xdg_runtime_dir) /
                              std::filesystem::path(audio_server_name).filename();
  full_env.set("PULSE_SINK", pulse_sink_name);
  full_env.set("PULSE_SOURCE", pulse_sink_name + ".monitor");
  full_env.set("PULSE_SERVER", audio_server_name);
  mounted_paths.push_back({audio_server_on_host, audio_server_name});

  full_env.set("GAMESCOPE_WIDTH", std::to_string(args->video_settings.width));
  full_env.set("GAMESCOPE_HEIGHT", std::to_string(args->video_settings.height));
  full_env.set("GAMESCOPE_REFRESH", std::to_string(args->video_settings.refresh_rate));
  full_env.set("WOLF_VIDEO_BUFFER_CAPS", args->video_settings.video_producer_buffer_caps);

  if (auto w_display = args->wayland_display.get()) {
    auto socket_name = virtual_display::get_wayland_socket_name(*w_display);
    auto local_wayland_socket = std::filesystem::path(args->xdg_runtime_dir) / socket_name;
    auto host_wayland_socket = std::filesystem::path(args->host->host_xdg_runtime_dir) / socket_name;
    mounted_paths.push_back({host_wayland_socket, local_wayland_socket});
    full_env.set("WAYLAND_DISPLAY", socket_name);
  }

  /* Adding custom state folder */
  mounted_paths.push_back({args->app_host_state_folder, "/home/retro"});

  /* GPU specific adjustments */
  auto render_node = args->video_settings.runner_render_node;
  auto additional_devices = linked_devices(render_node);
  std::copy(additional_devices.begin(), additional_devices.end(), std::back_inserter(all_devices));

  auto gpu_vendor = get_vendor(render_node);
  if (gpu_vendor == AMD || gpu_vendor == INTEL) {
    auto pci = get_gpu_pci_id(render_node);
    auto prime = pci ? wolf::gpu::mesa_prime_selector(*pci) : std::nullopt;
    if (!prime) {
      logs::log(logs::error, "[GPU] Cannot isolate app {}: missing PCI identity for {}", args->session_id, render_node);
      return;
    }
    full_env.set("DRI_PRIME", *prime);
    // Clear a potentially conflicting image-level vendor/device preference.
    full_env.set("MESA_VK_DEVICE_SELECT", "");
    full_env.set("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1");
    logs::log(logs::info, "[GPU] App {} pinned to {} (DRI_PRIME={})", args->session_id, render_node, *prime);
  }
  if (gpu_vendor == NVIDIA) {
    auto pci = get_gpu_pci_id(render_node);
    auto inventory = wolf::gpu::discover();
    auto selected = std::find_if(inventory.begin(), inventory.end(), [&](const auto &device) {
      return pci && device.id == *pci && device.nvidia_uuid.has_value();
    });
    if (selected == inventory.end()) {
      logs::log(logs::error,
                "[GPU] Cannot isolate app {}: NVIDIA UUID unavailable for {}",
                args->session_id,
                render_node);
      return;
    }
    full_env.set("WOLF_NVIDIA_GPU_UUID", *selected->nvidia_uuid);
    full_env.set("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
    full_env.set("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/10_nvidia.json");
    auto volume = utils::get_env("NVIDIA_DRIVER_VOLUME_NAME");
    full_env.set("VK_DRIVER_FILES",
                 volume && *volume ? "/usr/share/vulkan/icd.d/nvidia_icd.json" : "/etc/vulkan/icd.d/nvidia_icd.json");
    full_env.set("VK_ICD_FILENAMES",
                 volume && *volume ? "/usr/share/vulkan/icd.d/nvidia_icd.json" : "/etc/vulkan/icd.d/nvidia_icd.json");
    full_env.set("__NV_PRIME_RENDER_OFFLOAD", "1");
    full_env.set("__VK_LAYER_NV_optimus", "NVIDIA_only");
    full_env.set("CUDA_VISIBLE_DEVICES", *selected->nvidia_uuid);
    if (auto driver_volume = utils::get_env("NVIDIA_DRIVER_VOLUME_NAME"); driver_volume && *driver_volume) {
      logs::log(logs::info, "Mounting nvidia driver {}:/usr/nvidia", driver_volume);
      mounted_paths.push_back({driver_volume, "/usr/nvidia"});
    }
  } else if (gpu_vendor == INTEL) {
    full_env.set("INTEL_DEBUG", "norbc"); // see: https://github.com/games-on-whales/wolf/issues/50
  }

  full_env.set("PUID", std::to_string(args->client_settings->run_uid));
  full_env.set("PGID", std::to_string(args->client_settings->run_gid));

  // Add fake-udev and udev mounts
  mounted_paths.push_back(
      {std::filesystem::path(args->host->host_base_state_folder) / "fake-udev", "/usr/bin/fake-udev"});
  mounted_paths.push_back({std::filesystem::path(args->app_host_state_folder) / "udev", "/run/udev/"});

  /* Finally run the app, this will stop here until over */
  runner->run(args->session_id,
              args->app_local_state_folder,
              args->host->host_xdg_runtime_dir,
              plugged_devices_queue,
              all_devices.persistent(),
              mounted_paths.persistent(),
              full_env.persistent(),
              render_node);

  if (args->audio_server && args->audio_sink) {
    logs::log(logs::debug, "[STREAM_SESSION] Remove virtual audio sink");
    audio::delete_virtual_sink(args->audio_server->server, args->audio_sink);
  }
}

} // namespace wolf::core::sessions

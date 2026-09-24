#pragma once

#include <events/events.hpp>
#include <gpu/pipeline.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <immer/vector.hpp>
#include <optional>
#include <range/v3/view.hpp>
#include <state/config.hpp>
#include <state/serialised_config.hpp>

namespace state {

using namespace wolf::core;

inline std::optional<events::StreamSession> get_session_by_id(const immer::vector<events::StreamSession> &sessions,
                                                              const std::size_t id) {
  auto results =
      sessions |                                                                                             //
      ranges::views::filter([id](const events::StreamSession &session) { return session.session_id == id; }) //
      | ranges::views::take(1)                                                                               //
      | ranges::to_vector;                                                                                   //
  if (results.size() == 1) {
    return results[0];
  } else if (results.empty()) {
    return {};
  } else {
    logs::log(logs::warning, "Found multiple sessions for a given ID: {}", id);
    return {};
  }
}

inline std::optional<events::StreamSession> get_session_by_client(const immer::vector<events::StreamSession> &sessions,
                                                                  const wolf::config::PairedClient &client) {
  auto client_id = get_client_id(client);
  return get_session_by_id(sessions, client_id);
}

inline std::optional<events::Lobby> get_lobby_by_id(const immer::vector<events::Lobby> &lobbies,
                                                    std::string_view lobby_id) {
  auto results = lobbies |                                                                                      //
                 ranges::views::filter([lobby_id](const events::Lobby &lobby) { return lobby.id == lobby_id; }) //
                 | ranges::views::take(1)                                                                       //
                 | ranges::to_vector;                                                                           //
  if (results.size() == 1) {
    return results[0];
  } else if (results.empty()) {
    return {};
  } else {
    logs::log(logs::warning, "Found multiple lobbies for a given ID: {}", lobby_id);
    return {};
  }
}

inline std::optional<events::Lobby> get_lobby_by_connected_session(const immer::vector<events::Lobby> &lobbies,
                                                                   std::string_view session_id) {
  for (const events::Lobby &lobby : lobbies) {
    immer::vector<immer::box<std::string>> sessions = lobby.connected_sessions->load();
    auto session = std::find_if(sessions.begin(), sessions.end(), [session_id](const auto &session) {
      return session == session_id;
    });
    if (session == sessions.end()) {
      continue;
    }
    return lobby;
  }
  return {};
}

inline std::shared_ptr<events::StreamSession> create_stream_session(immer::box<state::AppState> state,
                                                                    const events::App &run_app,
                                                                    const wolf::config::PairedClient &current_client,
                                                                    const moonlight::DisplayMode &display_mode,
                                                                    int audio_channel_count,
                                                                    const std::string &aes_key,
                                                                    const std::string &aes_iv) {
  auto full_path = std::filesystem::path(state->host->local_base_state_folder) / current_client.app_state_folder /
                   run_app.base.title;
  logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
  std::filesystem::create_directories(full_path);

  std::random_device rd;
  std::mt19937 generator(rd());

  std::uniform_int_distribution<> chars(33, 126); // ASCII values for printable character
  std::array<char, 16> rtp_secret_payload;
  for (auto &c : rtp_secret_payload) {
    c = static_cast<char>(chars(generator));
  }

  std::uniform_int_distribution<u_int32_t> uints(0, UINT32_MAX);

  std::uniform_int_distribution<> ints(0, 255);
  auto rtsp_fake_ip = fmt::format("{}.{}.{}.{}", ints(generator), ints(generator), ints(generator), ints(generator));

  auto session = events::StreamSession{
      .display_mode = display_mode,
      .audio_channel_count = audio_channel_count,
      .event_bus = state->event_bus,
      .client_settings = current_client.settings,
      .app = std::make_shared<events::App>(run_app),
      .app_local_state_folder = full_path.string(),
      .app_host_state_folder = std::filesystem::path(state->host->host_base_state_folder) /
                               current_client.app_state_folder / run_app.base.title,

      .aes_key = aes_key,
      .aes_iv = aes_iv,

      // Moonlight protocol extension to support IP-less connections
      .rtp_secret_payload = rtp_secret_payload,
      .enet_secret_payload = uints(generator),
      .rtsp_fake_ip = rtsp_fake_ip,

      // client info
      .session_id = get_client_id(current_client),
      .video_stream_port = static_cast<unsigned short>(get_port(VIDEO_PING_PORT)),
      .audio_stream_port = static_cast<unsigned short>(get_port(AUDIO_PING_PORT)),
      .control_stream_port = static_cast<unsigned short>(get_port(CONTROL_PORT))};

  session.video_context = state->gst_context;
  return std::make_shared<events::StreamSession>(session);
}

inline std::shared_ptr<const events::GpuStreamTarget>
gpu_target(const AppState &state,
           std::shared_ptr<const wolf::gpu::Runtime::Launch> launch,
           const std::string &producer,
           std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> context = {}) {
  auto target = std::make_shared<events::GpuStreamTarget>();
  target->launch = std::move(launch);
  target->producer = producer;
  target->producer_caps = target->launch->zero_copy ? wolf::gpu::zero_copy_caps : "video/x-raw";
  target->context = context ? context : std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>();
  const auto &cfg = state.config->gpu_video;
  auto bind = [&](std::size_t index, const std::vector<wolf::config::GstEncoder> &encoders) {
    const auto &verified = target->launch->capabilities.codecs[index].encoder;
    if (!verified || (target->launch->zero_copy && !verified->zero_copy_postproc))
      return std::string{};
    std::vector<wolf::gpu::PipelineTemplate> templates;
    for (const auto &e : encoders)
      templates.push_back({e.plugin_name, e.encoder_pipeline});
    return wolf::gpu::bind_pipeline(*verified,
                                    templates,
                                    cfg.default_source,
                                    cfg.default_sink,
                                    target->launch->zero_copy);
  };
  target->pipelines = {bind(0, cfg.h264_encoders), bind(1, cfg.hevc_encoders), bind(2, cfg.av1_encoders)};
  logs::log(logs::info,
            "[GPU] Producer {} uses {} on {}",
            producer,
            target->launch->zero_copy ? "zero-copy (DMA-BUF -> VA)" : "CPU-buffer conversion with hardware encoding",
            target->launch->device.render_node);
  return target;
}

// Called only for a new launch. Resume retains the existing bundle and context.
inline void prepare_gpu_session(const AppState &state, events::StreamSession &session) {
  if (!state.gpu_runtime || !session.app->gpu_auto_select)
    return;
  if (session.app->video)
    throw std::runtime_error(
        "Automatic GPU selection cannot retarget custom video overrides; pin this app's render node");
  auto result = state.gpu_runtime->prepare(std::to_string(session.session_id));
  if (result.bypass)
    return;
  if (!result.launch)
    throw std::runtime_error(result.error);
  auto home = gpu_target(state, result.launch, std::to_string(session.session_id));
  auto app = std::make_shared<events::App>(*session.app);
  app->h264_gst_pipeline = home->pipelines[0];
  app->hevc_gst_pipeline = home->pipelines[1];
  app->av1_gst_pipeline = home->pipelines[2];
  app->video_producer_buffer_caps = home->producer_caps;
  app->render_node = result.launch->device.render_node;
  session.display_mode.hevc_supported = !app->hevc_gst_pipeline.empty();
  session.display_mode.av1_supported = !app->av1_gst_pipeline.empty();
  session.app = std::move(app);
  session.video_context = home->context;
  session.gpu = result.launch->reservation->gpu();
  session.gpu_launch = std::move(result.launch);
  session.gpu_route = std::make_shared<events::GpuStreamRoute>();
  session.gpu_route->home = std::move(home);
  session.gpu_route->target.store(session.gpu_route->home);
  session.gpu_route->runtime = state.gpu_runtime;
}

inline immer::vector<events::StreamSession> remove_session(const immer::vector<events::StreamSession> &sessions,
                                                           const events::StreamSession &session) {
  return sessions                                                                                           //
         | ranges::views::filter([remove_hash = session.session_id](const events::StreamSession &cur_ses) { //
             return cur_ses.session_id != remove_hash;                                                      //
           })                                                                                               //
         | ranges::to<immer::vector<events::StreamSession>>();                                              //
}
} // namespace state

#include <rest/dual-app.hpp>

#include <state/config.hpp>
#include <state/sessions.hpp>

namespace wolf::app_picker {
namespace {
using namespace wolf::core;
using namespace std::chrono_literals;

Reply fail(int status, std::string code, std::string message) {
  return {status, {.success = false, .code = std::move(code), .error = std::move(message)}};
}

std::optional<Reply> ready(const std::optional<events::StreamSession> &session,
                           const std::optional<std::string> &token = {}) {
  if (!session || !session->dual_display || session->dual_display->config.mode != "dual_app")
    return fail(409, "unsupported", "This stream does not support the dual-app picker.");
  const auto dual = session->dual_display;
  if (token && *token != dual->picker_token)
    return fail(409, "session_changed", "The stream changed. Reopen the app picker.");
  if (!session->connected->load() || !dual->negotiated.load() || !dual->streaming.load() || !dual->routing.load() ||
      dual->picker_token.empty())
    return fail(409, "unavailable", "The dual-app stream is not ready.");
  return {};
}

std::optional<immer::box<events::Profile>> profile(const state::AppState &state, const std::string &id) {
  if (id != events::MOONLIGHT_PROFILE_ID)
    for (const auto &candidate : *state.config->profiles->load())
      if (candidate->id == id)
        return candidate;
  return {};
}

std::optional<Reply> profile_access(const events::Profile &profile, const std::optional<std::vector<short>> &pin) {
  if (!profile.pin)
    return {};
  if (!pin)
    return fail(403, "pin_required", "Enter the profile PIN.");
  if (profile.pin != pin)
    return fail(403, "invalid_pin", "The profile PIN is incorrect.");
  return {};
}

std::optional<immer::box<events::App>> saved_app(const events::Profile &profile, const std::string &id) {
  for (const auto &app : *profile.apps->load())
    if (app->base.id == id)
      return app;
  return {};
}

// Match the private persistent app identity used by Wolf-UI. Only trusted saved
// Docker runners have this identity; the client cannot choose a runner or path.
std::optional<std::string> app_folder(const events::Profile &profile, const events::App &app) {
  if (!app.runner)
    return {};
  const auto runner = app.runner->serialize();
  if (!rfl::holds_alternative<wolf::config::AppDocker>(runner.variant()))
    return {};
  return "profile-data/" + profile.id + "/" + rfl::get<wolf::config::AppDocker>(runner.variant()).name;
}

std::optional<events::Lobby>
app_lobby(const state::AppState &state, const events::Profile &profile, const events::App &app) {
  const auto folder = app_folder(profile, app);
  if (folder)
    for (const auto &lobby : *state.lobbies->load())
      if (lobby.started_by_profile_id == profile.id && lobby.runner_state_folder == *folder)
        return lobby;
  return {};
}

std::optional<std::string> lobby_app_id(const state::AppState &state, const events::Lobby &lobby) {
  if (const auto owner = profile(state, lobby.started_by_profile_id))
    for (const auto &app : *(*owner)->apps->load())
      if (app_folder(**owner, *app) == lobby.runner_state_folder)
        return app->base.id;
  return {};
}

State snapshot(const state::AppState &state, const events::StreamSession &session) {
  const auto dual = session.dual_display;
  const auto routing = dual->routing.load();
  State result{.session_token = dual->picker_token, .available = true, .mode = dual->config.mode};
  for (const auto &p : *state.config->profiles->load())
    if (p->id != events::MOONLIGHT_PROFILE_ID)
      result.profiles.push_back({p->id, p->name, p->pin.has_value()});
  for (int index = 0; index < 2; ++index) {
    const auto &geometry = dual->config.displays[index];
    const auto &slot = routing->slots[index];
    Display display{.display_id = index,
                    .width = geometry.width,
                    .height = geometry.height,
                    .fps = geometry.fps,
                    .state = slot.lobby_id ? "running" : "launcher",
                    .assignment_id = slot.lobby_id.value_or("launcher")};
    if (slot.lobby_id)
      if (const auto lobby = state::get_lobby_by_id(state.lobbies->load(), *slot.lobby_id)) {
        display.app_id = lobby_app_id(state, *lobby);
        display.app_name = lobby->name;
        display.profile_id = lobby->started_by_profile_id;
      }
    result.displays.push_back(std::move(display));
  }
  return result;
}

Reply success(const state::AppState &state, const events::StreamSession &session) {
  return {200, {.success = true, .state = snapshot(state, session)}};
}

std::optional<Reply> target_access(const events::StreamSession &session, int display, const std::string &expected) {
  if (display < 0 || display > 1 || expected.empty())
    return fail(400, "invalid_request", "Choose display zero or one and its current assignment.");
  if (session.dual_display->routing.load()->slots[display].lobby_id.value_or("launcher") != expected)
    return fail(409, "conflict", "This display changed. Refresh the app picker.");
  return {};
}

bool busy(const events::Lobby &lobby, const events::StreamSession &session) {
  for (const auto &viewer : *lobby.connected_sessions->load())
    if (*viewer != std::to_string(session.session_id))
      return true;
  return false;
}

std::optional<Reply> lobby_access(const events::Lobby &lobby,
                                  const events::StreamSession &session,
                                  int display,
                                  const std::optional<std::vector<short>> &pin) {
  if (lobby.multi_user)
    return fail(409, "conflict", "This app is running in shared mode. Use Wolf-UI to join it.");
  if (lobby.display_config)
    return fail(409, "conflict", "Stop this desktop before changing its monitor layout.");
  if (busy(lobby, session))
    return fail(409, "busy", "This app is in use on another device.");
  if (session.dual_display->routing.load()->slots[1 - display].lobby_id == lobby.id)
    return fail(409, "conflict", "This app is already on the other display.");
  if (lobby.pin) {
    if (!pin)
      return fail(403, "lobby_pin_required", "Enter the running app PIN.");
    if (lobby.pin != pin)
      return fail(403, "invalid_lobby_pin", "The running app PIN is incorrect.");
  }
  return {};
}

Reply join(const immer::box<state::AppState> &state,
           const events::StreamSession &session,
           const events::Lobby &lobby,
           const AssignRequest &request) {
  if (const auto error = lobby_access(lobby, session, request.display_id, request.lobby_pin))
    return *error;
  events::JoinLobbyEvent event{.profile_id = request.profile_id,
                               .lobby_id = lobby.id,
                               .moonlight_session_id = session.session_id,
                               .pin = request.lobby_pin,
                               .display_id = request.display_id};
  auto result = event.error_message.get()->get_future();
  state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(event));
  if (result.wait_for(2s) != std::future_status::ready)
    return fail(503, "failed", "The app did not finish joining.");
  if (!result.get().empty())
    return fail(409, "failed", "Wolf could not assign this app. Refresh and retry.");
  return success(*state, session);
}

template <typename Request> auto read(const std::string &body) {
  return rfl::json::read<Request, rfl::NoExtraFields>(body);
}
} // namespace

Reply handle(Operation operation,
             const state::PairedClient &client,
             const immer::box<state::AppState> &state,
             const std::string &body) {
  std::unique_lock transition(*state->gpu_transitions);
  auto session = state::get_session_by_client(state->running_sessions->load(), client);
  if (!session || !session->dual_display)
    return fail(409, "unsupported", "This stream does not support the dual-app picker.");
  const auto dual = session->dual_display;
  std::unique_lock picker(dual->picker_transitions);
  if (const auto error = ready(session))
    return *error;
  if (operation == Operation::state)
    return success(*state, *session);

  if (operation == Operation::clear) {
    const auto request = read<ClearRequest>(body);
    if (!request || request->version != 1)
      return fail(400, "invalid_request", "The clear request is invalid.");
    if (const auto error = ready(session, request->session_token))
      return *error;
    if (const auto error = target_access(*session, request->display_id, request->expected_assignment))
      return *error;
    if (const auto lobby = dual->routing.load()->slots[request->display_id].lobby_id)
      state->event_bus->fire_event(
          immer::box<events::LeaveLobbyEvent>{events::LeaveLobbyEvent{*lobby, session->session_id}});
    return success(*state, *session);
  }

  if (operation == Operation::catalog) {
    const auto request = read<CatalogRequest>(body);
    if (!request || request->version != 1)
      return fail(400, "invalid_request", "The catalog request is invalid.");
    if (const auto error = ready(session, request->session_token))
      return *error;
    const auto owner = profile(*state, request->profile_id);
    if (!owner)
      return fail(404, "not_found", "The saved profile was not found.");
    if (const auto error = profile_access(**owner, request->profile_pin))
      return *error;
    auto result = success(*state, *session);
    result.body.apps.emplace();
    const auto routing = dual->routing.load();
    for (const auto &app : *(*owner)->apps->load()) {
      const auto lobby = app_lobby(*state, **owner, *app);
      App entry{.id = app->base.id,
                .name = app->base.title,
                .running = lobby.has_value(),
                .available = app_folder(**owner, *app).has_value() &&
                             (!lobby || (!lobby->multi_user && !lobby->display_config && !busy(*lobby, *session)))};
      if (lobby)
        for (int index = 0; index < 2; ++index)
          if (routing->slots[index].lobby_id == lobby->id)
            entry.assigned_display = index;
      result.body.apps->push_back(std::move(entry));
    }
    return result;
  }

  const auto request = read<AssignRequest>(body);
  if (!request || request->version != 1)
    return fail(400, "invalid_request", "The assignment request is invalid.");
  if (const auto error = ready(session, request->session_token))
    return *error;
  if (const auto error = target_access(*session, request->display_id, request->expected_assignment))
    return *error;
  auto owner = profile(*state, request->profile_id);
  if (!owner)
    return fail(404, "not_found", "The saved profile was not found.");
  if (const auto error = profile_access(**owner, request->profile_pin))
    return *error;
  const auto app = saved_app(**owner, request->app_id);
  if (!app)
    return fail(404, "not_found", "The saved app was not found.");
  const auto folder = app_folder(**owner, **app);
  if (!folder)
    return fail(409, "unsupported", "Use Wolf-UI to start this app type.");
  if (!state->gpu_runtime || !session->gpu_route)
    return fail(409, "unsupported", "This launcher needs automatic GPU selection.");

  auto lobby = app_lobby(*state, **owner, **app);
  if (lobby) {
    if (const auto error = lobby_access(*lobby, *session, request->display_id, request->lobby_pin))
      return *error;
    if (lobby->wayland_display->load().get())
      return join(state, *session, *lobby, *request);
  }

  std::optional<std::future<bool>> setup;
  std::string lobby_id;
  if (!lobby) {
    lobby_id = state::gen_uuid();
    const auto held = state->gpu_runtime->retain("app:" + lobby_id, *session->gpu_route->target.load()->launch);
    if (!held.launch)
      return fail(503, "unavailable", "Wolf cannot reserve a GPU for this app.");
    const auto target = state::gpu_target(*state, held.launch, lobby_id);
    const auto &geometry = dual->config.displays[request->display_id];
    try {
      state::validate_gpu_locality(held.launch->device.render_node);
    } catch (const std::exception &) {
      return fail(503, "unavailable", "The app GPU is unavailable.");
    }
    events::CreateLobbyEvent event{.gpu_target = target,
                                   .id = lobby_id,
                                   .profile_id = request->profile_id,
                                   .name = (*app)->base.title,
                                   .icon_png_path = (*app)->base.icon_png_path,
                                   .multi_user = false,
                                   .stop_when_everyone_leaves = false,
                                   .video_settings = {.width = geometry.width,
                                                      .height = geometry.height,
                                                      .refresh_rate = geometry.fps,
                                                      .wayland_render_node = held.launch->device.render_node,
                                                      .runner_render_node = held.launch->device.render_node,
                                                      .video_producer_buffer_caps = target->producer_caps},
                                   .audio_settings = {.channel_count = session->audio_channel_count},
                                   .client_settings = session->client_settings,
                                   .runner_state_folder = *folder,
                                   .runner = (*app)->runner};
    setup.emplace(event.on_setup_over.get()->get_future());
    state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(event));
  } else {
    lobby_id = lobby->id;
  }

  // Compositor startup must not block Wolf-UI, reconnect or slot transitions.
  // Keep old assignments intact and revalidate before the single atomic join.
  picker.unlock();
  transition.unlock();
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (true) {
    if (setup && setup->wait_for(10ms) == std::future_status::ready) {
      if (!setup->get())
        return fail(503, "failed", "The app could not start. Check Wolf's logs.");
      setup.reset();
    }
    transition.lock();
    picker.lock();
    session = state::get_session_by_client(state->running_sessions->load(), client);
    if (!session || session->dual_display != dual)
      return fail(409, "session_changed", "The stream changed. Reopen the app picker.");
    if (const auto error = ready(session, request->session_token))
      return *error;
    if (const auto error = target_access(*session, request->display_id, request->expected_assignment))
      return *error;
    owner = profile(*state, request->profile_id);
    if (!owner || !saved_app(**owner, request->app_id))
      return fail(404, "not_found", "The saved app or profile changed.");
    if (const auto error = profile_access(**owner, request->profile_pin))
      return *error;
    const auto ready_lobby = state::get_lobby_by_id(state->lobbies->load(), lobby_id);
    if (!ready_lobby)
      return fail(503, "failed", "The app stopped before it could be assigned.");
    if (!ready_lobby->gpu_target || !ready_lobby->gpu_target->launch->reservation->valid())
      return fail(503, "failed", "The app stopped before it could be assigned.");
    if (!setup && ready_lobby->wayland_display->load().get())
      return join(state, *session, *ready_lobby, *request);
    if (std::chrono::steady_clock::now() >= deadline)
      return fail(503, "failed", "The app did not become ready in time.");
    picker.unlock();
    transition.unlock();
    if (!setup)
      std::this_thread::sleep_for(10ms);
  }
}

void send(const std::shared_ptr<HttpsServer::Response> &response, const Reply &reply) {
  response->write(static_cast<SimpleWeb::StatusCode>(reply.status),
                  rfl::json::write(reply.body),
                  {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}});
  response->close_connection_after_response = true;
}

} // namespace wolf::app_picker

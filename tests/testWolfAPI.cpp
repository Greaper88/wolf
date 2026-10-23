#include <api/api.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <helpers/tsqueue.hpp>
#include <helpers/utils.hpp>
#include <rfl/toml.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <streaming/streaming.hpp>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;

using namespace wolf::api;
using curl_ptr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

std::string api_runtime_dir() {
  return utils::get_env("XDG_RUNTIME_DIR", "/tmp/");
}

std::string api_socket_path() {
  return (std::filesystem::path(api_runtime_dir()) / "wolf.sock").string();
}

/**
 * Perform a HTTP request using curl
 */
std::optional<std::pair<long /* response_code */, std::string /* raw message */>>
req(CURL *handle,
    HTTPMethod method,
    std::string_view target,
    std::string_view post_body = {},
    const std::vector<std::string> &header_params = {}) {
  logs::log(logs::trace, "[CURL] Sending [{}] -> {}", (int)method, target);
  curl_easy_setopt(handle, CURLOPT_URL, target.data());

  /* Set method */
  switch (method) {
  case HTTPMethod::GET:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "GET");
    break;
  case HTTPMethod::POST:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "POST");
    break;
  case HTTPMethod::PUT:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
    break;
  case HTTPMethod::DELETE:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    break;
  }
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  struct curl_slist *headers = nullptr;
  for (const auto &header : header_params) {
    headers = curl_slist_append(headers, header.c_str());
  }

  /* Pass POST params (if present) */
  if (method == HTTPMethod::POST && !post_body.empty()) {
    logs::log(logs::trace, "[CURL] POST: {}", post_body);

    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    headers = curl_slist_append(headers, "Content-type: application/json");
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, post_body.size());
  } else {
    curl_easy_setopt(handle, CURLOPT_POST, 0L);
  }
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

  /* Set custom writer (in order to receive back the response) */
  curl_easy_setopt(
      handle,
      CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>([](char *ptr, size_t size, size_t nmemb, void *read_buf) {
        *(static_cast<std::string *>(read_buf)) += std::string{ptr, size * nmemb};
        return size * nmemb;
      }));
  std::string read_buf;
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &read_buf);

  /* Run! */
  auto res = curl_easy_perform(handle);
  curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    logs::log(logs::warning, "[CURL] Request failed with error: {}", curl_easy_strerror(res));
    return {};
  } else {
    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
    // Avoid printing binary output if the target is "/api/v1/utils/get-icon"
    if (target.find("/api/v1/utils/get-icon") == std::string::npos) {
      logs::log(logs::trace, "[CURL] Received {} - {}", response_code, read_buf);
    }
    return {{response_code, read_buf}};
  }
}

TEST_CASE("Pair APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = state::load_or_default("config.test.toml", event_bus, running_sessions);
  { // Avoid overriding the test config file (shared across multiple tests)
    config.config_source = "config.test.EDITED.toml";
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>("config.test.toml").value();
    rfl::toml::save(config.config_source, tml);
  }
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = immer::box<state::Config>(config),
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the server to start

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/pair/pending");
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true,\"requests\":[]}"));

  // Checkout the list of paired clients (there will be one in the test config file)
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/clients");
  REQUIRE(response);
  REQUIRE_THAT(response->second,
               Equals("{\"success\":true,\"clients\":["
                      "{\"client_id\":\"10594003729173467913\","
                      "\"app_state_folder\":\"some/folder\","
                      "\"settings\":{"
                      "\"run_uid\":1234,"
                      "\"run_gid\":5678,"
                      "\"controllers_override\":[\"PS\"],"
                      "\"mouse_acceleration\":2.5,"
                      "\"v_scroll_acceleration\":1.5,"
                      "\"h_scroll_acceleration\":10.199999809265137,"
                      "\"motion_controller_override\":\"AUTO\"}}]}"));

  auto pair_promise = std::make_shared<boost::promise<std::string>>();

  // Simulate a Moonlight pairing request
  app_state->pairing_atom->update([pair_promise](auto pairing_map) {
    return pairing_map.set("secret",
                           immer::box<events::PairSignal>{
                               events::PairSignal{.client_ip = "1234", .host_ip = "5678", .user_pin = pair_promise}});
  });

  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/pair/pending");
  REQUIRE(response);
  REQUIRE_THAT(response->second,
               Equals("{\"success\":true,\"requests\":[{\"pair_secret\":\"secret\",\"client_ip\":\"1234\"}]}"));

  // Let's complete the pairing process
  response = req(curl.get(),
                 HTTPMethod::POST,
                 "http://localhost/api/v1/pair/client",
                 "{\"pair_secret\":\"secret\",\"pin\":\"1234\"}");
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
  REQUIRE(pair_promise->get_future().get() == "1234");
  REQUIRE(app_state->config.get().paired_clients->load().get().size() == 1);

  { // Test out if pairing with the same client_id overrides the previous client
    // https://github.com/games-on-whales/wolf/issues/211
    auto cfg = app_state->config.get();
    immer::box<state::PairedClient> previous_client = cfg.paired_clients->load().get()[0];
    state::pair(
        cfg,
        state::PairedClient{.client_cert = previous_client->client_cert, .app_state_folder = "ASDF", .settings = {}});
    REQUIRE(cfg.paired_clients->load().get().size() == 1);
    REQUIRE(cfg.paired_clients->load().get()[0]->app_state_folder == "ASDF");
  }

  REQUIRE(app_state->pairing_atom->load().get().size() == 0);

  { // Test out changing client settings
    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/clients/settings",
                   "{\"client_id\":\"10594003729173467913\",\"app_state_folder\":\"OVERRIDDEN\", \"settings\":{}}");
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
    REQUIRE_THAT(app_state->config.get().paired_clients->load().get()[0]->app_state_folder, Equals("OVERRIDDEN"));

    // Check back that we've correctly updated the config file
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(config.config_source).value();
    REQUIRE(tml.paired_clients[0].app_state_folder == "OVERRIDDEN");
  }

  { // Test out unpairing
    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/unpair/client",
                   "{\"client_id\":\"10594003729173467913\"}");
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

    // Check back that we've correctly updated the config file
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(config.config_source).value();
    REQUIRE(tml.paired_clients.empty());
  }
}

TEST_CASE("APPs APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = state::load_or_default("config.test.toml", event_bus, running_sessions);
  // Avoid overriding the test config file (shared across multiple tests)
  config.config_source = "config.test.EDITED.toml";
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = {config},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the server to start

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test that the initial list of apps matches what's in the test config file
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
  REQUIRE(response);
  auto apps = rfl::json::read<AppListResponse>(response->second).value();
  REQUIRE(apps.success);
  REQUIRE(apps.apps.size() == 2);
  REQUIRE(apps.apps[0].title == "Firefox");
  REQUIRE(apps.apps[1].title == "Test ball");

  // Test that we can add an app
  auto app = rfl::Reflector<wolf::core::events::App>::ReflType{
      .title = "Test app",
      .id = "test",
      .support_hdr = false,
      .h264_gst_pipeline = "h264",
      .hevc_gst_pipeline = "hevc",
      .av1_gst_pipeline = "av1",
      .render_node = "render",
      .opus_gst_pipeline = "opus",
      .start_virtual_compositor = false,
      .runner = wolf::config::AppDocker{.name = "test",
                                        .image = "test",
                                        .mounts = {"/tmp:/tmp"},
                                        .env = {"LOG_LEVEL=1234"},
                                        .devices = {"/dev/input:/dev/input"},
                                        .ports = {"8080:8080"}}};
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/add", rfl::json::write(app));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  // Test that the new app is in the list
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
  REQUIRE(response);
  auto apps2 = rfl::json::read<AppListResponse>(response->second).value();
  REQUIRE(apps2.success);
  REQUIRE(apps2.apps.size() == 3);
  REQUIRE(apps2.apps[2].title == "Test app");
  auto moonlight_profile = state::get_moonlight_profile(config);
  REQUIRE(moonlight_profile);
  immer::vector<immer::box<events::App>> wolf_apps = moonlight_profile.value()->apps->load();
  REQUIRE(wolf_apps.at(2)->base.title == "Test app");

  // Test that we can remove an app
  auto app_delete = AppDeleteRequest{.id = "test"};
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/delete", rfl::json::write(app_delete));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
  REQUIRE(response);
  auto apps3 = rfl::json::read<AppListResponse>(response->second).value();
  REQUIRE(apps3.success);
  REQUIRE(apps3.apps.size() == 2);
}

TEST_CASE("Profile APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = state::load_or_default("config.test.toml", event_bus, running_sessions);
  // Avoid overriding the test config file (shared across multiple tests)
  config.config_source = "config.test.EDITED.toml";
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = {config},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the server to start

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test the initial profile list only has the default user
  {
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/profiles");
    REQUIRE(response);
    REQUIRE_THAT(response->second,
                 Catch::Matchers::Equals(
                     R"({"success":true,"profiles":[{"name":"User","id":"user","icon_png_path":"","apps":[]}]})"));
  }

  // Test adding a profile
  {
    auto runner = std::make_shared<process::RunProcess>(event_bus, "destroy-all-humans.exe");
    auto apps = immer::vector<immer::box<events::App>>{
        events::App{.base = {.title = "Destroy All Humans", .id = "2", .support_hdr = false, .icon_png_path = ""},
                    .h264_gst_pipeline = "h264",
                    .hevc_gst_pipeline = "hevc",
                    .av1_gst_pipeline = "av1",
                    .render_node = "render_node",
                    .opus_gst_pipeline = "opus",
                    .start_virtual_compositor = true,
                    .start_audio_server = false,
                    .runner = runner}};
    auto profile = events::Profile{
        .id = "test",
        .name = "Test",
        .icon_png_path = "",
        .apps = std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(apps),
    };
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/profiles/add", rfl::json::write(profile));
    REQUIRE(response);
    REQUIRE_THAT(response->second, Catch::Matchers::Equals(R"({"success":true})"));

    // Test the profile list now has the new profile
    auto response2 = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/profiles");
    REQUIRE(response2);
    auto profiles = rfl::json::read<ProfileListResponse>(response2->second);
    REQUIRE(profiles);
    REQUIRE(profiles.value().profiles.size() == 2);
    REQUIRE_THAT(profiles.value().profiles[1].name, Equals(profile.name));
    REQUIRE_THAT(profiles.value().profiles[1].id, Equals(profile.id));
    REQUIRE_THAT(profiles.value().profiles[1].icon_png_path, Equals(profile.icon_png_path));
    REQUIRE(profiles.value().profiles[1].apps.size() == 1);
  }

  // Test removing a profile
  {
    auto response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/profiles/remove", R"({"id":"test"})");
    REQUIRE(response);
    REQUIRE_THAT(response->second, Catch::Matchers::Equals(R"({"success":true})"));
    // Test the profile list now has the new profile
    auto response2 = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/profiles");
    REQUIRE(response2);
    auto profiles = rfl::json::read<ProfileListResponse>(response2->second);
    REQUIRE(profiles);
    REQUIRE(profiles.value().profiles.size() == 1);
    REQUIRE_THAT(profiles.value().profiles[0].name, Equals("User"));
  }
}

TEST_CASE("Sessions APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the server to start

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test that the initial list of sessions is empty
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions.success);
  REQUIRE(sessions.sessions.size() == 0);

  // Test that we can add a session
  auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
      .client_ip = "127.0.0.1",
      .app_id = "304556286",               // test cfg file
      .client_id = "10594003729173467913", // test cfg file
  };
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Catch::Matchers::ContainsSubstring("{\"success\":true,\"session_id\":"));

  // Test that the new session is in the list
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions2 = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions2.success);
  REQUIRE(sessions2.sessions.size() == 1);

  // TODO: breaks Github CI
  // // Test that we can send input to a session
  // auto input_request = StreamSessionHandleInputRequest{
  //     .session_id = "10594003729173467913",
  //     // example CONTROLLER_MULTI packet
  //     .input_packet_hex = "060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500"};
  // response =
  //     req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/input", rfl::json::write(input_request));
  // REQUIRE(response);
  // REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  // Test that we can pause a session
  auto pause_request = StreamSessionPauseRequest{.session_id = "10594003729173467913"};
  response =
      req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/pause", rfl::json::write(pause_request));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  // Test that we can stop a session
  auto stop_request = StreamSessionStopRequest{.session_id = "10594003729173467913"};
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/stop", rfl::json::write(stop_request));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
}

TEST_CASE("Session APIs without app_id or client_id", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the server to start

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test that the initial list of sessions is empty
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions.success);
  REQUIRE(sessions.sessions.size() == 0);

  // Test that we can add a session
  auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
      .client_ip = "127.0.0.1",
  };

  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Catch::Matchers::ContainsSubstring("{\"success\":true,\"session_id\":"));

  // Test that the new session is in the list
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions2 = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions2.success);
  REQUIRE(sessions2.sessions.size() == 1);
}

// A device unplugged while its session is inside a lobby must be relayed to the lobby's runner.
TEST_CASE("Lobby relays a device unplug to its runner", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});

  // A lobby with one connected Moonlight session, added straight to state (no compositor).
  const std::string lobby_id = "lobby-under-test";
  const std::string moonlight_session_id = "1234";
  events::Lobby lobby{.id = lobby_id,
                      .name = "test_lobby",
                      .started_by_profile_id = "test_profile",
                      .multi_user = false,
                      .stop_when_everyone_leaves = false};
  lobby.connected_sessions->update([&](const immer::vector<immer::box<std::string>> &connected) {
    return connected.push_back(immer::box<std::string>{moonlight_session_id});
  });
  app_state->lobbies->update([&](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(lobby); });

  auto lobbies_handlers = sessions::setup_lobbies_handlers(app_state, api_runtime_dir(), {});

  int relayed_to_lobby = 0;
  auto observer = event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [&](const immer::box<events::UnplugDeviceEvent> &ev) {
        if (ev->session_id == lobby_id) {
          relayed_to_lobby++;
        }
      });

  event_bus->fire_event(
      immer::box<events::UnplugDeviceEvent>{events::UnplugDeviceEvent{.session_id = moonlight_session_id}});

  REQUIRE(relayed_to_lobby == 1);
}

TEST_CASE("Lobbies APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});
  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();

  // Setup the event bus handlers for the lobbies events
  auto lobbies_handlers = sessions::setup_lobbies_handlers(app_state, runtime_dir, {});

  // Wait for the server to start
  std::this_thread::sleep_for(std::chrono::milliseconds(42));

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  { // Test that the initial list of lobbies is empty
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 0);
  }

  std::string lobby_id;
  std::vector<short> pin = {1, 2, 3, 4};
  { // Test creating a lobby
    auto new_lobby = CreateLobbyRequest{.profile_id = "test_profile",
                                        .name = "test_lobby",
                                        .multi_user = false,
                                        .pin = pin,
                                        .stop_when_everyone_leaves = false,
                                        .video_settings = {.width = 1920,
                                                           .height = 1080,
                                                           .refresh_rate = 60,
                                                           .wayland_render_node = "software",
                                                           .runner_render_node = "runner_render_node",
                                                           .video_producer_buffer_caps = "video/x-raw"},
                                        .audio_settings = {.channel_count = 2},
                                        .runner_state_folder = "runner_state_folder",
                                        .runner = wolf::config::AppCMD{.run_cmd = "sleep 10"}};
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/lobbies/create", rfl::json::write(new_lobby));
    REQUIRE(response);
    auto lobby_res = rfl::json::read<LobbyCreateResponse>(response->second).value();
    REQUIRE(lobby_res.success);
    REQUIRE(!lobby_res.lobby_id.empty());
    lobby_id = lobby_res.lobby_id;
    // Test that the lobby is listed in the list of lobbies
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].id, Equals(lobby_res.lobby_id));
    REQUIRE(sessions.lobbies[0].connected_sessions.size() == 0);
  }

  std::size_t moonlight_session_id = 1234;
  { // Test joining a lobby
    // First, create a StreamSession
    running_sessions->update([moonlight_session_id](const immer::vector<events::StreamSession> &sessions) {
      return sessions.push_back(events::StreamSession{.session_id = moonlight_session_id});
    });
    // Then, call the join endpoint
    auto payload =
        rfl::json::write(events::JoinLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = moonlight_session_id});
    auto response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/lobbies/join", payload);
    REQUIRE(response);
    auto error_res = rfl::json::read<GenericErrorResponse>(response->second).value();
    // we expect this to fail since we don't pass the PIN!
    REQUIRE(!error_res.success);

    // Now call it with the right PIN
    response = req(
        curl.get(),
        HTTPMethod::POST,
        "http://localhost/api/v1/lobbies/join",
        rfl::json::write(
            events::JoinLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = moonlight_session_id, .pin = pin}));
    REQUIRE(response);
    auto join_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    REQUIRE(join_res.success);

    // Test that the lobby now lists this session
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].id, Equals(lobby_id));
    REQUIRE(sessions.lobbies[0].connected_sessions.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].connected_sessions[0], Equals(std::to_string(moonlight_session_id)));
  }

  { // Test leaving a lobby
    auto response = req(
        curl.get(),
        HTTPMethod::POST,
        "http://localhost/api/v1/lobbies/leave",
        rfl::json::write(events::LeaveLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = moonlight_session_id}));
    REQUIRE(response);
    auto leave_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    REQUIRE(leave_res.success);
    // Test that the lobby no longer lists this session
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].id, Equals(lobby_id));
    REQUIRE(sessions.lobbies[0].connected_sessions.empty());
  }

  { // Test stopping the lobby
    auto response = req(curl.get(),
                        HTTPMethod::POST,
                        "http://localhost/api/v1/lobbies/stop",
                        rfl::json::write(events::StopLobbyEvent{.lobby_id = lobby_id}));
    REQUIRE(response);
    auto error_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    // We expect this to fail since we don't pass a PIN!
    REQUIRE(!error_res.success);
    // Now try again with the PIN
    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/lobbies/stop",
                   rfl::json::write(events::StopLobbyEvent{.lobby_id = lobby_id, .pin = pin}));
    REQUIRE(response);
    auto stop_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    REQUIRE(stop_res.success);

    // Test that the lobby no longer exists
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.empty());
  }

  // Give it a few seconds to turn down the Gstreamer pipeline
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_CASE("Utils APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});
  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();

  // Wait for the server to start
  std::this_thread::sleep_for(std::chrono::milliseconds(42));

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  { // Test that calling
    // http://localhost/api/v1/utils/get-icon\?icon_path\=https://games-on-whales.github.io/wildlife/apps/firefox/assets/icon.png
    // returns the content of the PNG file
    auto response = req(curl.get(),
                        HTTPMethod::GET,
                        "http://localhost/api/v1/utils/get-icon?icon_path=https://games-on-whales.github.io/wildlife/"
                        "apps/firefox/assets/icon.png");

    REQUIRE(response);
    REQUIRE_THAT(response->second, Catch::Matchers::ContainsSubstring("PNG"));
  }

  { // Test that when getting a non 200 from a remote resource we don't return an icon
    auto response = req(curl.get(),
                        HTTPMethod::GET,
                        "http://localhost/api/v1/utils/get-icon?icon_path=https://games-on-whales.github.io/wildlife/"
                        "THIS_PAGE_DOES_NOT_EXISTS");

    REQUIRE(response);
    REQUIRE(response->first == 404);
  }
}

struct SSEEvent {
  std::string event;
  std::string data;
};

void listen_sse(CURL *handle, std::shared_ptr<TSQueue<SSEEvent>> queue, std::string_view api_endpoint) {
  curl_easy_setopt(handle, CURLOPT_URL, api_endpoint.data());
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  curl_easy_setopt(
      handle,
      CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>([](char *ptr, size_t size, size_t nmemb, void *tsqueue) {
        std::string data{ptr, size * nmemb};
        logs::log(logs::debug, "[CURL] Received: {}", data);

        auto ts_queue = static_cast<TSQueue<SSEEvent> *>(tsqueue);
        auto lines = utils::split(data, '\n');
        if (lines.size() >= 2 && lines[0].starts_with("event: ") && lines[1].starts_with("data: ")) {
          ts_queue->push(SSEEvent{.event = std::string(lines[0].substr(7)), .data = std::string(lines[1].substr(6))});
        } else {
          logs::log(logs::warning, "[CURL] Invalid SSE event: {}", data);
        }

        return size * nmemb;
      }));
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, reinterpret_cast<void *>(queue.get()));

  curl_easy_perform(handle);
}

TEST_CASE("SSE APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the server to start

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());

  auto queue = std::make_shared<TSQueue<SSEEvent>>();

  std::thread sse_thread(listen_sse, curl.get(), queue, "http://localhost/api/v1/events");
  sse_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the SSE client to start

  // Test out one of the events
  event_bus->fire_event(events::IDRRequestEvent{.session_id = 42});

  auto event = queue->pop();
  REQUIRE(event.has_value());
  REQUIRE_THAT(event->event, Equals("wolf::core::events::IDRRequestEvent"));
  REQUIRE_THAT(event->data, Equals("{\"session_id\":\"42\"}"));
}

TEST_CASE("Dashboard reports retained apps without confusing owners and viewers", "[dashboard]") {
  auto sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>();
  auto profiles = std::make_shared<immer::atom<state::ProfilesList>>();
  profiles->update([](const auto &current) {
    return current.push_back(immer::box<events::Profile>{
        events::Profile{.id = "owner", .name = "Owner", .pin = std::vector<short>{1, 2, 3, 4}}});
  });
  state::AppState state{.config = immer::box<state::Config>{state::Config{.profiles = profiles}},
                        .event_bus = std::make_shared<events::EventBusType>(),
                        .lobbies = lobbies,
                        .running_sessions = sessions};
  auto route = std::make_shared<events::GpuStreamRoute>();
  route->streaming.store(true);
  route->codec.store(1);
  route->profile_id.store(std::make_shared<const std::string>("viewer"));
  route->negotiated_video.store(std::make_shared<const events::VideoSession>(
      events::VideoSession{.display_mode = {.width = 1920, .height = 1080, .refreshRate = 60}, .bitrate_kbps = 20000}));
  events::StreamSession session{.app = std::make_shared<events::App>(events::App{.base = {.title = "Wolf UI"}}),
                                .gpu_route = route,
                                .aes_key = "SECRET_AES_KEY",
                                .aes_iv = "SECRET_AES_IV",
                                .session_id = 42,
                                .ip = "PRIVATE_CLIENT_ADDRESS"};
  session.connected->store(true);
  session.monitoring->video.store(std::make_shared<const events::StreamVideoStatus>(
      events::StreamVideoStatus{.codec = "HEVC", .bitrate_kbps = 20000, .width = 1920, .height = 1080, .fps = 60}));
  sessions->update([&](const auto &current) { return current.push_back(session); });
  events::Lobby lobby{.id = "desktop", .name = "Plasma", .started_by_profile_id = "owner"};
  lobby.connected_sessions->update(
      [](const auto &current) { return current.push_back(immer::box<std::string>{"42"}); });
  lobbies->update([&](const auto &current) { return current.push_back(lobby); });
  state.active_containers->update([](const auto &current) {
    return current.set("container",
                       events::DockerContainerCreated{.container_id = "container",
                                                      .session_id = "desktop",
                                                      .name = "WolfPlasma_desktop",
                                                      .image = "plasma:test",
                                                      .render_node = "/dev/dri/renderD130"});
  });
  std::vector<wolf::gpu::Device> devices{
      {.id = "amd", .render_node = "/dev/dri/renderD130", .name = "AMD", .gpu_percent = 0}};
  auto status = make_status(state, devices);
  REQUIRE(status.containers.size() == 1);
  REQUIRE(status.containers[0].profile_id == "owner");
  REQUIRE(status.containers[0].gpu_id == "amd");
  REQUIRE(status.sessions[0].profile_id == "viewer");
  REQUIRE(status.sessions[0].app == "Plasma");
  REQUIRE(status.sessions[0].connected == true);
  REQUIRE(status.sessions[0].codec == "HEVC");
  REQUIRE(status.sessions[0].bitrate_kbps == 20000);
  REQUIRE(status.sessions[0].width == 1920);
  REQUIRE(status.gpus[0].gpu_percent == 0);
  REQUIRE_FALSE(status.gpus[0].encoder_percent);
  auto json = rfl::json::write(status);
  REQUIRE(json.find("SECRET_") == std::string::npos);
  REQUIRE(json.find("PRIVATE_CLIENT") == std::string::npos);
  REQUIRE(json.find("\"pin\"") == std::string::npos);

  session.connected->store(false); // The video thread may still be tearing down.
  status = make_status(state, devices);
  REQUIRE_FALSE(status.sessions[0].streaming);
  REQUIRE_FALSE(status.sessions[0].codec);
  REQUIRE_FALSE(status.sessions[0].bitrate_kbps);
  REQUIRE_FALSE(status.sessions[0].width);
  REQUIRE(status.containers.size() == 1);

  sessions->store({}); // The retained desktop remains visible after the client session disappears.
  status = make_status(state, devices);
  REQUIRE(status.sessions.empty());
  REQUIRE(status.containers[0].profile_id == "owner");
}

TEST_CASE("Dashboard distinguishes CPU sockets, GPU locality and applied CPU masks", "[dashboard][numa]") {
  auto sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  state::AppState state{.config = immer::box<state::Config>{state::Config{
                            .profiles = std::make_shared<immer::atom<state::ProfilesList>>()}},
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = sessions};
  // A socket can contain multiple NUMA nodes; their IDs are not interchangeable.
  StatusTopology topology{.host = {.cpus = {{.id = 2, .name = "CPU A", .cpus = {0, 1, 2, 3}, .numa_nodes = {0, 1}},
                                            {.id = 8, .name = "CPU B", .cpus = {4, 5}, .numa_nodes = {4}}},
                                   .node_cpus = {{0, {0, 1}}, {1, {2, 3}}, {4, {4, 5}}}},
                          .gpus = {{"local-gpu", {.numa_node = 4, .cpus = {4, 5}, .cpu_ids = {8}}}}};
  std::vector<wolf::gpu::Device> devices{{.id = "local-gpu", .render_node = "/dev/dri/renderD130"},
                                         {.id = "unknown-gpu", .render_node = "/dev/dri/renderD131"}};
  events::StreamSession session{.app = std::make_shared<events::App>(events::App{.base = {.title = "Desktop"}}),
                                .session_id = 42};
  session.connected->store(true);
  auto video = events::StreamVideoStatus{.codec = "HEVC",
                                         .bitrate_kbps = 12000,
                                         .width = 1920,
                                         .height = 1080,
                                         .fps = 60,
                                         .gpu_id = "local-gpu"};
  session.monitoring->video.store(std::make_shared<const events::StreamVideoStatus>(video));
  sessions->update([&](const auto &current) { return current.push_back(session); });
  state.active_containers->update([](const auto &current) {
    return current.set("app",
                       events::DockerContainerCreated{.container_id = "app",
                                                      .session_id = "42",
                                                      .name = "Desktop_42",
                                                      .render_node = "/dev/dri/renderD130",
                                                      .cpus = {5}});
  });
  auto status = make_status(state, devices, topology);
  REQUIRE(status.version == 1);
  REQUIRE(status.cpus.size() == 2);
  REQUIRE(status.cpus[0].id == 2);
  REQUIRE(status.cpus[0].numa_nodes == std::vector<unsigned>{0, 1});
  REQUIRE_FALSE(status.cpus[0].usage_percent);
  REQUIRE(status.gpus[0].cpu_ids == std::vector<int>{8});
  REQUIRE(status.gpus[0].numa_node == 4);
  REQUIRE(status.gpus[0].local_cpus == std::vector<unsigned>{4, 5});
  REQUIRE(status.gpus[1].cpu_ids.empty());
  REQUIRE_FALSE(status.gpus[1].numa_node);
  // A GPU assignment alone does not establish that CPU affinity was applied.
  REQUIRE_FALSE(status.sessions[0].placement);
  REQUIRE(status.containers[0].placement->cpus == std::vector<unsigned>{5});
  REQUIRE(status.containers[0].placement->cpu_ids == std::vector<int>{8});
  REQUIRE(status.containers[0].placement->numa_node == 4);

  // Actual worker telemetry takes precedence even when its mask differs from the GPU's local mask.
  video.cpus = {1};
  session.monitoring->video.store(std::make_shared<const events::StreamVideoStatus>(video));
  status = make_status(state, devices, topology);
  REQUIRE(status.sessions[0].placement->cpus == std::vector<unsigned>{1});
  REQUIRE(status.sessions[0].placement->cpu_ids == std::vector<int>{2});
  REQUIRE(status.sessions[0].placement->numa_node == 0);
  session.connected->store(false);
  REQUIRE_FALSE(make_status(state, devices, topology).sessions[0].placement);

  // Automatic guest fallback can span sockets; do not relabel it as GPU-local placement.
  state.active_containers->update([](const auto &current) {
    auto container = current.at("app");
    container.cpus = {0, 5};
    return current.set("app", container);
  });
  status = make_status(state, devices, topology);
  REQUIRE(status.containers[0].placement->cpu_ids == std::vector<int>{2, 8});
  REQUIRE_FALSE(status.containers[0].placement->numa_node);
  auto json = rfl::json::write(status);
  REQUIRE(json.find("\"cpu_ids\":[2,8]") != std::string::npos);
  REQUIRE(json.find("\"cpus\":[0,5]") != std::string::npos);

  // Device-wide utilization is additive; zero must survive serialization while
  // unavailable measurements stay optional. Topology and placements are unchanged.
  status = make_status(state, devices, topology, {{2, 0.0}, {8, 42.5}});
  REQUIRE(status.cpus[0].usage_percent == 0.0);
  REQUIRE(status.cpus[1].usage_percent == 42.5);
  json = rfl::json::write(status);
  REQUIRE(json.find("\"usage_percent\":0") != std::string::npos);
  REQUIRE(json.find("\"usage_percent\":42.5") != std::string::npos);
  status = make_status(state, devices, topology, {{2, std::nullopt}});
  REQUIRE_FALSE(status.cpus[0].usage_percent);
  REQUIRE_FALSE(status.cpus[1].usage_percent);

  status = make_status(state,
                       devices,
                       topology,
                       {},
                       {{.id = 0, .cpu_ids = {2}, .total_bytes = 4096, .used_bytes = 1024, .used_percent = 25.0},
                        {.id = 1, .cpu_ids = {2}, .total_bytes = 8192, .used_bytes = 0, .used_percent = 0.0},
                        {.id = 9, .cpu_ids = {2, 8}}});
  REQUIRE(status.memory_nodes.size() == 3);
  REQUIRE(status.memory_nodes[0].total_bytes == 4096);
  REQUIRE(status.memory_nodes[1].used_bytes == 0);
  REQUIRE(status.memory_nodes[2].cpu_ids == std::vector<int>{2, 8});
  REQUIRE_FALSE(status.memory_nodes[2].total_bytes);
  json = rfl::json::write(status);
  REQUIRE(json.find("\"memory_nodes\":[") != std::string::npos);
  // Wolf's size_t reflector also covers uint64_t on 64-bit platforms and writes strings.
  REQUIRE(
      (json.find("\"used_bytes\":0") != std::string::npos || json.find("\"used_bytes\":\"0\"") != std::string::npos));
  REQUIRE(json.find("\"used_percent\":25") != std::string::npos);
}

TEST_CASE("Dashboard reports container RAM residency only against known current placement", "[dashboard][numa]") {
  state::AppState state{.config = immer::box<state::Config>{state::Config{
                            .profiles = std::make_shared<immer::atom<state::ProfilesList>>()}},
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()};
  StatusTopology topology{.host = {.cpus = {{.id = 2, .name = "CPU", .cpus = {0, 1}, .numa_nodes = {0, 2}}},
                                   .node_cpus = {{0, {0}}, {2, {1}}}}};
  state.active_containers->update([](const auto &current) {
    return current.set("app", events::DockerContainerCreated{.container_id = "app", .name = "Desktop", .cpus = {0}});
  });
  wolf::numa::ContainerMemories samples{{"app", {.local_nodes = {0}, .anon = {4096, 0}, .file = {1024, 2048}}}};
  auto status = make_status(state, {}, topology, {}, {}, samples);
  REQUIRE(status.version == 1);
  REQUIRE(status.containers[0].memory);
  REQUIRE(status.containers[0].memory->local_nodes == std::vector<int>{0});
  REQUIRE(status.containers[0].memory->anon.local_bytes == 4096);
  REQUIRE(status.containers[0].memory->anon.remote_bytes == 0);
  REQUIRE(status.containers[0].memory->file.remote_bytes == 2048);
  auto json = rfl::json::write(status);
  REQUIRE(json.find("\"memory\":{") != std::string::npos);
  REQUIRE(json.find("\"local_nodes\":[0]") != std::string::npos);
  REQUIRE((json.find("\"remote_bytes\":0") != std::string::npos ||
           json.find("\"remote_bytes\":\"0\"") != std::string::npos));
  REQUIRE_FALSE(make_status(state, {}, topology).containers[0].memory);
  state.active_containers->update([](const auto &current) {
    auto container = current.at("app");
    container.cpus = {0, 1};
    return current.set("app", container);
  });
  REQUIRE_FALSE(make_status(state, {}, topology, {}, {}, samples).containers[0].memory);
  samples.at("app").local_nodes = {0, 2};
  REQUIRE(make_status(state, {}, topology, {}, {}, samples).containers[0].memory);
  state.active_containers->update([](const auto &current) {
    auto container = current.at("app");
    container.cpus = {0, 99};
    return current.set("app", container);
  });
  REQUIRE_FALSE(make_status(state, {}, topology, {}, {}, samples).containers[0].memory);
  state.active_containers->update([](const auto &current) { return current.erase("app"); });
  REQUIRE(make_status(state, {}, topology, {}, {}, samples).containers.empty());
}

TEST_CASE("Dashboard follows container lifecycle events", "[dashboard]") {
  auto app_state = immer::box<state::AppState>(
      state::AppState{.event_bus = std::make_shared<events::EventBusType>(),
                      .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()});
  auto handlers = sessions::setup_moonlight_handlers(app_state, "/tmp", {});
  app_state->event_bus->fire_event(
      immer::box<events::DockerContainerCreated>{events::DockerContainerCreated{.container_id = "app",
                                                                                .session_id = "42",
                                                                                .name = "Steam",
                                                                                .render_node = "/dev/dri/renderD128"}});
  REQUIRE(app_state->active_containers->load()->size() == 1);
  app_state->event_bus->fire_event(
      immer::box<events::DockerContainerStopped>{events::DockerContainerStopped{.container_id = "app"}});
  REQUIRE(app_state->active_containers->load()->empty());
}

namespace {
struct DashboardVideoWorker {
  std::shared_ptr<events::EventBusType> bus = std::make_shared<events::EventBusType>();
  std::jthread worker;
  explicit DashboardVideoWorker(events::VideoSession settings, std::vector<unsigned> cpus = {}) {
    worker = std::jthread([this, settings, cpus](std::stop_token stop) {
      std::optional<wolf::numa::Placement> placement;
      if (!cpus.empty())
        placement = wolf::numa::Placement{.cpus = cpus};
      wolf::numa::ThreadScope affinity(placement);
      // Repeat cancellation to also cover startup before its event handlers are installed.
      std::jthread cancellation([this, stop, id = settings.session_id](std::stop_token done) {
        while (!done.stop_requested()) {
          if (stop.stop_requested())
            bus->fire_event(immer::box<events::StopStreamEvent>{events::StopStreamEvent{id}});
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      });
      boost::asio::io_context io;
      auto socket = std::make_shared<streaming::udp::socket>(io, streaming::udp::endpoint(streaming::udp::v4(), 0));
      streaming::start_streaming_video(immer::box<events::VideoSession>(settings),
                                       bus,
                                       "127.0.0.1",
                                       12345,
                                       std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>(),
                                       socket);
    });
  }
  void stop() {
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
  }
  ~DashboardVideoWorker() {
    stop();
  }
};
template <typename F> bool dashboard_eventually(F ready) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}
} // namespace

TEST_CASE("Dashboard monitors route-less video workers and clears retiring streams", "[dashboard]") {
  auto sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  state::AppState state{.config = immer::box<state::Config>{state::Config{
                            .profiles = std::make_shared<immer::atom<state::ProfilesList>>()}},
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = sessions};
  events::StreamSession session{.app = std::make_shared<events::App>(events::App{.base = {.title = "Desktop"}}),
                                .session_id = 42};
  session.connected->store(true);
  session.monitoring->profile_id.store(std::make_shared<const std::string>("manual-user"));
  sessions->update([&](const auto &current) { return current.push_back(session); });
  const std::string render_node = "/dev/dri/wolf-dashboard-test-no-gpu";
  std::vector<wolf::gpu::Device> devices{{.id = "manual-gpu", .render_node = render_node}};
  REQUIRE_FALSE(make_status(state, devices).sessions[0].streaming);
  events::VideoSession settings{.monitoring = session.monitoring,
                                .monitoring_codec = std::string("HEVC"),
                                .display_mode = {.width = 1920, .height = 1080, .refreshRate = 60},
                                .gst_pipeline = "videotestsrc is-live=true ! fakesink sync=true",
                                .render_node = render_node,
                                .session_id = 42,
                                .packet_size = 1392,
                                .bitrate_kbps = 12000};
  REQUIRE_FALSE(settings.gpu_route.get());
  REQUIRE_FALSE(make_status(state, devices).sessions[0].streaming); // Negotiation is not a running worker.
  DashboardVideoWorker first(settings);
  REQUIRE(dashboard_eventually([&] { return session.monitoring->video.load() != nullptr; }));
  auto status = make_status(state, devices);
  REQUIRE(status.sessions[0].streaming);
  REQUIRE(status.sessions[0].codec == "HEVC");
  REQUIRE(status.sessions[0].bitrate_kbps == 12000);
  REQUIRE(status.sessions[0].width == 1920);
  REQUIRE(status.sessions[0].height == 1080);
  REQUIRE(status.sessions[0].fps == 60);
  REQUIRE(status.sessions[0].gpu_id == "manual-gpu");
  REQUIRE(status.sessions[0].profile_id == "manual-user");

  session.connected->store(false); // A retiring worker must not present active negotiation.
  status = make_status(state, devices);
  REQUIRE_FALSE(status.sessions[0].streaming);
  REQUIRE_FALSE(status.sessions[0].codec);
  REQUIRE_FALSE(status.sessions[0].bitrate_kbps);
  REQUIRE_FALSE(status.sessions[0].width);
  session.connected->store(true);

  auto old_snapshot = session.monitoring->video.load();
  DashboardVideoWorker replacement(settings);
  REQUIRE(dashboard_eventually([&] { return session.monitoring->video.load() != old_snapshot; }));
  auto replacement_snapshot = session.monitoring->video.load();
  first.stop();
  REQUIRE(session.monitoring->video.load() == replacement_snapshot);
  REQUIRE(make_status(state, devices).sessions[0].streaming);
  replacement.stop();
  REQUIRE_FALSE(session.monitoring->video.load());
  status = make_status(state, devices);
  REQUIRE_FALSE(status.sessions[0].streaming);
  REQUIRE_FALSE(status.sessions[0].codec);
  REQUIRE_FALSE(status.sessions[0].bitrate_kbps);
#ifdef __linux__
  // A real worker reports its eligible mask even without GPU-local placement.
  std::ifstream process_status("/proc/thread-self/status");
  std::string line;
  std::vector<unsigned> allowed_cpus;
  while (std::getline(process_status, line))
    if (line.starts_with("Cpus_allowed_list:"))
      allowed_cpus = wolf::numa::parse_list(std::string_view(line).substr(18));
  REQUIRE_FALSE(allowed_cpus.empty());
  DashboardVideoWorker pinned(settings, {allowed_cpus.front()});
  REQUIRE(dashboard_eventually([&] { return session.monitoring->video.load() != nullptr; }));
  status = make_status(state, devices);
  REQUIRE(status.sessions[0].placement);
  REQUIRE(status.sessions[0].placement->cpus == std::vector<unsigned>{allowed_cpus.front()});
  pinned.stop();
  REQUIRE_FALSE(make_status(state, devices).sessions[0].placement);
#endif
}

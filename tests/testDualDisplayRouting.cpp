#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <control/input_handler.hpp>
#include <map>
#include <rest/dual-app.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

using namespace wolf::core;
using namespace std::chrono_literals;

namespace {
// A real GStreamer source pad supplies negotiated producer geometry. Its event
// endpoint records exactly what the production WaylandMouse sends, without a
// compositor, GPU, or virtual input device.
struct PointerCanvas {
  std::vector<std::pair<double, double>> points;
  gstreamer::gst_element_ptr source;
  virtual_display::wl_state_ptr wayland;

  PointerCanvas(int width = 0, int height = 0) {
    static const auto source_type = [] {
      GTypeInfo info{};
      info.class_size = sizeof(GstElementClass);
      info.instance_size = sizeof(GstElement);
      info.class_init = [](gpointer klass, gpointer) {
        GST_ELEMENT_CLASS(klass)->send_event = [](GstElement *element, GstEvent *event) -> gboolean {
          const auto structure = gst_event_get_structure(event);
          if (structure && gst_structure_has_name(structure, "MouseMoveAbsolute")) {
            double x = 0, y = 0;
            if (gst_structure_get_double(structure, "pointer_x", &x) &&
                gst_structure_get_double(structure, "pointer_y", &y)) {
              auto points = static_cast<std::vector<std::pair<double, double>> *>(
                  g_object_get_data(G_OBJECT(element), "pointer-points"));
              points->emplace_back(x, y);
            }
          }
          gst_event_unref(event);
          return true;
        };
      };
      return g_type_register_static(GST_TYPE_ELEMENT, "WolfPointerCoordinateSource", &info, GTypeFlags(0));
    }();
    source = {GST_ELEMENT(g_object_new(source_type, nullptr)), gst_object_unref};
    g_object_set_data(G_OBJECT(source.get()), "pointer-points", &points);
    auto pad = gst_pad_new("src", GST_PAD_SRC);
    REQUIRE(gst_element_add_pad(source.get(), pad));
    REQUIRE(gst_pad_set_active(pad, true));
    if (width > 0 && height > 0) {
      auto caps = gst_caps_new_simple("video/x-raw",
                                      "width",
                                      G_TYPE_INT,
                                      width,
                                      "height",
                                      G_TYPE_INT,
                                      height,
                                      "format",
                                      G_TYPE_STRING,
                                      "RGBx",
                                      nullptr);
      auto event = gst_event_new_caps(caps);
      REQUIRE(gst_pad_store_sticky_event(pad, event) == GST_FLOW_OK);
      gst_event_unref(event);
      gst_caps_unref(caps);
    }
    wayland = virtual_display::create_wayland_display(source, "pointer-test");
  }

  ~PointerCanvas() {
    auto pad = std::shared_ptr<GstPad>(gst_element_get_static_pad(source.get(), "src"), gst_object_unref);
    gst_pad_set_active(pad.get(), false);
  }
};

bool wait_ready(const std::function<bool()> &ready) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

TEST_CASE("Wayland absolute input follows producer geometry instead of encoded display size", "[dual-display][input]") {
  SECTION("smaller secondary stream reaches the complete shared launcher") {
    PointerCanvas canvas(1920, 1080);
    virtual_display::WaylandMouse mouse(canvas.wayland);
    mouse.move_abs(640, 360, 1280, 720);
    REQUIRE(canvas.points.size() == 1);
    CHECK(canvas.points.back().first == Catch::Approx(960));
    CHECK(canvas.points.back().second == Catch::Approx(540));
  }
  SECTION("a reassigned app uses its own producer geometry") {
    PointerCanvas launcher(1920, 1080), app(640, 360);
    auto mouse = std::make_shared<std::optional<events::MouseTypes>>();
    mouse->emplace(virtual_display::WaylandMouse(launcher.wayland));
    std::visit([](auto &m) { m.move_abs(640, 360, 1280, 720); }, **mouse);
    mouse->emplace(virtual_display::WaylandMouse(app.wayland));
    std::visit([](auto &m) { m.move_abs(640, 360, 1280, 720); }, **mouse);
    REQUIRE(launcher.points.size() == 1);
    REQUIRE(app.points.size() == 1);
    CHECK(launcher.points.back().first == Catch::Approx(960));
    CHECK(launcher.points.back().second == Catch::Approx(540));
    CHECK(app.points.back().first == Catch::Approx(320));
    CHECK(app.points.back().second == Catch::Approx(180));
  }
  SECTION("ordinary one-to-one positions remain unchanged") {
    PointerCanvas canvas(1920, 1080);
    virtual_display::WaylandMouse mouse(canvas.wayland);
    mouse.move_abs(1234, 789, 1920, 1080);
    REQUIRE(canvas.points.size() == 1);
    CHECK(canvas.points.back().first == Catch::Approx(1234));
    CHECK(canvas.points.back().second == Catch::Approx(789));
  }
  SECTION("no negotiated caps or invalid reference sizes preserve fallback pixels") {
    PointerCanvas unnegotiated, ready(1920, 1080);
    virtual_display::WaylandMouse first(unnegotiated.wayland), second(ready.wayland);
    first.move_abs(321, 123, 1280, 720);
    second.move_abs(321, 123, 0, 0);
    REQUIRE(unnegotiated.points.size() == 1);
    REQUIRE(ready.points.size() == 1);
    CHECK(unnegotiated.points.back().first == Catch::Approx(321));
    CHECK(unnegotiated.points.back().second == Catch::Approx(123));
    CHECK(ready.points.back().first == Catch::Approx(321));
    CHECK(ready.points.back().second == Catch::Approx(123));
  }
}

TEST_CASE("Dual monitor absolute input retains its canvas offset", "[dual-display][input]") {
  for (const auto layout : {"horizontal", "vertical"}) {
    CAPTURE(layout);
    auto dual = std::make_shared<events::DualDisplayState>();
    dual->config = moonlight::dual_display::parse({{"gd_version", "1"},
                                                   {"gd_mode", "dual_monitor"},
                                                   {"gd_layout", layout},
                                                   {"gd_width2", "1280"},
                                                   {"gd_height2", "720"},
                                                   {"gd_fps2", "30"}},
                                                  1920,
                                                  1080,
                                                  60);
    const auto &display = dual->config.displays[1];
    PointerCanvas canvas(dual->config.canvas_width(), dual->config.canvas_height());
    events::StreamSession view{};
    view.display_mode = {.width = 1280, .height = 720};
    view.dual_display = dual;
    view.input_scoped = true;
    view.input_display_id = 1;
    view.mouse->emplace(virtual_display::WaylandMouse(canvas.wayland));
    control::MOUSE_MOVE_ABS_PACKET packet{};
    packet.x = boost::endian::native_to_big<short>(640);
    packet.y = boost::endian::native_to_big<short>(360);
    packet.width = boost::endian::native_to_big<short>(1280);
    packet.height = boost::endian::native_to_big<short>(720);
    control::mouse_move_abs(packet, view);
    REQUIRE(canvas.points.size() == 1);
    CHECK(canvas.points.back().first == Catch::Approx(display.x + 640));
    CHECK(canvas.points.back().second == Catch::Approx(display.y + 360));
  }
}

// No GPU, compositor, input device, network listener, or container is created.
// The real runtime receives a synthetic cached probe, and Wayland input wrappers
// retain an inert plugin handle. Production event handlers own every transition.
struct RoutingFixture {
  state::PairedClient picker_client{.client_cert = "picker-test-client"};
  std::shared_ptr<events::EventBusType> bus = std::make_shared<events::EventBusType>();
  wolf::gpu::Device device{
      .id = "dual-test-gpu", .render_node = "/test/renderD128", .driver = "amdgpu", .accessible = true};
  std::shared_ptr<wolf::gpu::Runtime> runtime;
  immer::box<state::AppState> app_state;
  events::StreamSession session{};
  immer::vector<immer::box<events::EventBusHandlers>> handlers;
  std::map<std::shared_ptr<wolf::gpu::Admission::Reservation>, std::uint64_t> encoder_epochs;

  void settle_encoders() {
    const auto routing = session.dual_display->routing.load();
    for (auto current = encoder_epochs.begin(); current != encoder_epochs.end();) {
      if (routing->slots[0].target->launch->reservation == current->first ||
          routing->slots[1].target->launch->reservation == current->first) {
        ++current;
      } else {
        current->first->end_encoder(current->second);
        current = encoder_epochs.erase(current);
      }
    }
    for (const auto &slot : routing->slots) {
      const auto reservation = slot.target->launch->reservation;
      if (encoder_epochs.count(reservation))
        continue;
      REQUIRE(runtime->resume(*slot.target->launch).empty());
      const auto epoch = reservation->begin_encoder();
      REQUIRE(epoch != 0);
      reservation->encoder_active(epoch);
      encoder_epochs.emplace(reservation, epoch);
    }
  }

  explicit RoutingFixture(const std::string &mode = "dual_app", bool picker = false) {
    runtime = std::make_shared<wolf::gpu::Runtime>(
        wolf::gpu::Options{},
        [gpu = device] { return std::vector{gpu}; },
        [] { return "dual-routing-test"; },
        [](const wolf::gpu::Device &gpu, wolf::gpu::Codec codec, std::stop_token) {
          if (codec != wolf::gpu::Codec::h264)
            return wolf::gpu::EncoderProbeResult{};
          return wolf::gpu::EncoderProbeResult{
              .encoder = wolf::gpu::EncoderBinding{"vah264enc", "va", codec, gpu.render_node, std::nullopt}};
        });
    REQUIRE(wait_ready([&] {
      const auto cached = runtime->capabilities(device);
      return cached && cached->status == wolf::gpu::CapabilityCache::Status::ready;
    }));
    state::Config config{};
    config.profiles = std::make_shared<immer::atom<state::ProfilesList>>();
    config.gpu_video.default_source = "interpipesrc listen-to={session_id}_video";
    config.gpu_video.default_sink = "rtpmoonlightpay_video name=moonlight_pay ! fakesink";
    config.gpu_video.h264_encoders.push_back({.plugin_name = "va", .encoder_pipeline = "vah264enc ! h264parse"});
    app_state = immer::box<state::AppState>(
        state::AppState{.config = immer::box<state::Config>(config),
                        .event_bus = bus,
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(),
                        .gpu_runtime = runtime});
    session.session_id = picker ? state::get_client_id(picker_client) : 7;
    session.event_bus = bus;
    session.app = std::make_shared<events::App>(
        events::App{.base = {.title = "Routing fixture", .id = "fixture-launcher", .support_hdr = false},
                    .gpu_auto_select = true});
    session.display_mode = {.width = 640, .height = 480, .refreshRate = 60};
    session.audio_channel_count = 2;
    session.dual_display = std::make_shared<events::DualDisplayState>();
    session.dual_display->picker_token = "picker-epoch-one";
    session.dual_display->config = moonlight::dual_display::parse({{"gd_version", "1"},
                                                                   {"gd_mode", mode},
                                                                   {"gd_layout", "vertical"},
                                                                   {"gd_primary", "0"},
                                                                   {"gd_width2", "320"},
                                                                   {"gd_height2", "240"},
                                                                   {"gd_fps2", "30"}},
                                                                  640,
                                                                  480,
                                                                  60);
    session.wayland_display->store(virtual_display::create_wayland_display({}, "fixture-launcher"));
    state::prepare_gpu_session(*app_state, session);
    state::prepare_dual_session(*app_state, session);
    // A ready transport has encoded buffers on both home tracks. Exercise
    // production admission without creating actual hardware encoders.
    settle_encoders();
    session.dual_display->negotiated = true;
    session.dual_display->streaming = true;
    session.connected->store(true);
    app_state->running_sessions->update([&](auto sessions) { return sessions.push_back(session); });
    handlers = sessions::setup_lobbies_handlers(app_state, "/tmp", {});
  }

  ~RoutingFixture() {
    for (const auto &[reservation, epoch] : encoder_epochs)
      reservation->end_encoder(epoch);
  }

  events::Lobby
  app(const std::string &id, bool shared = false, const std::string &owner = "alice", const std::string &folder = {}) {
    const auto held = runtime->retain("app:" + id, *session.gpu_route->home->launch);
    REQUIRE(held.launch);
    const auto target = state::gpu_target(*app_state, held.launch, id);
    events::Lobby lobby{.id = id,
                        .name = id,
                        .started_by_profile_id = owner,
                        .runner_state_folder = folder,
                        .gpu_target = target,
                        .multi_user = shared,
                        .stop_when_everyone_leaves = false};
    lobby.wayland_display->store(virtual_display::create_wayland_display({}, "fixture-" + id));
    if (session.dual_display->config.mode == "dual_monitor")
      lobby.display_config = session.dual_display->config;
    app_state->lobbies->update([&](auto lobbies) { return lobbies.push_back(lobby); });
    return lobby;
  }

  std::string join(const std::string &id, std::optional<int> display, std::optional<std::string> profile = "alice") {
    events::JoinLobbyEvent request{.profile_id = std::move(profile),
                                   .lobby_id = id,
                                   .moonlight_session_id = session.session_id,
                                   .display_id = display};
    auto result = request.error_message.get()->get_future();
    bus->fire_event(immer::box<events::JoinLobbyEvent>(request));
    REQUIRE(result.wait_for(1s) == std::future_status::ready);
    auto error = result.get();
    if (error.empty())
      settle_encoders();
    return error;
  }

  void leave(const std::string &id) {
    bus->fire_event(immer::box<events::LeaveLobbyEvent>{events::LeaveLobbyEvent{id, session.session_id}});
    settle_encoders();
  }
};
} // namespace

TEST_CASE("Dual app lobby routing preserves the other display and membership", "[dual-display][routing]") {
  RoutingFixture fixture;
  auto first = fixture.app("first");
  auto second = fixture.app("second");
  REQUIRE(fixture.join("first", 1).empty());
  auto routing = fixture.session.dual_display->routing.load();
  REQUIRE(routing->slots[0].producer == "7");
  REQUIRE(routing->slots[1].producer == "first");
  REQUIRE(routing->active_display == 0);
  REQUIRE(first.connected_sessions->load()->size() == 1);
  const auto first_target = routing->slots[1].target;
  REQUIRE(fixture.join("second", 0).empty());
  routing = fixture.session.dual_display->routing.load();
  REQUIRE(routing->slots[0].producer == "second");
  REQUIRE(routing->slots[1].target == first_target);
  REQUIRE(second.connected_sessions->load()->size() == 1);
  REQUIRE(first.connected_sessions->load()->size() == 1);
  REQUIRE(fixture.join("second", 0).empty());
  REQUIRE(second.connected_sessions->load()->size() == 1);

  const auto second_target = routing->slots[0].target;
  fixture.leave("first");
  routing = fixture.session.dual_display->routing.load();
  REQUIRE(routing->slots[1].producer == "7");
  REQUIRE_FALSE(routing->slots[1].lobby_id);
  REQUIRE(routing->slots[1].target == fixture.session.dual_display->home[1]);
  REQUIRE(routing->slots[0].target == second_target);
  REQUIRE(routing->active_display == 1);
  REQUIRE(first.connected_sessions->load()->empty());
  REQUIRE(second.connected_sessions->load()->size() == 1);
}

TEST_CASE("Dual app replacement removes only the replaced lobby membership", "[dual-display][routing]") {
  RoutingFixture fixture;
  auto first = fixture.app("first");
  auto second = fixture.app("second");
  auto replacement = fixture.app("replacement");
  REQUIRE(fixture.join("first", 0).empty());
  REQUIRE(fixture.join("second", 1).empty());
  const auto unaffected = fixture.session.dual_display->routing.load()->slots[1].target;
  REQUIRE(fixture.join("replacement", 0).empty());
  REQUIRE(first.connected_sessions->load()->empty());
  REQUIRE(replacement.connected_sessions->load()->size() == 1);
  REQUIRE(second.connected_sessions->load()->size() == 1);
  REQUIRE(fixture.session.dual_display->routing.load()->slots[1].target == unaffected);
}

TEST_CASE("Dual display rejects unsafe joins without publishing partial routes", "[dual-display][routing]") {
  RoutingFixture fixture;
  auto lobby = fixture.app("private");
  const auto original = fixture.session.dual_display->routing.load();
  SECTION("different or missing private profile") {
    REQUIRE_FALSE(fixture.join("private", 0, "bob").empty());
    REQUIRE_FALSE(fixture.join("private", 0, std::nullopt).empty());
  }
  SECTION("unnegotiated and stopped video") {
    fixture.session.dual_display->negotiated = false;
    REQUIRE_FALSE(fixture.join("private", 0).empty());
    fixture.session.dual_display->negotiated = true;
    fixture.session.dual_display->streaming = false;
    REQUIRE_FALSE(fixture.join("private", 0).empty());
  }
  SECTION("invalid display target") {
    REQUIRE_FALSE(fixture.join("private", 2).empty());
    REQUIRE_FALSE(fixture.join("private", -1).empty());
  }
  SECTION("closing app") {
    lobby.gpu_target->launch->reservation->cancel();
    REQUIRE_FALSE(fixture.join("private", 0).empty());
  }
  SECTION("unsupported codec rolls back retained reservation") {
    fixture.session.gpu_route->codec = 2;
    REQUIRE_FALSE(fixture.join("private", 0).empty());
    REQUIRE(fixture.session.dual_display->routing.load() == original);
    fixture.session.gpu_route->codec = 0;
    REQUIRE(fixture.join("private", 0).empty());
    return;
  }
  REQUIRE(fixture.session.dual_display->routing.load() == original);
  REQUIRE(lobby.connected_sessions->load()->empty());
}

TEST_CASE("Dual monitor assigns both tracks to one desktop and leaves together", "[dual-display][routing]") {
  RoutingFixture fixture("dual_monitor");
  auto lobby = fixture.app("desktop");
  REQUIRE_FALSE(fixture.join("desktop", 1).empty());
  REQUIRE(fixture.join("desktop", std::nullopt).empty());
  auto routing = fixture.session.dual_display->routing.load();
  REQUIRE(routing->slots[0].lobby_id == "desktop");
  REQUIRE(routing->slots[1].lobby_id == "desktop");
  REQUIRE(routing->slots[0].target->launch->reservation != routing->slots[1].target->launch->reservation);
  REQUIRE(lobby.connected_sessions->load()->size() == 1);
  fixture.leave("desktop");
  routing = fixture.session.dual_display->routing.load();
  REQUIRE_FALSE(routing->slots[0].lobby_id);
  REQUIRE_FALSE(routing->slots[1].lobby_id);
  REQUIRE(lobby.connected_sessions->load()->empty());
}

TEST_CASE("Dual display disconnect detaches both apps without stopping persistent lobbies", "[dual-display][routing]") {
  RoutingFixture fixture;
  auto first = fixture.app("first");
  auto second = fixture.app("second");
  REQUIRE(fixture.join("first", 0).empty());
  REQUIRE(fixture.join("second", 1).empty());
  fixture.bus->fire_event(immer::box<events::PauseStreamEvent>{events::PauseStreamEvent{fixture.session.session_id}});
  const auto routing = fixture.session.dual_display->routing.load();
  REQUIRE_FALSE(routing->slots[0].lobby_id);
  REQUIRE_FALSE(routing->slots[1].lobby_id);
  REQUIRE(first.connected_sessions->load()->empty());
  REQUIRE(second.connected_sessions->load()->empty());
  REQUIRE(fixture.app_state->lobbies->load()->size() == 2);
  REQUIRE(first.gpu_target->launch->reservation->valid());
  REQUIRE(second.gpu_target->launch->reservation->valid());
}

TEST_CASE("Dual monitor crops raw canvas before conversion and encoding", "[dual-display][routing]") {
  events::VideoSession video{};
  video.session_id = 7;
  video.display_mode = {.width = 320, .height = 240, .refreshRate = 30};
  video.bitrate_kbps = 2000;
  video.gst_pipeline = "interpipesrc listen-to={session_id}_video ! videoconvertscale ! "
                       "video/x-raw,width={width},height={height} ! vah264enc ! "
                       "rtpmoonlightpay_video name=moonlight_pay ! fakesink";
  auto dual = std::make_shared<events::DualDisplayState>();
  dual->config = moonlight::dual_display::parse({{"gd_version", "1"},
                                                 {"gd_mode", "dual_monitor"},
                                                 {"gd_layout", "vertical"},
                                                 {"gd_width2", "320"},
                                                 {"gd_height2", "240"},
                                                 {"gd_fps2", "30"}},
                                                640,
                                                480,
                                                60);
  video.dual_display = dual;
  video.display_id = 1;
  video.rtp_ssrc = moonlight::dual_display::ssrc(1);
  const auto pipeline =
      streaming::format_video_pipeline(immer::box<events::VideoSession>(video), "127.0.0.1", 5000, "desktop");
  REQUIRE(pipeline.find("listen-to=desktop_video") != std::string::npos);
  const auto crop = pipeline.find("videocrop left=0 top=480 right=320 bottom=0");
  const auto encode = pipeline.find("vah264enc");
  const auto convert = pipeline.find("videoconvertscale");
  REQUIRE(crop != std::string::npos);
  REQUIRE(crop < convert);
  REQUIRE(convert < encode);
  REQUIRE(pipeline.find("framerate=30/1") < encode);
}

TEST_CASE("Dual video rate filtering parses capture caps and preserves memory features", "[dual-display][routing]") {
  // Parsing needs no buffers or GPU. A hardware capture source can put a bare
  // caps expression immediately after appsrc/interpipesrc, before conversion.
  for (const auto &capture_caps :
       {"video/x-raw,format=RGBx,width=640,height=480,framerate=60/1",
        "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=XR24,width=640,height=480,framerate=60/1",
        "video/x-raw(memory:CUDAMemory),format=RGBA,width=640,height=480,framerate=60/1"}) {
    for (const bool dual : {false, true}) {
      CAPTURE(capture_caps, dual);
      events::VideoSession video{};
      video.session_id = 7;
      video.display_mode = {.width = 640, .height = 480, .refreshRate = 60};
      video.bitrate_kbps = 2000;
      video.rtp_ssrc = dual ? moonlight::dual_display::ssrc(1) : 0;
      video.gst_pipeline = "appsrc name=capture ! " + std::string(capture_caps) + " ! fakesink";
      const auto description =
          streaming::format_video_pipeline(immer::box<events::VideoSession>(video), "127.0.0.1", 5000, "desktop");
      if (!dual)
        REQUIRE(description == video.gst_pipeline);
      INFO(description);
      GError *error = nullptr;
      auto pipeline = gstreamer::gst_element_ptr(gst_parse_launch(description.c_str(), &error), gst_object_unref);
      const auto message = error ? std::string(error->message) : std::string{};
      if (error)
        g_error_free(error);
      INFO(message);
      REQUIRE(message.empty());
      REQUIRE(pipeline);

      auto expected_capture = std::shared_ptr<GstCaps>(gst_caps_from_string(capture_caps), gst_caps_unref);
      auto expected_rate =
          std::shared_ptr<GstCaps>(gst_caps_from_string("video/x-raw(ANY),framerate=60/1"), gst_caps_unref);
      REQUIRE(expected_capture);
      REQUIRE(expected_rate);
      unsigned capture_filters = 0, rate_filters = 0;
      auto iterator = gst_bin_iterate_recurse(GST_BIN(pipeline.get()));
      GValue value = G_VALUE_INIT;
      while (gst_iterator_next(iterator, &value) == GST_ITERATOR_OK) {
        auto element = GST_ELEMENT(g_value_get_object(&value));
        auto factory = gst_element_get_factory(element);
        if (factory && std::string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))) == "capsfilter") {
          GstCaps *raw_caps = nullptr;
          g_object_get(element, "caps", &raw_caps, nullptr);
          auto caps = std::shared_ptr<GstCaps>(raw_caps, gst_caps_unref);
          if (caps && gst_caps_is_equal(caps.get(), expected_capture.get()))
            ++capture_filters;
          if (caps && gst_caps_is_equal(caps.get(), expected_rate.get())) {
            ++rate_filters;
            // Rate conversion must permit the original DMA-BUF/CUDA features,
            // instead of forcing a CPU-memory download during negotiation.
            CHECK(gst_caps_features_is_any(gst_caps_get_features(caps.get(), 0)));
          }
        }
        g_value_reset(&value);
      }
      g_value_unset(&value);
      gst_iterator_free(iterator);
      REQUIRE(capture_filters == 1);
      REQUIRE(rate_filters == (dual ? 1 : 0));
    }
  }
}

namespace {
struct PickerFixture : RoutingFixture {
  PickerFixture() : RoutingFixture("dual_app", true) {
    immer::vector<immer::box<events::App>> apps;
    for (const auto &name : {"First", "Second", "New"}) {
      auto app_runner = state::get_runner(
          wolf::config::AppDocker{.name = name, .image = "private-image-secret", .env = {"PRIVATE_ENV_SECRET=value"}},
          bus);
      apps = apps.push_back({events::App{.base = {.title = name, .id = name}, .runner = app_runner}});
    }
    app_state->config->profiles->update([&](auto profiles) {
      return profiles
          .push_back(
              {events::Profile{.id = "alice",
                               .name = "Alice",
                               .pin = std::vector<short>{1, 2, 3, 4},
                               .apps = std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(apps)}})
          .push_back(
              {events::Profile{.id = std::string(events::MOONLIGHT_PROFILE_ID),
                               .name = "Launcher",
                               .apps = std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(apps)}});
    });
  }

  wolf::app_picker::Reply call(wolf::app_picker::Operation op, const std::string &body = {}) {
    return wolf::app_picker::handle(op, picker_client, app_state, body);
  }

  wolf::app_picker::AssignRequest assignment(const std::string &app, int display = 0) {
    return {.version = 1,
            .session_token = session.dual_display->picker_token,
            .display_id = display,
            .expected_assignment = session.dual_display->routing.load()->slots[display].lobby_id.value_or("launcher"),
            .profile_id = "alice",
            .app_id = app,
            .profile_pin = std::vector<short>{1, 2, 3, 4}};
  }

  // Keep production join/leave handlers and substitute only external app startup.
  // The real CreateLobbyEvent remains the factory's observable contract.
  void replace_startup() {
    handlers = handlers | ranges::views::drop(1) | ranges::to<immer::vector<immer::box<events::EventBusHandlers>>>();
  }

  void publish(const events::CreateLobbyEvent &event) {
    events::Lobby lobby{.id = event.id,
                        .name = event.name,
                        .started_by_profile_id = event.profile_id,
                        .runner_state_folder = event.runner_state_folder,
                        .gpu_target = event.gpu_target.get(),
                        .multi_user = event.multi_user,
                        .pin = event.pin,
                        .stop_when_everyone_leaves = event.stop_when_everyone_leaves,
                        .runner = event.runner};
    lobby.wayland_display->store(virtual_display::create_wayland_display({}, "picker-ready"));
    app_state->lobbies->update([&](auto lobbies) { return lobbies.push_back(lobby); });
  }

  void replace_lobby(const events::Lobby &replacement) {
    app_state->lobbies->update([&](const auto &lobbies) {
      return lobbies | ranges::views::transform([&](const events::Lobby &current) {
               return current.id == replacement.id ? replacement : current;
             }) |
             ranges::to<immer::vector<events::Lobby>>();
    });
  }
};
} // namespace

TEST_CASE("Native app picker scopes catalog and access to the paired ready session", "[dual-display][picker]") {
  using namespace wolf::app_picker;
  PickerFixture fixture;
  const auto initial = fixture.call(Operation::state);
  REQUIRE(initial.body.success);
  REQUIRE(initial.body.state->profiles.size() == 1);
  CHECK(initial.body.state->profiles[0].id == "alice");
  CHECK(initial.body.state->profiles[0].pin_required);
  CHECK(initial.body.state->displays[1].width == 320);
  CHECK(initial.body.state->displays[1].fps == 30);
  CatalogRequest catalog{.version = 1, .session_token = "picker-epoch-one", .profile_id = "alice"};
  CHECK(fixture.call(Operation::catalog, rfl::json::write(catalog)).body.code == "pin_required");
  catalog.profile_pin = std::vector<short>{9};
  CHECK(fixture.call(Operation::catalog, rfl::json::write(catalog)).body.code == "invalid_pin");
  catalog.profile_pin = std::vector<short>{1, 2, 3, 4};
  const auto apps = fixture.call(Operation::catalog, rfl::json::write(catalog));
  REQUIRE(apps.body.success);
  REQUIRE(apps.body.apps->size() == 3);
  const auto json = rfl::json::write(apps.body);
  CHECK(json.find("PRIVATE_ENV_SECRET") == std::string::npos);
  CHECK(json.find("private-image-secret") == std::string::npos);
  CHECK(json.find("profile-data/") == std::string::npos);
  CHECK(json.find("\"profile_pin\"") == std::string::npos);
  CHECK(json.find("\"runner\"") == std::string::npos);
  catalog.profile_id = std::string(events::MOONLIGHT_PROFILE_ID);
  CHECK(fixture.call(Operation::catalog, rfl::json::write(catalog)).body.code == "not_found");
  const auto other =
      handle(Operation::state, state::PairedClient{.client_cert = "other-paired-client"}, fixture.app_state);
  CHECK_FALSE(other.body.success);
  auto request = fixture.assignment("Not saved");
  CHECK(fixture.call(Operation::assign, rfl::json::write(request)).body.code == "not_found");
  CHECK(fixture.call(Operation::assign, "{\"runner\":{}}").body.code == "invalid_request");
  auto extra = rfl::json::write(fixture.assignment("First"));
  extra.insert(1, "\"session_id\":\"another-client\",");
  CHECK(fixture.call(Operation::assign, extra).body.code == "invalid_request");
  for (const auto mode : {"off", "controls", "dual_monitor"}) {
    fixture.session.dual_display->config.mode = mode;
    CHECK(fixture.call(Operation::state).body.code == "unsupported");
  }
  fixture.session.dual_display->config.mode = "dual_app";
  fixture.session.dual_display->negotiated = false;
  CHECK(fixture.call(Operation::state).body.code == "unavailable");
}

TEST_CASE("Native assignments share Wolf-UI lobbies and preserve the other display", "[dual-display][picker]") {
  using namespace wolf::app_picker;
  PickerFixture fixture;
  auto first = fixture.app("ui-first", false, "alice", "profile-data/alice/First");
  auto second = fixture.app("ui-second", false, "alice", "profile-data/alice/Second");
  REQUIRE(fixture.call(Operation::assign, rfl::json::write(fixture.assignment("First", 0))).body.success);
  fixture.settle_encoders();
  const auto primary = fixture.session.dual_display->routing.load()->slots[0].target;
  const auto assigned = fixture.call(Operation::assign, rfl::json::write(fixture.assignment("Second", 1)));
  REQUIRE(assigned.body.success);
  fixture.settle_encoders();
  REQUIRE(assigned.body.state->displays[0].assignment_id == "ui-first");
  CHECK(assigned.body.state->displays[0].app_id == "First");
  CHECK(assigned.body.state->displays[1].app_name == "ui-second");
  CHECK(fixture.session.dual_display->routing.load()->slots[0].target == primary);
  CHECK(first.connected_sessions->load()->size() == 1);
  CHECK(second.connected_sessions->load()->size() == 1);
  CatalogRequest catalog{.version = 1,
                         .session_token = "picker-epoch-one",
                         .profile_id = "alice",
                         .profile_pin = std::vector<short>{1, 2, 3, 4}};
  auto apps = fixture.call(Operation::catalog, rfl::json::write(catalog));
  REQUIRE(apps.body.apps);
  CHECK(apps.body.apps->at(1).running);
  CHECK(apps.body.apps->at(1).assigned_display == 1);
  auto conflict = fixture.assignment("First", 1);
  CHECK(fixture.call(Operation::assign, rfl::json::write(conflict)).body.code == "conflict");
  conflict = fixture.assignment("First", 0);
  conflict.expected_assignment = "launcher";
  CHECK(fixture.call(Operation::assign, rfl::json::write(conflict)).body.code == "conflict");
  ClearRequest clear{.version = 1, .session_token = "stale-epoch", .display_id = 1, .expected_assignment = "ui-second"};
  CHECK(fixture.call(Operation::clear, rfl::json::write(clear)).body.code == "session_changed");
  clear.session_token = "picker-epoch-one";
  REQUIRE(fixture.call(Operation::clear, rfl::json::write(clear)).body.success);
  fixture.settle_encoders();
  CHECK(fixture.session.dual_display->routing.load()->slots[0].target == primary);
  CHECK_FALSE(fixture.session.dual_display->routing.load()->slots[1].lobby_id);
  CHECK(second.connected_sessions->load()->empty());
  CHECK(fixture.app_state->lobbies->load()->size() == 2);
  CHECK(second.gpu_target->launch->reservation->valid());
}

TEST_CASE("Native app factory starts only the saved persistent private app", "[dual-display][picker]") {
  using namespace wolf::app_picker;
  PickerFixture fixture;
  fixture.replace_startup();
  auto observer = fixture.bus->register_handler<immer::box<events::CreateLobbyEvent>>(
      [&](const immer::box<events::CreateLobbyEvent> &event) {
        CHECK(event->profile_id == "alice");
        CHECK(event->name == "New");
        CHECK(event->runner_state_folder == "profile-data/alice/New");
        CHECK_FALSE(event->multi_user);
        CHECK_FALSE(event->stop_when_everyone_leaves);
        CHECK(event->video_settings.width == 320);
        CHECK(event->video_settings.height == 240);
        CHECK(event->video_settings.refresh_rate == 30);
        CHECK(event->audio_settings.channel_count == 2);
        CHECK(event->client_settings.run_uid == fixture.session.client_settings->run_uid);
        const auto runner = rfl::get<wolf::config::AppDocker>(event->runner->serialize().variant());
        CHECK(runner.name == "New");
        CHECK(runner.image == "private-image-secret");
        fixture.publish(*event);
        event->on_setup_over.get()->set_value(true);
      });
  const auto result = fixture.call(Operation::assign, rfl::json::write(fixture.assignment("New", 1)));
  REQUIRE(result.body.success);
  CHECK(result.body.state->displays[1].app_id == "New");
  CHECK_FALSE(fixture.session.dual_display->routing.load()->slots[0].lobby_id);
  observer.unregister();
}

TEST_CASE("Native picker respects running app access and failed join keeps video routes", "[dual-display][picker]") {
  using namespace wolf::app_picker;
  PickerFixture fixture;
  auto first = fixture.app("first", false, "alice", "profile-data/alice/First");
  auto second = fixture.app("second", false, "alice", "profile-data/alice/Second");
  auto blocked = fixture.app("new", false, "alice", "profile-data/alice/New");
  first.pin = std::vector<short>{5, 6};
  fixture.replace_lobby(first);
  auto request = fixture.assignment("First", 0);
  CHECK(fixture.call(Operation::assign, rfl::json::write(request)).body.code == "lobby_pin_required");
  request.lobby_pin = std::vector<short>{9};
  CHECK(fixture.call(Operation::assign, rfl::json::write(request)).body.code == "invalid_lobby_pin");
  request.lobby_pin = std::vector<short>{5, 6};
  REQUIRE(fixture.call(Operation::assign, rfl::json::write(request)).body.success);
  fixture.settle_encoders();
  REQUIRE(fixture.call(Operation::assign, rfl::json::write(fixture.assignment("Second", 1))).body.success);
  fixture.settle_encoders();
  const auto original = fixture.session.dual_display->routing.load();
  blocked.connected_sessions->update([](auto viewers) { return viewers.push_back({"another-device"}); });
  request = fixture.assignment("New", 1);
  CHECK(fixture.call(Operation::assign, rfl::json::write(request)).body.code == "busy");
  const auto catalog = fixture.call(Operation::catalog,
                                    rfl::json::write(CatalogRequest{.version = 1,
                                                                    .session_token = "picker-epoch-one",
                                                                    .profile_id = "alice",
                                                                    .profile_pin = std::vector<short>{1, 2, 3, 4}}));
  REQUIRE(catalog.body.apps);
  CHECK_FALSE(catalog.body.apps->at(2).available);
  blocked.connected_sessions->update([](auto) { return immer::vector<immer::box<std::string>>{}; });
  // A closing app fails the production JoinLobbyEvent before publishing a new
  // route; the native endpoint must leave both encoded tracks assigned as-is.
  blocked.gpu_target->launch->reservation->cancel();
  CHECK(fixture.call(Operation::assign, rfl::json::write(request)).body.code == "failed");
  const auto after = fixture.session.dual_display->routing.load();
  CHECK(after->slots[0].target == original->slots[0].target);
  CHECK(after->slots[1].target == original->slots[1].target);
  CHECK(first.connected_sessions->load()->size() == 1);
  CHECK(second.connected_sessions->load()->size() == 1);
}

TEST_CASE("Failed or obsolete native app startup preserves both previous assignments", "[dual-display][picker]") {
  using namespace wolf::app_picker;
  for (const auto change : {"failure", "epoch", "target"}) {
    CAPTURE(change);
    PickerFixture fixture;
    fixture.replace_startup();
    auto first = fixture.app("first", false, "alice", "profile-data/alice/First");
    auto second = fixture.app("second", false, "alice", "profile-data/alice/Second");
    REQUIRE(fixture.join("first", 0).empty());
    REQUIRE(fixture.join("second", 1).empty());
    const auto original = fixture.session.dual_display->routing.load();
    auto request = fixture.assignment("New", 1);
    std::promise<immer::box<events::CreateLobbyEvent>> creating;
    auto notified = creating.get_future();
    auto observer = fixture.bus->register_handler<immer::box<events::CreateLobbyEvent>>(
        [&](const immer::box<events::CreateLobbyEvent> &event) {
          fixture.publish(*event);
          creating.set_value(event);
        });
    auto response =
        std::async(std::launch::async, [&] { return fixture.call(Operation::assign, rfl::json::write(request)); });
    REQUIRE(notified.wait_for(1s) == std::future_status::ready);
    auto event = notified.get();
    {
      std::lock_guard transition(*fixture.app_state->gpu_transitions);
      std::lock_guard picker(fixture.session.dual_display->picker_transitions);
      if (std::string(change) == "epoch") {
        fixture.session.dual_display->picker_token = "new-reconnected-epoch";
      } else if (std::string(change) == "target") {
        // Wolf-UI can change the selected slot while the compositor starts.
        fixture.leave("second");
      }
    }
    event->on_setup_over.get()->set_value(std::string(change) != "failure");
    REQUIRE(response.wait_for(1s) == std::future_status::ready);
    const auto result = response.get();
    CHECK_FALSE(result.body.success);
    CHECK(result.body.code == (std::string(change) == "failure" ? "failed"
                               : std::string(change) == "epoch" ? "session_changed"
                                                                : "conflict"));
    const auto after = fixture.session.dual_display->routing.load();
    CHECK(after->slots[0].target == original->slots[0].target);
    if (std::string(change) != "target")
      CHECK(after->slots[1].target == original->slots[1].target);
    else
      CHECK_FALSE(after->slots[1].lobby_id);
    CHECK(first.connected_sessions->load()->size() == 1);
    observer.unregister();
  }
}

TEST_CASE("Native picker waits for an existing Wolf-UI app to finish setup", "[dual-display][picker]") {
  using namespace wolf::app_picker;
  for (const bool stopped : {false, true}) {
    CAPTURE(stopped);
    PickerFixture fixture;
    auto first = fixture.app("first", false, "alice", "profile-data/alice/First");
    auto pending = fixture.app("ui-starting", false, "alice", "profile-data/alice/New");
    REQUIRE(fixture.join("first", 0).empty());
    const auto primary = fixture.session.dual_display->routing.load()->slots[0].target;
    pending.wayland_display->store(nullptr);
    auto request = fixture.assignment("New", 1);
    auto response =
        std::async(std::launch::async, [&] { return fixture.call(Operation::assign, rfl::json::write(request)); });
    REQUIRE(response.wait_for(30ms) == std::future_status::timeout);
    {
      // Acquiring these while the request waits proves startup does not block
      // Wolf-UI transitions or a reconnect's epoch change.
      std::lock_guard transition(*fixture.app_state->gpu_transitions);
      std::lock_guard picker(fixture.session.dual_display->picker_transitions);
      if (stopped)
        pending.gpu_target->launch->reservation->cancel();
      else
        pending.wayland_display->store(virtual_display::create_wayland_display({}, "ui-setup-ready"));
    }
    REQUIRE(response.wait_for(1s) == std::future_status::ready);
    const auto result = response.get();
    CHECK(result.body.success == !stopped);
    CHECK(fixture.session.dual_display->routing.load()->slots[0].target == primary);
    CHECK(fixture.app_state->lobbies->load()->size() == 2);
    if (stopped)
      CHECK_FALSE(fixture.session.dual_display->routing.load()->slots[1].lobby_id);
    else
      CHECK(fixture.session.dual_display->routing.load()->slots[1].lobby_id == pending.id);
  }
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <control/input_handler.hpp>
#include <map>
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

  explicit RoutingFixture(const std::string &mode = "dual_app") {
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
    config.gpu_video.default_source = "interpipesrc listen-to={session_id}_video";
    config.gpu_video.default_sink = "rtpmoonlightpay_video name=moonlight_pay ! fakesink";
    config.gpu_video.h264_encoders.push_back({.plugin_name = "va", .encoder_pipeline = "vah264enc ! h264parse"});
    app_state = immer::box<state::AppState>(
        state::AppState{.config = immer::box<state::Config>(config),
                        .event_bus = bus,
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(),
                        .gpu_runtime = runtime});
    session.session_id = 7;
    session.event_bus = bus;
    session.app = std::make_shared<events::App>(
        events::App{.base = {.title = "Routing fixture", .id = "fixture-launcher", .support_hdr = false},
                    .gpu_auto_select = true});
    session.display_mode = {.width = 640, .height = 480, .refreshRate = 60};
    session.dual_display = std::make_shared<events::DualDisplayState>();
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
    app_state->running_sessions->update([&](auto sessions) { return sessions.push_back(session); });
    handlers = sessions::setup_lobbies_handlers(app_state, "/tmp", {});
  }

  ~RoutingFixture() {
    for (const auto &[reservation, epoch] : encoder_epochs)
      reservation->end_encoder(epoch);
  }

  events::Lobby app(const std::string &id, bool shared = false, const std::string &owner = "alice") {
    const auto held = runtime->retain("app:" + id, *session.gpu_route->home->launch);
    REQUIRE(held.launch);
    const auto target = state::gpu_target(*app_state, held.launch, id);
    events::Lobby lobby{.id = id,
                        .name = id,
                        .started_by_profile_id = owner,
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

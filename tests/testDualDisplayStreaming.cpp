#include <catch2/catch_test_macros.hpp>
#include <future>
#include <set>
#include <streaming/streaming.hpp>

using namespace wolf::core;
using namespace std::chrono_literals;

namespace {
template <typename Predicate> bool wait_for_dual_stream(Predicate ready, std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (ready())
      return true;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

bool cpu_streaming_plugins_available() {
  for (const auto name : {"x264enc", "h264parse", "interpipesrc", "interpipesink", "videotestsrc"}) {
    auto factory = gst_element_factory_find(name);
    if (!factory)
      return false;
    gst_object_unref(factory);
  }
  return true;
}

// Only the cached GPU capability/admission inputs are synthetic. Capture, two
// CPU encoders, Moonlight packetizers, UDP transmission and teardown are real.
struct DualStreamingFixture {
  wolf::gpu::Device device{
      .id = "dual-cpu-encoder-fixture", .render_node = "/test/no-gpu", .driver = "amdgpu", .accessible = true};
  std::shared_ptr<wolf::gpu::Runtime> runtime;
  std::shared_ptr<events::EventBusType> bus = std::make_shared<events::EventBusType>();
  std::shared_ptr<events::DualDisplayState> dual = std::make_shared<events::DualDisplayState>();
  std::array<std::shared_ptr<events::GpuStreamTarget>, 2> targets;
  std::array<gstreamer::gst_element_ptr, 2> producers;
  boost::asio::io_context io;
  boost::asio::ip::udp::socket receiver{io, boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0)};
  std::shared_ptr<boost::asio::ip::udp::socket> sender = std::make_shared<boost::asio::ip::udp::socket>(
      io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
  events::VideoSession settings{};
  std::promise<void> finished;
  std::future<void> completion;
  std::jthread worker;
  std::exception_ptr error;

  DualStreamingFixture() {
    receiver.non_blocking(true);
    runtime = std::make_shared<wolf::gpu::Runtime>(
        wolf::gpu::Options{.use_zero_copy = false},
        [gpu = device] { return std::vector{gpu}; },
        [] { return "dual-real-streaming-regression"; },
        [](const wolf::gpu::Device &gpu, wolf::gpu::Codec codec, std::stop_token) {
          if (codec != wolf::gpu::Codec::h264)
            return wolf::gpu::EncoderProbeResult{};
          return wolf::gpu::EncoderProbeResult{
              .encoder = wolf::gpu::EncoderBinding{"x264enc", "x264", codec, gpu.render_node, std::nullopt}};
        });
    REQUIRE(wait_for_dual_stream([&] {
      const auto cached = runtime->capabilities(device);
      return cached && cached->status == wolf::gpu::CapabilityCache::Status::ready;
    }));
    const auto first = runtime->prepare("dual-real-streaming-primary", device.id);
    INFO(first.error);
    REQUIRE(first.launch);
    const auto second = runtime->retain("dual-real-streaming-secondary", *first.launch);
    INFO(second.error);
    REQUIRE(second.launch);
    // Reproduce first-start admission: the retained secondary cannot resume
    // while the initial primary is still pending its first encoded telemetry.
    REQUIRE_FALSE(runtime->resume(*second.launch).empty());
    REQUIRE(first.launch->reservation->valid());
    REQUIRE(second.launch->reservation->valid());

    const std::string pipeline =
        "interpipesrc name=interpipesrc_{session_id}_video listen-to={session_id}_video "
        "is-live=true stream-sync=restart-ts ! videoconvertscale ! "
        "video/x-raw,width={width},height={height},format=I420 ! "
        "x264enc tune=zerolatency speed-preset=ultrafast bitrate={bitrate} key-int-max=30 ! "
        "h264parse ! video/x-h264,stream-format=byte-stream,alignment=au ! queue ! "
        "rtpmoonlightpay_video name=moonlight_pay payload_size={payload_size} fec_percentage={fec_percentage} "
        "min_required_fec_packets={min_required_fec_packets} ! appsink name=wolf_udp_sink sync=false";
    for (int index = 0; index < 2; ++index) {
      targets[index] = std::make_shared<events::GpuStreamTarget>();
      targets[index]->launch = index == 0 ? first.launch : second.launch;
      targets[index]->context = std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>();
      targets[index]->pipelines[0] = pipeline;
      targets[index]->producer = index == 0 ? "dual_real_primary" : "dual_real_secondary";
      GError *parse_error = nullptr;
      producers[index] = {gst_parse_launch(fmt::format("videotestsrc is-live=true pattern={} ! "
                                                       "video/x-raw,width=320,height=240,framerate=30/1 ! "
                                                       "interpipesink name={}_video sync=false async=false",
                                                       index == 0 ? "black" : "ball",
                                                       targets[index]->producer)
                                               .c_str(),
                                           &parse_error),
                          [](auto pipeline) {
                            gst_element_set_state(pipeline, GST_STATE_NULL);
                            gst_object_unref(pipeline);
                          }};
      INFO((parse_error ? parse_error->message : ""));
      REQUIRE_FALSE(parse_error);
      REQUIRE(producers[index]);
      REQUIRE(gst_element_set_state(producers[index].get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    }
    dual->config = moonlight::dual_display::parse({{"gd_version", "1"},
                                                   {"gd_mode", "dual_app"},
                                                   {"gd_layout", "horizontal"},
                                                   {"gd_primary", "0"},
                                                   {"gd_width2", "160"},
                                                   {"gd_height2", "128"},
                                                   {"gd_fps2", "15"}},
                                                  320,
                                                  240,
                                                  30);
    auto routing = std::make_shared<events::DisplayRouting>();
    for (int index = 0; index < 2; ++index) {
      routing->slots[index].producer = targets[index]->producer;
      routing->slots[index].target = targets[index];
      dual->home[index] = targets[index];
    }
    dual->routing.store(routing);
    dual->negotiated = true;
    dual->generation = 7;
    auto route = std::make_shared<events::GpuStreamRoute>();
    route->home = targets[0];
    route->target.store(targets[0]);
    route->runtime = runtime;
    settings = {.gpu_route = route,
                .display_mode = {.width = 320, .height = 240, .refreshRate = 30},
                .gst_pipeline = pipeline,
                .render_node = device.render_node,
                .session_id = 707,
                .packet_size = 1024,
                .fec_percentage = 20,
                .min_required_fec_packets = 2,
                .bitrate_kbps = 2000,
                .slices_per_frame = 1,
                .color_range = events::ColorRange::MPEG,
                .color_space = events::ColorSpace::BT709,
                .dual_display = dual,
                .display_generation = dual->generation.load()};
  }

  void start() {
    // Discard the prior connection's tail before checking a resumed connection.
    while (!receive(0ms).empty()) {
    }
    finished = std::promise<void>();
    completion = finished.get_future();
    error = {};
    worker = std::jthread([this] {
      try {
        streaming::start_streaming_video(immer::box<events::VideoSession>(settings),
                                         bus,
                                         "127.0.0.1",
                                         receiver.local_endpoint().port(),
                                         targets[0]->context,
                                         sender);
      } catch (...) {
        error = std::current_exception();
      }
      finished.set_value();
    });
  }

  std::vector<unsigned char> receive(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      std::vector<unsigned char> bytes(4096);
      boost::system::error_code ec;
      const auto size = receiver.receive(boost::asio::buffer(bytes), 0, ec);
      if (!ec) {
        bytes.resize(size);
        return bytes;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
  }

  void stop() {
    bus->fire_event(immer::box<events::StopStreamEvent>{events::StopStreamEvent{settings.session_id}});
  }

  ~DualStreamingFixture() {
    if (worker.joinable()) {
      // Repeat during cleanup so a failed early assertion cannot miss the
      // handlers while the streaming thread is still registering them.
      for (int attempt = 0; attempt < 100 && completion.wait_for(0ms) != std::future_status::ready; ++attempt) {
        stop();
        std::this_thread::sleep_for(10ms);
      }
      worker.join();
    }
  }
};
} // namespace

TEST_CASE("Dual streaming waits for startup admission and sends both real CPU encoded tracks",
          "[dual-display][streaming]") {
  if (!cpu_streaming_plugins_available())
    SKIP("Real CPU streaming requires x264enc, h264parse, videotestsrc and interpipe plugins");
  DualStreamingFixture fixture;
  for (int connection = 0; connection < 2; ++connection) {
    CAPTURE(connection);
    fixture.start();
    std::array<std::set<std::uint32_t>, 2> frames;
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while ((frames[0].size() < 3 || frames[1].size() < 3) && std::chrono::steady_clock::now() < deadline) {
      if (fixture.completion.wait_for(0ms) == std::future_status::ready)
        break;
      const auto bytes = fixture.receive(50ms);
      if (bytes.empty())
        continue;
      REQUIRE(bytes.size() >= sizeof(gst_moonlight_video::VideoRTPHeaders));
      gst_moonlight_video::VideoRTPHeaders header;
      std::memcpy(&header, bytes.data(), sizeof(header));
      const auto ssrc = boost::endian::big_to_native(header.rtp.ssrc);
      REQUIRE((ssrc == moonlight::dual_display::ssrc(0) || ssrc == moonlight::dual_display::ssrc(1)));
      if (header.packet.flags & FLAG_SOF)
        frames[ssrc & 1].insert(header.packet.frameIndex);
    }
    CAPTURE(frames[0].size(), frames[1].size());
    CAPTURE(fixture.targets[0]->launch->reservation->error(), fixture.targets[1]->launch->reservation->error());
    if (fixture.completion.wait_for(0ms) == std::future_status::ready && fixture.error)
      std::rethrow_exception(fixture.error);
    REQUIRE(frames[0].size() >= 3);
    REQUIRE(frames[1].size() >= 3);
    REQUIRE(fixture.targets[0]->launch->reservation->encoding());
    REQUIRE(fixture.targets[1]->launch->reservation->encoding());
    REQUIRE(fixture.dual->streaming.load());
    fixture.stop();
    REQUIRE(fixture.completion.wait_for(2s) == std::future_status::ready);
    fixture.worker.join();
    if (fixture.error)
      std::rethrow_exception(fixture.error);
    REQUIRE_FALSE(fixture.dual->streaming.load());
    REQUIRE_FALSE(fixture.targets[0]->launch->reservation->encoding());
    REQUIRE_FALSE(fixture.targets[1]->launch->reservation->encoding());
    REQUIRE(fixture.targets[0]->launch->reservation->valid());
    REQUIRE(fixture.targets[1]->launch->reservation->valid());
  }
}

TEST_CASE("Stopping dual streaming cancels a secondary encoder waiting for initial admission",
          "[dual-display][streaming]") {
  if (!cpu_streaming_plugins_available())
    SKIP("Real CPU streaming requires x264enc, h264parse, videotestsrc and interpipe plugins");
  DualStreamingFixture fixture;
  fixture.start();
  REQUIRE(wait_for_dual_stream([&] { return fixture.targets[0]->launch->reservation->encoding(); }));
  // The primary is running but its one-second encoded-output settling window
  // has not elapsed, so the retained secondary still has to await admission.
  REQUIRE_FALSE(fixture.targets[1]->launch->reservation->encoding());
  fixture.stop();
  REQUIRE(fixture.completion.wait_for(2s) == std::future_status::ready);
  fixture.worker.join();
  if (fixture.error)
    std::rethrow_exception(fixture.error);
  REQUIRE_FALSE(fixture.dual->streaming.load());
  REQUIRE_FALSE(fixture.targets[0]->launch->reservation->encoding());
  REQUIRE_FALSE(fixture.targets[1]->launch->reservation->encoding());
  REQUIRE(fixture.targets[0]->launch->reservation->valid());
  REQUIRE(fixture.targets[1]->launch->reservation->valid());
}

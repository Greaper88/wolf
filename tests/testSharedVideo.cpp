#include <catch2/catch_test_macros.hpp>
#include <future>
#include <gst/app/gstappsink.h>
#include <sessions/handlers.hpp>
#include <set>
#include <state/config.hpp>
#include <streaming/streaming.hpp>

using namespace wolf::core;
using namespace streaming;
using namespace std::chrono_literals;

namespace {
EncodedSample frame(bool key, std::uint64_t number) {
  auto buffer = gst_buffer_new_allocate(nullptr, 16, nullptr);
  GST_BUFFER_OFFSET(buffer) = number;
  if (!key)
    GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  auto sample = EncodedSample(gst_sample_new(buffer, nullptr, nullptr, nullptr), gst_sample_unref);
  gst_buffer_unref(buffer);
  return sample;
}
events::VideoSession video_settings() {
  return {.display_mode = {.width = 320, .height = 240, .refreshRate = 30},
          .gst_pipeline = "interpipesrc name=interpipesrc_{}_video listen-to={session_id}_video is-live=true "
                          "stream-sync=restart-ts ! videoconvertscale ! "
                          "video/x-raw,width={width},height={height},format=I420 ! "
                          "x264enc tune=zerolatency bitrate={bitrate} key-int-max=300 ! h264parse ! "
                          "video/x-h264,stream-format=byte-stream,alignment=au ! queue ! "
                          "rtpmoonlightpay_video name=moonlight_pay payload_size={payload_size} "
                          "fec_percentage={fec_percentage} min_required_fec_packets={min_required_fec_packets} ! "
                          "appsink name=wolf_udp_sink sync=false",
          .render_node = "/dev/dri/renderD129",
          .session_id = 1,
          .packet_size = 1392,
          .fec_percentage = 20,
          .min_required_fec_packets = 2,
          .bitrate_kbps = 1000,
          .slices_per_frame = 1,
          .color_range = events::ColorRange::MPEG,
          .color_space = events::ColorSpace::BT709};
}
template <typename F> bool eventually(F ready) {
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}
struct TestGpuRuntime {
  wolf::gpu::Device a{.id = "gpu-a", .render_node = "/dev/dri/renderD129", .driver = "amdgpu", .accessible = true};
  wolf::gpu::Device b{.id = "gpu-b", .render_node = "/dev/dri/renderD130", .driver = "amdgpu", .accessible = true};
  std::atomic_bool saturated{false};
  std::shared_ptr<wolf::gpu::Runtime> runtime = std::make_shared<wolf::gpu::Runtime>(
      wolf::gpu::Options{},
      [this] {
        auto copy = a;
        copy.encoder_percent = saturated ? 99 : 5;
        return std::vector{copy, b};
      },
      [] { return "shared-video-test"; },
      [](const wolf::gpu::Device &d, wolf::gpu::Codec c, std::stop_token) {
        return wolf::gpu::EncoderProbeResult{
            .encoder = wolf::gpu::EncoderBinding{"vah264enc", "va", c, d.render_node, std::nullopt}};
      });
  TestGpuRuntime(std::string node = "/dev/dri/renderD129", std::string other_node = "/dev/dri/renderD130")
      : a{.id = "gpu-a", .render_node = std::move(node), .driver = "amdgpu", .accessible = true},
        b{.id = "gpu-b", .render_node = std::move(other_node), .driver = "amdgpu", .accessible = true} {
    REQUIRE(eventually([&] {
      auto cached = runtime->capabilities(b);
      return cached && cached->status == wolf::gpu::CapabilityCache::Status::ready;
    }));
  }
  std::shared_ptr<events::GpuStreamTarget> target(const std::string &id, const std::string &gpu) {
    auto admitted = runtime->prepare(id, gpu);
    REQUIRE(admitted.launch);
    auto result = std::make_shared<events::GpuStreamTarget>();
    result->launch = admitted.launch;
    result->producer = id;
    result->context = std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>();
    result->pipelines[0] = video_settings().gst_pipeline;
    return result;
  }
};

// Exercise the production appsrc -> per-client packetizer -> UDP path without a Moonlight device.
struct TestVideoViewer {
  boost::asio::io_context io;
  udp::socket receiver{io, udp::endpoint(boost::asio::ip::address_v4::loopback(), 0)};
  std::shared_ptr<udp::socket> sender = std::make_shared<udp::socket>(io, udp::endpoint(udp::v4(), 0));
  std::shared_ptr<events::EventBusType> bus = std::make_shared<events::EventBusType>();
  std::shared_ptr<events::GpuStreamRoute> route = std::make_shared<events::GpuStreamRoute>();
  std::jthread worker;
  TestVideoViewer(events::VideoSession settings,
                  const std::shared_ptr<events::GpuStreamTarget> &home,
                  const std::shared_ptr<SharedVideoEncoder> &encoder,
                  const std::shared_ptr<wolf::gpu::Runtime> &runtime,
                  const std::string &producer) {
    receiver.non_blocking(true);
    auto target = std::make_shared<events::GpuStreamTarget>(*home);
    if (encoder)
      target->launch = encoder->launch;
    target->shared_encoder = encoder;
    target->producer = producer;
    route->home = home;
    route->target.store(target);
    route->runtime = runtime;
    settings.gpu_route = route;
    worker = std::jthread([this, settings, home](std::stop_token stop) {
      std::jthread cancellation([this, stop, id = settings.session_id](std::stop_token done) {
        while (!done.stop_requested()) {
          if (stop.stop_requested())
            bus->fire_event(immer::box<events::PauseStreamEvent>{events::PauseStreamEvent{id}});
          std::this_thread::sleep_for(5ms);
        }
      });
      start_streaming_video(immer::box<events::VideoSession>(settings),
                            bus,
                            "127.0.0.1",
                            receiver.local_endpoint().port(),
                            home->context,
                            sender);
    });
  }
  ~TestVideoViewer() {
    worker.request_stop();
    worker.join();
  }
  std::vector<unsigned char> receive(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      std::vector<unsigned char> data(10000);
      boost::system::error_code error;
      auto size = receiver.receive(boost::asio::buffer(data), 0, error);
      if (!error) {
        data.resize(size);
        return data;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
  }
};

// Reassemble data shards (the loopback test does not lose packets) and decode them.
// Packet counts alone cannot detect an unusable bitstream or a broken encoder handoff.
struct TestHevcDecoder {
  gstreamer::gst_element_ptr pipeline;
  gstreamer::gst_element_ptr source;
  std::atomic_uint decoded{0};
  unsigned complete = 0;
  std::uint32_t frame_index = UINT32_MAX;
  std::map<unsigned, std::map<unsigned, std::vector<unsigned char>>> blocks;
  std::map<unsigned, unsigned> block_sizes;
  explicit TestHevcDecoder(const std::string &factory) {
    GError *error = nullptr;
    pipeline = {gst_parse_launch(fmt::format("appsrc name=input is-live=true format=time caps=\"video/x-h265,"
                                             "stream-format=byte-stream,alignment=au\" ! h265parse ! {} ! "
                                             "appsink name=decoded sync=false async=false",
                                             factory)
                                     .c_str(),
                                 &error),
                gst_object_unref};
    INFO((error ? error->message : ""));
    REQUIRE_FALSE(error);
    REQUIRE(pipeline);
    source = {gst_bin_get_by_name(GST_BIN(pipeline.get()), "input"), gst_object_unref};
    auto sink = gst_bin_get_by_name(GST_BIN(pipeline.get()), "decoded");
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = [](GstAppSink *sink, gpointer data) {
      if (auto sample = gst_app_sink_pull_sample(sink)) {
        static_cast<TestHevcDecoder *>(data)->decoded.fetch_add(1);
        gst_sample_unref(sample);
      }
      return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
    gst_object_unref(sink);
    REQUIRE(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  }
  ~TestHevcDecoder() {
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
  }
  void receive(const std::vector<unsigned char> &packet) {
    using namespace gst_moonlight_video;
    if (packet.size() < sizeof(VideoRTPHeaders))
      return;
    VideoRTPHeaders header;
    std::memcpy(&header, packet.data(), sizeof(header));
    auto &video = header.packet;
    if (video.frameIndex != frame_index) {
      frame_index = video.frameIndex;
      blocks.clear();
      block_sizes.clear();
    }
    auto shard = (video.fecInfo >> 12) & 0x3ff;
    auto count = video.fecInfo >> 22;
    if (shard >= count)
      return; // parity
    auto block = video.multiFecBlocks >> 4;
    auto last_block = video.multiFecBlocks & 0xf;
    block_sizes[block] = count;
    blocks[block][shard] = {packet.begin() + sizeof(VideoRTPHeaders), packet.end()};
    for (unsigned i = 0; i <= last_block; ++i)
      if (!block_sizes.contains(i) || blocks[i].size() != block_sizes[i])
        return;
    std::vector<unsigned char> data;
    for (auto &[block_index, shards] : blocks)
      for (auto &[shard_index, bytes] : shards)
        data.insert(data.end(), bytes.begin(), bytes.end());
    REQUIRE(data.size() >= sizeof(VideoShortHeader));
    VideoShortHeader short_header;
    std::memcpy(&short_header, data.data(), sizeof(short_header));
    const auto last_size = blocks[last_block].rbegin()->second.size();
    REQUIRE(short_header.last_payload_len <= last_size);
    data.resize(data.size() - last_size + short_header.last_payload_len);
    REQUIRE(data.size() >= sizeof(VideoShortHeader));
    auto buffer = gst_buffer_new_allocate(nullptr, data.size() - sizeof(VideoShortHeader), nullptr);
    gst_buffer_fill(buffer, 0, data.data() + sizeof(VideoShortHeader), data.size() - sizeof(VideoShortHeader));
    GST_BUFFER_PTS(buffer) = complete * GST_SECOND / 144;
    ++complete;
    REQUIRE(gst_app_src_push_buffer(GST_APP_SRC(source.get()), buffer) == GST_FLOW_OK);
  }
};
struct ProducerTimestamp {
  std::mutex mutex;
  std::shared_ptr<GstBuffer> first;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
};
std::shared_ptr<ProducerTimestamp> watch_timestamp(GstElement *pipeline, const char *sink_name) {
  auto state = std::make_shared<ProducerTimestamp>();
  auto sink = gst_bin_get_by_name(GST_BIN(pipeline), sink_name);
  auto pad = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(
      pad,
      GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad *, GstPadProbeInfo *info, gpointer data) {
        auto state = *static_cast<std::shared_ptr<ProducerTimestamp> *>(data);
        std::lock_guard lock(state->mutex);
        if (!state->first) {
          auto buffer = GST_PAD_PROBE_INFO_BUFFER(info);
          state->pts = GST_BUFFER_PTS(buffer);
          state->first = {gst_buffer_ref(buffer), gst_buffer_unref};
        }
        return GST_PAD_PROBE_OK;
      },
      new std::shared_ptr<ProducerTimestamp>(state),
      [](gpointer data) { delete static_cast<std::shared_ptr<ProducerTimestamp> *>(data); });
  gst_object_unref(pad);
  gst_object_unref(sink);
  return state;
}
} // namespace

TEST_CASE("Shared video waits for keyframes and isolates slow viewers", "[shared-video]") {
  EncodedVideoQueue queue;
  queue.push(frame(false, 1));
  REQUIRE_FALSE(queue.pop(0ms));
  queue.push(frame(true, 2));
  REQUIRE(GST_BUFFER_OFFSET(gst_sample_get_buffer(queue.pop(0ms).get())) == 2);
  for (unsigned i = 3; i != 7; ++i)
    REQUIRE_FALSE(queue.push(frame(false, i)));
  REQUIRE(queue.push(frame(false, 7)));
  REQUIRE_FALSE(queue.pop(0ms));
  queue.push(frame(false, 8));
  REQUIRE_FALSE(queue.pop(0ms));
  queue.push(frame(true, 9));
  REQUIRE(GST_BUFFER_OFFSET(gst_sample_get_buffer(queue.pop(0ms).get())) == 9);
}

TEST_CASE("Shared VA video continues after a high frame rate viewer joins", "[shared-video][hardware]") {
  const auto node = std::getenv("WOLF_TEST_SHARED_VA_NODE");
  if (!node)
    SKIP("Set WOLF_TEST_SHARED_VA_NODE to opt into an isolated VA hardware check");
  const auto home_node = std::getenv("WOLF_TEST_SHARED_VA_HOME_NODE");
  TestGpuRuntime gpu(node, home_node ? home_node : "/dev/dri/renderD130");
  auto home = gpu.target("va-home", gpu.a.id);
  auto held = gpu.runtime->retain("va-app", *home->launch);
  REQUIRE(held.launch);
  auto lobby = *home;
  lobby.launch = held.launch;
  lobby.producer = "va_shared_regression";
  lobby.shared_encoders = std::make_shared<SharedVideoEncoders>();
  auto settings = video_settings();
  settings.display_mode.width = 1920;
  settings.display_mode.height = 1080;
  settings.bitrate_kbps = 6000;
  auto factory = std::filesystem::path(node).filename().string();
  settings.render_node = node;
  settings.gst_pipeline = fmt::format(
      "interpipesrc name=interpipesrc_{{}}_video listen-to={{session_id}}_video is-live=true "
      "stream-sync=restart-ts max-bytes=0 max-buffers=1 leaky-type=downstream ! videoconvertscale ! "
      "video/x-raw,width={{width}},height={{height}},format=NV12 ! "
      "va{}h265enc aud=false b-frames=0 ref-frames=1 num-slices=1 bitrate={{bitrate}} "
      "cpb-size={{vbv_buffer_size}} min-qp=20 key-int-max=1024 rate-control=cbr target-usage=6 ! "
      "h265parse ! video/x-h265,stream-format=byte-stream,alignment=au ! queue ! "
      "rtpmoonlightpay_video name=moonlight_pay payload_size={{payload_size}} "
      "fec_percentage={{fec_percentage}} min_required_fec_packets={{min_required_fec_packets}} ! "
      "appsink name=wolf_udp_sink sync=false",
      factory);
  home->pipelines[0] = settings.gst_pipeline;
  lobby.pipelines[0] = settings.gst_pipeline;
  auto first = prepare_shared_video(settings, lobby, *home, gpu.runtime);
  REQUIRE(first.encoder);
  gstreamer::gst_element_ptr producer(
      gst_parse_launch("videotestsrc is-live=true ! video/x-raw,width=1920,height=1080,framerate=30/1 ! "
                       "interpipesink name=va_shared_regression_video sync=true async=false max-buffers=1",
                       nullptr),
      [](auto p) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
      });
  REQUIRE(gst_element_set_state(producer.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  auto owner = first.encoder->subscribe();
  REQUIRE(owner->pop(2000ms));
  first.encoder->launch->reservation->activate();
  settings.display_mode.refreshRate = 144;
  settings.bitrate_kbps = 24108;
  auto second = prepare_shared_video(settings, lobby, *home, gpu.runtime);
  INFO(second.error);
  REQUIRE(second.encoder);
  auto guest = second.encoder->subscribe();
  REQUIRE(guest->pop(2000ms));
  TestHevcDecoder decoder("va" + factory + "h265dec");
  auto initial_encoder = first.encoder;
  SECTION("switch between shared encoders") {}
  SECTION("switch from a dedicated launcher encoder") {
    initial_encoder.reset();
    if (home_node) {
      home = gpu.target("va-other-home", gpu.b.id);
      auto home_pipeline = settings.gst_pipeline;
      const auto old_factory = "va" + factory + "h265enc";
      const auto new_factory = "va" + std::filesystem::path(home_node).filename().string() + "h265enc";
      home_pipeline.replace(home_pipeline.find(old_factory), old_factory.size(), new_factory);
      home->pipelines[0] = home_pipeline;
    }
  }
  TestVideoViewer transport(settings, home, initial_encoder, gpu.runtime, lobby.producer);
  auto warmup = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < warmup) {
    owner->pop(0ms);
    guest->pop(0ms);
    decoder.receive(transport.receive(1ms));
  }
  REQUIRE(decoder.decoded > 5);
  const auto before_switch = decoder.decoded.load();
  auto next_target = std::make_shared<events::GpuStreamTarget>(*transport.route->target.load());
  next_target->launch = second.encoder->launch;
  next_target->context = lobby.context;
  next_target->pipelines = lobby.pipelines;
  next_target->shared_encoder = second.encoder;
  transport.route->target.store(next_target);
  std::set<std::uint32_t> frames_sent;
  unsigned owner_frames = 0, guest_frames = 0;
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (owner->pop(0ms))
      ++owner_frames;
    if (guest->pop(0ms))
      ++guest_frames;
    auto packet = transport.receive(1ms);
    decoder.receive(packet);
    const auto offset = sizeof(moonlight::RTP_PACKET) + 4 + offsetof(moonlight::NV_VIDEO_PACKET, frameIndex);
    if (packet.size() >= offset + sizeof(std::uint32_t)) {
      std::uint32_t index;
      std::memcpy(&index, packet.data() + offset, sizeof(index));
      frames_sent.insert(index);
    }
  }
  INFO("owner frames=" << owner_frames << ", guest frames=" << guest_frames << ", sent=" << frames_sent.size());
  REQUIRE(owner_frames > 30);
  REQUIRE(guest_frames > 30);
  REQUIRE(frames_sent.size() > 30);
  INFO("decoded before switch=" << before_switch << ", after switch=" << decoder.decoded.load() - before_switch);
  REQUIRE(decoder.decoded.load() - before_switch > 30);
}

TEST_CASE("Shared video matches encoding settings independently of transport", "[shared-video]") {
  auto first = video_settings();
  auto a = shared_video_description(first, "lobby-a");
  REQUIRE(a);
  auto second = first;
  second.session_id = 987;
  second.client_ip = "192.0.2.7";
  second.packet_size = 1000;
  second.fec_percentage = 40;
  auto b = shared_video_description(second, "lobby-a");
  REQUIRE(b);
  REQUIRE(a->key == b->key);
  REQUIRE(a->viewer != b->viewer);
  REQUIRE(a->key != shared_video_description(first, "another-app")->key);
  for (int setting = 0; setting != 7; ++setting) {
    auto changed = first;
    switch (setting) {
    case 0:
      changed.display_mode.width *= 2;
      break;
    case 1:
      changed.display_mode.refreshRate *= 2;
      break;
    case 2:
      changed.bitrate_kbps *= 2;
      break;
    case 3:
      changed.color_range = events::ColorRange::JPEG;
      break;
    case 4:
      changed.slices_per_frame = 2;
      break;
    case 5:
      changed.render_node = "/dev/dri/renderD130";
      break;
    case 6:
      changed.gst_pipeline.replace(changed.gst_pipeline.find("x264enc"), 7, "x265enc");
      break;
    }
    REQUIRE(a->key != shared_video_description(changed, "lobby-a")->key);
  }
  first.gst_pipeline = "custom source and sink";
  REQUIRE_FALSE(shared_video_description(first, "lobby-a"));
}

TEST_CASE("Shared encoders are allocated once and retire after the last owner", "[shared-video]") {
  SharedVideoEncoders registry;
  std::atomic_int created{0}, started{0}, stopped{0};
  auto create = [&] {
    ++created;
    return std::make_shared<SharedVideoEncoder>(nullptr, [&](auto &, std::stop_token stop) {
      ++started;
      while (!stop.stop_requested())
        std::this_thread::sleep_for(1ms);
      ++stopped;
    });
  };
  auto pending = std::async(std::launch::async, [&] { return registry.acquire("same", create); });
  auto first = registry.acquire("same", create);
  auto second = pending.get();
  REQUIRE(first == second);
  REQUIRE(created == 1);
  auto slow = first->subscribe();
  auto fast = second->subscribe();
  REQUIRE(eventually([&] { return started == 1; }));
  for (unsigned i = 0; i != 8; ++i) {
    first->publish(frame(i == 0, i));
    REQUIRE(fast->pop(0ms));
  }
  REQUIRE_FALSE(slow->pop(0ms));
  REQUIRE(first->take_keyframe_request());
  first.reset();
  REQUIRE(stopped == 0);
  REQUIRE(second->healthy());
  second.reset();
  REQUIRE(stopped == 1);
  auto replacement = registry.acquire("same", create);
  REQUIRE(created == 2);
}

TEST_CASE("Shared video accepts a source caps filter", "[shared-video]") {
  auto settings = video_settings();
  const auto boundary = settings.gst_pipeline.find('!');
  settings.gst_pipeline.insert(boundary + 1, " video/x-raw(ANY) !");
  auto description = shared_video_description(settings, "caps-regression");
  REQUIRE(description);
  GError *error = nullptr;
  auto pipeline = gst_parse_launch(description->encoder.c_str(), &error);
  const auto message = error ? std::string(error->message) : std::string{};
  if (error)
    g_error_free(error);
  if (pipeline)
    gst_object_unref(pipeline);
  INFO(message);
  REQUIRE(message.empty());
}

TEST_CASE("Lobby shares one real encoder across late viewers and refuses extra work when full", "[shared-video]") {
  TestGpuRuntime gpu;
  auto home = gpu.target("home", gpu.a.id);
  auto held = gpu.runtime->retain("app", *home->launch);
  REQUIRE(held.launch);
  auto lobby = *home;
  lobby.launch = held.launch;
  lobby.producer = "shared_video_regression";
  lobby.shared_encoders = std::make_shared<SharedVideoEncoders>();
  auto first = prepare_shared_video(video_settings(), lobby, *home, gpu.runtime);
  REQUIRE(first.error.empty());
  REQUIRE(first.encoder);
  REQUIRE(first.encoder->launch->reservation != home->launch->reservation);
  gstreamer::gst_element_ptr producer(
      gst_parse_launch("videotestsrc is-live=true ! video/x-raw,width=320,height=240,framerate=30/1 ! "
                       "interpipesink name=shared_video_regression_video sync=false async=false",
                       nullptr),
      [](auto p) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
      });
  REQUIRE(producer);
  auto timestamp = watch_timestamp(producer.get(), "shared_video_regression_video");
  REQUIRE(gst_element_set_state(producer.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  auto first_viewer = first.encoder->subscribe();
  auto picture = first_viewer->pop(2000ms);
  REQUIRE(picture);
  // Interpipe fans out references to the same raw buffer. A listener must not
  // reset that shared header's timestamps while another encoder is reading it.
  {
    std::lock_guard lock(timestamp->mutex);
    REQUIRE(timestamp->first);
    REQUIRE(GST_BUFFER_PTS(timestamp->first.get()) == timestamp->pts);
  }
  REQUIRE_FALSE(GST_BUFFER_FLAG_IS_SET(gst_sample_get_buffer(picture.get()), GST_BUFFER_FLAG_DELTA_UNIT));
  auto caps = gst_sample_get_caps(picture.get());
  REQUIRE(std::string(gst_structure_get_name(gst_caps_get_structure(caps, 0))) == "video/x-h264");
  gpu.saturated = true;
  auto remote_home = gpu.target("remote-home", gpu.b.id);
  auto second = prepare_shared_video(video_settings(), lobby, *remote_home, gpu.runtime);
  REQUIRE(second.error.empty());
  REQUIRE(second.encoder == first.encoder); // Reuse does not ask saturated GPU for another slot.
  auto second_viewer = second.encoder->subscribe();
  REQUIRE(second_viewer->pop(2000ms)); // A late viewer requests a fresh keyframe and codec headers.
  auto changed = video_settings();
  changed.bitrate_kbps *= 2;
  changed.display_mode.refreshRate = 144;
  auto rejected = prepare_shared_video(changed, lobby, *remote_home, gpu.runtime);
  REQUIRE_FALSE(rejected.encoder);
  REQUIRE_FALSE(rejected.error.empty());
  home->launch->reservation->cancel(); // First player leaves: their launcher does not own the encoder.
  first_viewer.reset();
  first.encoder.reset();
  REQUIRE(second.encoder->healthy());
  REQUIRE(second_viewer->pop(1000ms));
  gpu.saturated = false;
  REQUIRE(eventually([&] { return second.encoder->launch->reservation->encoding(); }));
  second.encoder->launch->reservation->activate();
  auto additional = prepare_shared_video(changed, lobby, *remote_home, gpu.runtime);
  REQUIRE(additional.error.empty());
  REQUIRE(additional.encoder);
  REQUIRE(additional.encoder != second.encoder);
  REQUIRE(additional.encoder->launch->device.id == gpu.a.id);
  auto third_viewer = additional.encoder->subscribe();
  REQUIRE(third_viewer->pop(2000ms));
  // The first frame alone is not a working stream. Joining a 30 FPS app with a
  // 144 FPS client must keep producing frames in both independently timed encoders.
  for (auto viewer : {second_viewer, third_viewer}) {
    unsigned frames = 0;
    auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < deadline && frames < 10)
      if (viewer->pop(20ms))
        ++frames;
    REQUIRE(frames >= 10);
  }
  REQUIRE(additional.encoder->launch->reservation->encoding());
  REQUIRE(second.encoder->launch->reservation->encoding());

  auto transport_a =
      std::make_unique<TestVideoViewer>(video_settings(), remote_home, second.encoder, gpu.runtime, lobby.producer);
  auto transported = transport_a->receive(2000ms);
  REQUIRE_FALSE(transported.empty());
  auto client_b = video_settings();
  client_b.session_id = 222;
  client_b.packet_size = 512;
  client_b.fec_percentage = 40;
  auto transport_b =
      std::make_unique<TestVideoViewer>(client_b, remote_home, second.encoder, gpu.runtime, lobby.producer);
  auto independent = transport_b->receive(2000ms);
  REQUIRE_FALSE(independent.empty());
  REQUIRE(independent.size() < transported.size()); // Different packetization from the same encoded frames.
  transport_a.reset();
  while (!transport_b->receive(0ms).empty()) {
  } // Discard packets sent before the first viewer left.
  REQUIRE_FALSE(transport_b->receive(1000ms).empty());
  REQUIRE(second.encoder->healthy());
  transport_b.reset();
  auto reservation = second.encoder->launch->reservation;
  auto other_reservation = additional.encoder->launch->reservation;
  std::weak_ptr<SharedVideoEncoder> last_encoder = second.encoder;
  second.encoder.reset();
  additional.encoder.reset();
  REQUIRE(last_encoder.expired());
  REQUIRE_FALSE(reservation->encoding());
  REQUIRE_FALSE(other_reservation->encoding());
  REQUIRE(lobby.launch->reservation->valid()); // Empty persistent lobby keeps the game, not the encoders.
  home.reset();
  auto restarted = prepare_shared_video(video_settings(), lobby, *remote_home, gpu.runtime);
  REQUIRE(restarted.error.empty());
  REQUIRE(restarted.encoder);
  REQUIRE(restarted.encoder->launch->reservation != reservation);
  auto resumed_viewer = restarted.encoder->subscribe();
  REQUIRE(resumed_viewer->pop(2000ms));
}

TEST_CASE("Automatic shared lobby routes viewers to its pinned GPU and preserves private ownership", "[shared-video]") {
  TestGpuRuntime gpu;
  auto home = gpu.target("owner-home", gpu.a.id);
  auto remote_home = gpu.target("guest-home", gpu.b.id);
  auto held = gpu.runtime->retain("pinned-app", *home->launch);
  REQUIRE(held.launch);
  auto pinned = std::make_shared<events::GpuStreamTarget>(*home);
  pinned->launch = held.launch;
  pinned->producer = "coop";
  pinned->shared_encoders = std::make_shared<SharedVideoEncoders>();
  auto bus = std::make_shared<events::EventBusType>();
  auto sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  for (auto [id, target] : {std::pair{1U, home}, std::pair{2U, remote_home}}) {
    events::StreamSession viewer{};
    viewer.session_id = id;
    viewer.gpu_route = std::make_shared<events::GpuStreamRoute>();
    viewer.gpu_route->home = target;
    viewer.gpu_route->target.store(target);
    viewer.gpu_route->runtime = gpu.runtime;
    viewer.gpu_route->streaming = true;
    viewer.gpu_route->negotiated_video.store(std::make_shared<events::VideoSession>(video_settings()));
    sessions->update([&](auto list) { return list.push_back(viewer); });
  }
  events::Lobby lobby{.id = "coop",
                      .name = "Game",
                      .started_by_profile_id = "owner",
                      .gpu_target = pinned,
                      .multi_user = true,
                      .stop_when_everyone_leaves = false};
  auto state = immer::box<state::AppState>(
      state::AppState{.event_bus = bus,
                      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                      .running_sessions = sessions,
                      .gpu_runtime = gpu.runtime});
  state->lobbies->update([&](auto list) { return list.push_back(lobby); });
  auto handlers = sessions::setup_lobbies_handlers(state, "/tmp", {});
  auto join = [&](unsigned id, const std::string &profile) {
    events::JoinLobbyEvent request{.profile_id = profile, .lobby_id = "coop", .moonlight_session_id = id};
    bus->fire_event(immer::box<events::JoinLobbyEvent>(request));
    return request.error_message.get()->get_future().get();
  };
  REQUIRE(join(1, "owner").empty());
  gpu.saturated = true;
  REQUIRE(join(2, "guest").empty());
  auto owner = sessions->load()->at(0).gpu_route;
  auto guest = sessions->load()->at(1).gpu_route;
  REQUIRE(guest->target.load()->launch->device.id == gpu.a.id);
  REQUIRE(guest->target.load()->shared_encoder == owner->target.load()->shared_encoder);
  REQUIRE(lobby.connected_sessions->load()->size() == 2);
  REQUIRE(join(2, "guest").empty());
  REQUIRE(lobby.connected_sessions->load()->size() == 2); // Duplicate join is idempotent.
  bus->fire_event(immer::box<events::PauseStreamEvent>{events::PauseStreamEvent{1}});
  REQUIRE(lobby.connected_sessions->load()->size() == 1);
  REQUIRE(owner->target.load() == owner->home);
  REQUIRE(guest->target.load()->shared_encoder->healthy());
  REQUIRE(lobby.gpu_target->launch->reservation->valid());
  owner->codec.store(2);
  auto mismatch = join(1, "owner");
  REQUIRE(mismatch.find("AV1") != std::string::npos);
  REQUIRE(owner->target.load() == owner->home);
  REQUIRE(lobby.connected_sessions->load()->size() == 1);
  REQUIRE(guest->target.load()->shared_encoder->healthy());
  owner->codec.store(0);
  bus->fire_event(immer::box<events::PauseStreamEvent>{events::PauseStreamEvent{2}});
  REQUIRE(lobby.connected_sessions->load()->empty());
  REQUIRE(guest->target.load() == guest->home);
  REQUIRE(lobby.gpu_target->launch->reservation->valid());
  // A private app keeps the profile ownership check even with the shared machinery available.
  state->lobbies->update([&](auto) {
    return immer::vector<events::Lobby>{events::Lobby{.id = "coop",
                                                      .name = "Private",
                                                      .started_by_profile_id = "owner",
                                                      .gpu_target = pinned,
                                                      .multi_user = false,
                                                      .stop_when_everyone_leaves = false}};
  });
  REQUIRE_FALSE(join(2, "guest").empty());
}

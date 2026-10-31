#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::Equals;

#include <algorithm>
#include <boost/beast/_experimental/test/stream.hpp>
#include <crypto/crypto.hpp>
#include <rtsp/net.hpp>
#include <rtsp/parser.hpp>
#include <state/data-structures.hpp>
#include <string>
using namespace std::string_literals;
using namespace state;
using namespace rtsp;
using namespace wolf::core::audio;
using namespace wolf::core;

/**
 * In order to test rtsp::tcp_connection we create a derived class that does the opposite:
 * instead of waiting for a message; it'll first send a message and then wait for a response.
 */
class tcp_tester : public tcp_connection {

public:
  static auto create_client(asio::io_context &io_context, int port, const state::SessionsAtoms state) {
    auto tester = new tcp_tester(io_context, state);

    tcp::resolver resolver(io_context);
    asio::connect(tester->socket(), resolver.resolve("0.0.0.0", std::to_string(port)));

    return boost::shared_ptr<tcp_tester>(tester);
  }

  void run(std::string_view raw_msg, std::function<void(std::optional<RTSP_PACKET> /* response */)> on_response) {
    auto send_msg = rtsp::parse(raw_msg).value();

    send_message(send_msg, [self = shared_from_this(), on_response](auto bytes) {
      self->receive_message([self = self->shared_from_this(), on_response](auto reply_msg) {
        on_response(reply_msg);
        self->socket().close();
      });
    });

    run_context();
  }

  void run_context() {
    while (socket().is_open()) {
      ioc.run_one();
    }
  }

protected:
  explicit tcp_tester(asio::io_context &io_context, const state::SessionsAtoms state)
      : tcp_connection(io_context, state), ioc(io_context) {}

  boost::asio::io_context &ioc;
};

TEST_CASE("Custom Parser", "[RTSP]") {
  SECTION("Requests") {
    SECTION("Non valid packet") {
      auto parsed = rtsp::parse("OPTIONS rtsp://10.1.2.49:48010 RTSP/1.0"); // missing CSeq
      REQUIRE(parsed.has_value() == false);
    }

    SECTION("Basic packet") {
      auto payload = "MissingNo rtsp://1.1.1.1:1234 RTSP/1.0\r\n"
                     "CSeq: 1993\r\n\r\n"s;
      auto parsed = rtsp::parse(payload).value();

      REQUIRE(parsed.type == REQUEST);
      REQUIRE(parsed.request.type == TARGET_URI);
      REQUIRE_THAT(parsed.request.cmd, Equals("MissingNo"));
      REQUIRE_THAT(parsed.request.uri.ip, Equals("1.1.1.1"));
      REQUIRE_THAT(parsed.request.uri.protocol, Equals("rtsp"));
      REQUIRE(parsed.request.uri.port == 1234);
      REQUIRE(parsed.seq_number == 1993);

      REQUIRE(parsed.options.empty());
      REQUIRE(parsed.payloads.empty());

      // Round trip
      REQUIRE(rtsp::to_string(parsed) == rtsp::to_string(rtsp::parse(rtsp::to_string(parsed)).value()));
    }

    SECTION("Missing IP (AndroidTV)") {
      auto payload = "OPTIONS rtsp://:48010 RTSP/1.0\n"
                     "CSeq: 1\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: \n\n"s;
      auto parsed = rtsp::parse(payload).value();
      REQUIRE(parsed.type == REQUEST);
      REQUIRE(parsed.request.type == TARGET_URI);
      REQUIRE_THAT(parsed.request.cmd, Equals("OPTIONS"));
      REQUIRE_THAT(parsed.request.uri.ip, Equals(""));
      REQUIRE(parsed.request.uri.port == 48010);
      REQUIRE(parsed.seq_number == 1);

      REQUIRE(parsed.options.size() == 2);
      REQUIRE_THAT(parsed.options["X-GS-ClientVersion"], Equals("14"));
      REQUIRE_THAT(parsed.options["Host"], Equals(""));
      REQUIRE(parsed.payloads.empty());
    }

    SECTION("Stream target") {
      auto payload = "MissingNo streamid=audio/1/2/3 RTSP/1.0\r\n"
                     "CSeq: 1993\r\n\r\n"s;
      auto parsed = rtsp::parse(payload).value();

      REQUIRE(parsed.type == REQUEST);
      REQUIRE(parsed.request.type == TARGET_STREAM);
      REQUIRE_THAT(parsed.request.cmd, Equals("MissingNo"));
      REQUIRE_THAT(parsed.request.stream.type, Equals("audio"));
      REQUIRE_THAT(parsed.request.stream.params, Equals("/1/2/3"));
      REQUIRE(parsed.seq_number == 1993);

      REQUIRE(parsed.options.empty());
      REQUIRE(parsed.payloads.empty());

      // Round trip
      REQUIRE(rtsp::to_string(parsed) == rtsp::to_string(rtsp::parse(rtsp::to_string(parsed)).value()));
    }

    SECTION("Complete packet") {
      auto payload = "OPTIONS rtsp://10.1.2.49:48010 RTSP/1.0\n"
                     "CSeq: 1\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 10.1.2.49"
                     "\r\n\r\n"
                     "v=0\n"
                     "o=android 0 14 IN IPv4 0.0.0.0\n"
                     "s=NVIDIA Streaming Client\n"
                     "a=x-nv-video[0].clientViewportWd:1920\n"
                     "a=x-nv-video[0].clientViewportHt:1080"s;
      auto parsed = rtsp::parse(payload).value();

      REQUIRE(parsed.type == REQUEST);
      REQUIRE_THAT(parsed.request.cmd, Equals("OPTIONS"));
      REQUIRE(parsed.request.type == TARGET_URI);
      REQUIRE_THAT(parsed.request.uri.ip, Equals("10.1.2.49"));
      REQUIRE_THAT(parsed.request.uri.protocol, Equals("rtsp"));
      REQUIRE(parsed.request.uri.port == 48010);
      REQUIRE(parsed.seq_number == 1);

      // Options
      REQUIRE_THAT(parsed.options["X-GS-ClientVersion"], Equals("14"));
      REQUIRE_THAT(parsed.options["Host"], Equals("10.1.2.49"));

      // Payloads
      REQUIRE_THAT(parsed.payloads[0].second, Equals("0"));
      REQUIRE_THAT(parsed.payloads[1].second, Equals("android 0 14 IN IPv4 0.0.0.0"));
      REQUIRE_THAT(parsed.payloads[2].second, Equals("NVIDIA Streaming Client"));
      REQUIRE_THAT(parsed.payloads[3].second, Equals("x-nv-video[0].clientViewportWd:1920"));
      REQUIRE_THAT(parsed.payloads[4].second, Equals("x-nv-video[0].clientViewportHt:1080"));

      // Round trip
      REQUIRE(rtsp::to_string(parsed) == rtsp::to_string(rtsp::parse(rtsp::to_string(parsed)).value()));
    }

    SECTION("Incomplete packet") {
      // We no longer try to parse an incomplete packet, we still want to be robust to those cases
      std::string payload = "ANNOUNCE streamid=control/13/0 RTSP/1.0\n"
                            "CSeq: 6\n"
                            "X-GS-ClientVersion: 14\n"
                            "Host: 192.168.1.227\n"
                            "Session:  DEADBEEFCAFE\n"
                            "Content-type: application/sdp\n"
                            "Content-length: 1347\n"
                            "\n"
                            "v=0\n"
                            "o=android 0 14 IN IPv4 192.168.1.227\n"
                            "s=NVIDIA Streaming Client\n"
                            "a=x-nv-video[0].clientViewportWd:1920 \n"
                            "a=x-nv-video[0].clientViewportHt:1080 \n"
                            "a=x-nv-video[0].maxFPS:60 \n"
                            "a=x-nv-video[0].packetSize:1392 \n"
                            "a=x-nv-video[0].rateControlMode:4 \n"
                            "a=x-nv-video[0].timeoutLengthMs:7000 \n"
                            "a=x-nv-video[0].framesWithInvalidRefThreshold:0 \n"
                            "a\n\n\n\n"
                            "💩🚽"; // We can now eat up all the extra rubbish at the end
      auto parsed = rtsp::parse(payload).value();
      REQUIRE_THAT(parsed.options["Content-length"], Equals("1347"));
      // Round trip
      REQUIRE(rtsp::to_string(parsed) == rtsp::to_string(rtsp::parse(rtsp::to_string(parsed)).value()));
    }
  }

  SECTION("Response") {
    SECTION("Non valid packet") {
      auto parsed = rtsp::parse("RTSP/1.0 200 OK"); // missing CSeq
      REQUIRE(parsed.has_value() == false);
    }

    SECTION("Basic response") {
      auto payload = "RTSP/1.0 200 OK\r\n"
                     "CSeq: 123\r\n\r\n";
      auto parsed = rtsp::parse(payload).value();

      REQUIRE(parsed.type == RESPONSE);
      REQUIRE(parsed.seq_number == 123);

      REQUIRE_THAT(parsed.response.msg, Equals("OK"));
      REQUIRE(parsed.response.status_code == 200);

      REQUIRE(parsed.payloads.empty());
      REQUIRE(parsed.options.empty());

      // Round trip
      REQUIRE(rtsp::to_string(parsed) == rtsp::to_string(rtsp::parse(rtsp::to_string(parsed)).value()));
    }

    SECTION("Complete packet") {
      auto payload = "RTSP/1.0 404 NOT OK\n"
                     "CSeq: 1\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 10.1.2.49"
                     "\r\n\r\n"
                     "v=0\n"
                     "o=android 0 14 IN IPv4 0.0.0.0\n"
                     "s=NVIDIA Streaming Client\n"
                     "a=x-nv-video[0].clientViewportWd:1920\n"
                     "a=x-nv-video[0].clientViewportHt:1080"s;
      auto parsed = rtsp::parse(payload).value();

      REQUIRE(parsed.type == RESPONSE);
      REQUIRE_THAT(parsed.response.msg, Equals("NOT OK"));
      REQUIRE(parsed.response.status_code == 404);
      REQUIRE(parsed.seq_number == 1);

      // Options
      REQUIRE_THAT(parsed.options["X-GS-ClientVersion"], Equals("14"));
      REQUIRE_THAT(parsed.options["Host"], Equals("10.1.2.49"));

      // Payloads
      REQUIRE_THAT(parsed.payloads[0].second, Equals("0"));
      REQUIRE_THAT(parsed.payloads[1].second, Equals("android 0 14 IN IPv4 0.0.0.0"));
      REQUIRE_THAT(parsed.payloads[2].second, Equals("NVIDIA Streaming Client"));
      REQUIRE_THAT(parsed.payloads[3].second, Equals("x-nv-video[0].clientViewportWd:1920"));
      REQUIRE_THAT(parsed.payloads[4].second, Equals("x-nv-video[0].clientViewportHt:1080"));

      // Round trip
      REQUIRE(rtsp::to_string(parsed) == rtsp::to_string(rtsp::parse(rtsp::to_string(parsed)).value()));
    }
  }
}

state::SessionsAtoms test_init_state() {
  events::StreamSession session = {
      .display_mode = {1920, 1080, 60},
      .audio_channel_count = 2,
      .event_bus = std::make_shared<events::EventBusType>(),
      .app = std::make_shared<events::App>(events::App{.base = {},
                                                       .h264_gst_pipeline = "fakesrc ! fakesink",
                                                       .hevc_gst_pipeline = "",
                                                       .opus_gst_pipeline = "",
                                                       .runner = nullptr}),
      .aes_key = crypto::hex_to_str("9d804e47a6aa6624b7d4b502b32cc522", true),
      .aes_iv = crypto::hex_to_str("01234567890", true),
      .rtsp_fake_ip = "00.11.22.33.44",
      .session_id = 1234,
      .ip = "127.0.0.1",
      .video_stream_port = 1234,
      .audio_stream_port = 1235,
      .control_stream_port = 1236};
  return std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(
      immer::vector<events::StreamSession>{session});
}

namespace {
struct DualAnnounceFixture {
  events::StreamSession session = test_init_state()->load()->front();
  std::vector<std::pair<std::string, std::string>> attributes;

  explicit DualAnnounceFixture(const std::string &mode) {
    session.dual_display = std::make_shared<events::DualDisplayState>();
    session.dual_display->config = moonlight::dual_display::parse({{"gd_version", "1"},
                                                                   {"gd_mode", mode},
                                                                   {"gd_layout", "vertical"},
                                                                   {"gd_primary", "1"},
                                                                   {"gd_width2", "1280"},
                                                                   {"gd_height2", "720"},
                                                                   {"gd_fps2", "30"}},
                                                                  1920,
                                                                  1080,
                                                                  60);
    attributes = {{"x-nv-video[0].clientViewportWd", "1920"},
                  {"x-nv-video[0].clientViewportHt", "1080"},
                  {"x-nv-video[0].maxFPS", "60"},
                  {"x-nv-video[0].packetSize", "1024"},
                  {"x-nv-vqos[0].bitStreamFormat", "0"},
                  {"x-gd-version", "1"},
                  {"x-gd-transport", "rtp-ssrc-v1"},
                  {"x-gd-mode", mode},
                  {"x-gd-layout", "vertical"},
                  {"x-gd-primary", "1"}};
    for (const auto &display : session.dual_display->config.displays) {
      const auto prefix = fmt::format("x-gd-video[{}].", display.id);
      attributes.emplace_back(prefix + "width", std::to_string(display.width));
      attributes.emplace_back(prefix + "height", std::to_string(display.height));
      attributes.emplace_back(prefix + "fps", std::to_string(display.fps));
      attributes.emplace_back(prefix + "ssrc", std::to_string(moonlight::dual_display::ssrc(display.id)));
    }
  }

  void replace(const std::string &name, const std::string &value) {
    auto entry =
        std::find_if(attributes.begin(), attributes.end(), [&](const auto &entry) { return entry.first == name; });
    REQUIRE(entry != attributes.end());
    entry->second = value;
  }

  std::string wire(const std::string &leading = "", const std::string &trailing = " ") const {
    // moonlight-common-c's fillSerializedAttributeList() deliberately emits
    // "a=name:value \r\n", including its final space, for every SDP option.
    std::string payload = "v=0\r\no=android 0 14 IN IPv4 0.0.0.0\r\ns=NVIDIA Streaming Client\r\n";
    for (const auto &[name, value] : attributes)
      payload += "a=" + name + ":" + leading + value + trailing + "\r\n";
    return "ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq: 6\r\nContent-type: application/sdp\r\n"
           "Content-length: " +
           std::to_string(payload.size()) + "\r\n\r\n" + payload;
  }
};
} // namespace

TEST_CASE("Dual display ANNOUNCE accepts native client SDP whitespace on the wire", "[RTSP][dual-display]") {
  for (const auto *mode : {"dual_monitor", "dual_app"}) {
    for (const auto &padding :
         std::vector<std::pair<std::string, std::string>>{{"", " "}, {" ", "  "}, {"\t ", " \t"}}) {
      CAPTURE(mode, padding.first, padding.second);
      DualAnnounceFixture fixture(mode);
      const auto request = rtsp::parse(fixture.wire(padding.first, padding.second));
      REQUIRE(request);
      REQUIRE(request->payloads.size() == fixture.attributes.size() + 3);
      int videos = 0, audios = 0;
      auto video_observer = fixture.session.event_bus->register_handler<immer::box<events::VideoSession>>(
          [&](const immer::box<events::VideoSession> &video) {
            ++videos;
            CHECK(video->display_mode.width == 1920);
            CHECK(video->display_mode.height == 1080);
            CHECK(video->display_mode.refreshRate == 60);
            CHECK(video->dual_display.get() == fixture.session.dual_display);
            CHECK(fixture.session.dual_display->negotiated.load());
          });
      auto audio_observer = fixture.session.event_bus->register_handler<immer::box<events::AudioSession>>(
          [&](const immer::box<events::AudioSession> &) { ++audios; });
      const auto response = rtsp::commands::announce(*request, fixture.session);
      REQUIRE(response.response.status_code == 200);
      REQUIRE(response.seq_number == 6);
      REQUIRE(fixture.session.dual_display->negotiated.load());
      REQUIRE(fixture.session.dual_display->config.primary == 1);
      REQUIRE(fixture.session.dual_display->config.displays[1].width == 1280);
      REQUIRE(fixture.session.dual_display->config.displays[1].height == 720);
      REQUIRE(fixture.session.dual_display->config.displays[1].fps == 30);
      REQUIRE(videos == 1);
      REQUIRE(audios == 1);
      video_observer.unregister();
      audio_observer.unregister();
    }
  }
}

TEST_CASE("Dual display ANNOUNCE rejects inconsistent agreements before starting media", "[RTSP][dual-display]") {
  const std::vector<std::pair<std::string, std::string>> invalid{{"x-gd-version", "2"},
                                                                 {"x-gd-transport", "other-transport"},
                                                                 {"x-gd-mode", "controls"},
                                                                 {"x-gd-layout", "horizontal"},
                                                                 {"x-gd-primary", "0"},
                                                                 {"x-gd-video[0].width", "1280"},
                                                                 {"x-gd-video[1].width", "1920"},
                                                                 {"x-gd-video[1].height", "1080"},
                                                                 {"x-gd-video[1].fps", "60"},
                                                                 {"x-gd-video[0].ssrc", "0"},
                                                                 {"x-gd-video[1].ssrc", "1195638784"},
                                                                 {"x-gd-transport", "rtp-ssrc-v1 extra"},
                                                                 {"missing", ""},
                                                                 {"duplicate", ""}};
  for (const auto *mode : {"dual_monitor", "dual_app"}) {
    for (const auto &[name, value] : invalid) {
      CAPTURE(mode, name, value);
      DualAnnounceFixture fixture(mode);
      if (name == "missing") {
        fixture.attributes.erase(std::remove_if(fixture.attributes.begin(),
                                                fixture.attributes.end(),
                                                [](const auto &entry) { return entry.first == "x-gd-layout"; }),
                                 fixture.attributes.end());
      } else if (name == "duplicate") {
        fixture.attributes.emplace_back("x-gd-mode", mode);
      } else {
        fixture.replace(name, value);
      }
      const auto request = rtsp::parse(fixture.wire());
      REQUIRE(request);
      int videos = 0, audios = 0;
      auto video_observer = fixture.session.event_bus->register_handler<immer::box<events::VideoSession>>(
          [&](const immer::box<events::VideoSession> &) { ++videos; });
      auto audio_observer = fixture.session.event_bus->register_handler<immer::box<events::AudioSession>>(
          [&](const immer::box<events::AudioSession> &) { ++audios; });
      const auto response = rtsp::commands::announce(*request, fixture.session);
      REQUIRE(response.response.status_code == 400);
      REQUIRE_FALSE(fixture.session.dual_display->negotiated.load());
      REQUIRE(videos == 0);
      REQUIRE(audios == 0);
      video_observer.unregister();
      audio_observer.unregister();
    }
  }
}

#ifdef __linux__
TEST_CASE("RTSP encoder locality follows manual split nodes and retained automatic GPUs", "[RTSP][numa]") {
  struct EncoderEnvironment {
    std::optional<std::string> original;
    EncoderEnvironment() {
      if (const auto value = std::getenv("WOLF_ENCODER_NODE"))
        original = value;
    }
    ~EncoderEnvironment() {
      if (original)
        setenv("WOLF_ENCODER_NODE", original->c_str(), 1);
      else
        unsetenv("WOLF_ENCODER_NODE");
    }
  } environment;
  auto sessions = test_init_state();
  auto session = sessions->load()->front();
  session.app->render_node = "/dev/dri/renderD130";
  std::string selected;
  auto observer = session.event_bus->register_handler<immer::box<events::VideoSession>>(
      [&](const immer::box<events::VideoSession> &video) { selected = video->render_node; });
  const RTSP_PACKET request{.seq_number = 1,
                            .payloads = {{"a", "x-nv-video[0].clientViewportWd:1920"},
                                         {"a", "x-nv-video[0].clientViewportHt:1080"},
                                         {"a", "x-nv-video[0].maxFPS:60"}}};
  unsetenv("WOLF_ENCODER_NODE");
  REQUIRE(rtsp::commands::announce(request, session).response.status_code == 200);
  REQUIRE(selected == "/dev/dri/renderD130");
  setenv("WOLF_ENCODER_NODE", "/dev/dri/renderD128", 1);
  REQUIRE(rtsp::commands::announce(request, session).response.status_code == 200);
  REQUIRE(selected == "/dev/dri/renderD128");
  session.gpu_launch = std::make_shared<const wolf::gpu::Runtime::Launch>();
  REQUIRE(rtsp::commands::announce(request, session).response.status_code == 200);
  REQUIRE(selected == "/dev/dri/renderD130");
  observer.unregister();
}
#endif

TEST_CASE("Commands (Payload matching)", "[RTSP]") {
  constexpr int port = 8080;
  boost::asio::io_context ioc;
  auto state = test_init_state();
  auto wolf_server = tcp_server(ioc, port, state);
  auto wolf_client = tcp_tester::create_client(ioc, port, state);

  SECTION("MissingNo") {
    wolf_client->run("MissingNo rtsp://00.11.22.33.44:48010 RTSP/1.0\r\n"
                     "CSeq: 1\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 404);
                       REQUIRE(response.value().seq_number == 1);
                     });
  }

  SECTION("OPTION") {
    wolf_client->run("OPTIONS rtsp://00.11.22.33.44:48010 RTSP/1.0\r\n"
                     "CSeq: 1\r\n"
                     "X-GS-ClientVersion: 14\r\n"
                     "Host: 0.0.0.0"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 1);
                     });
  }

  SECTION("DESCRIBE") {
    wolf_client->run("DESCRIBE rtsp://00.11.22.33.44:48010 RTSP/1.0\n"
                     "CSeq: 2\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 00.11.22.33.44\n"
                     "Accept: application/sdp"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response);
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 2);
                       REQUIRE(response.value().payloads.size() == 5);
                       REQUIRE_THAT(response.value().payloads[0].first, Equals("sprop-parameter-sets"));
                       REQUIRE_THAT(response.value().payloads[0].second, Equals("AAAAAU"));
                       REQUIRE_THAT(response.value().payloads[1].second, Equals("fmtp:97 surround-params=21101"));
                       REQUIRE_THAT(response.value().payloads[2].second, Equals("fmtp:97 surround-params=642014235"));
                       REQUIRE_THAT(response.value().payloads[3].second, Equals("fmtp:97 surround-params=85301423675"));
                       REQUIRE_THAT(response.value().payloads[4].second, Equals("x-ss-general.featureFlags: 3"));
                     });
  }

  SECTION("SETUP audio") {
    wolf_client->run("SETUP streamid=audio/0/0 RTSP/1.0\n"
                     "CSeq: 3\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 00.11.22.33.44\n"
                     "Transport: unicast;X-GS-ClientPort=50000-50001\n"
                     "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 3);

                       REQUIRE_THAT(response.value().options["Session"], Equals("DEADBEEFCAFE;timeout = 90"));
                       REQUIRE_THAT(response.value().options["Transport"], Equals(fmt::format("server_port={}", 1235)));
                     });
  }

  SECTION("SETUP video") {
    wolf_client->run("SETUP streamid=video/0/0 RTSP/1.0\n"
                     "CSeq: 4\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 00.11.22.33.44\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Transport: unicast;X-GS-ClientPort=50000-50001\n"
                     "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 4);

                       REQUIRE_THAT(response.value().options["Session"], Equals("DEADBEEFCAFE;timeout = 90"));
                       REQUIRE_THAT(response.value().options["Transport"], Equals(fmt::format("server_port={}", 1234)));
                     });
  }

  SECTION("SETUP control") {
    wolf_client->run("SETUP streamid=control/0/0 RTSP/1.0\n"
                     "CSeq: 5\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 00.11.22.33.44\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Transport: unicast;X-GS-ClientPort=50000-50001\n"
                     "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 5);

                       REQUIRE_THAT(response.value().options["Session"], Equals("DEADBEEFCAFE;timeout = 90"));
                       REQUIRE_THAT(response.value().options["Transport"], Equals(fmt::format("server_port={}", 1236)));
                     });
  }

  SECTION("ANNOUNCE control") {
    // This is a very long message, it'll kick the recursion in receive_message()
    wolf_client->run("ANNOUNCE streamid=control/13/0 RTSP/1.0\n"
                     "CSeq: 6\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 00.11.22.33.44\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Content-type: application/sdp\n"
                     "Content-length: 1308"
                     "\r\n\r\n" // start of payload
                     "v=0\n"
                     "o=android 0 14 IN IPv4 0.0.0.0\n"
                     "s=NVIDIA Streaming Client\n"
                     "a=x-nv-video[0].clientViewportWd:1920 \n"
                     "a=x-nv-video[0].clientViewportHt:1080 \n"
                     "a=x-nv-video[0].maxFPS:60 \n"
                     "a=x-nv-video[0].packetSize:1024 \n"
                     "a=x-nv-video[0].rateControlMode:4 \n"
                     "a=x-nv-video[0].timeoutLengthMs:7000 \n"
                     "a=x-nv-video[0].framesWithInvalidRefThreshold:0 \n"
                     "a=x-nv-video[0].initialBitrateKbps:15500 \n"
                     "a=x-nv-video[0].initialPeakBitrateKbps:15500 \n"
                     "a=x-nv-vqos[0].bw.minimumBitrateKbps:15500 \n"
                     "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \n"
                     "a=x-nv-vqos[0].fec.enable:1 \n"
                     "a=x-nv-vqos[0].videoQualityScoreUpdateTime:5000 \n"
                     "a=x-nv-vqos[0].qosTrafficType:0 \n"
                     "a=x-nv-aqos.qosTrafficType:0 \n"
                     "a=x-nv-general.featureFlags:167 \n"
                     "a=x-nv-general.useReliableUdp:13 \n"
                     "a=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
                     "a=x-nv-vqos[0].drc.enable:0 \n"
                     "a=x-nv-general.enableRecoveryMode:0 \n"
                     "a=x-nv-video[0].videoEncoderSlicesPerFrame:1 \n"
                     "a=x-nv-clientSupportHevc:0 \n"
                     "a=x-nv-vqos[0].bitStreamFormat:0 \n"
                     "a=x-nv-video[0].dynamicRangeMode:0 \n"
                     "a=x-nv-video[0].maxNumReferenceFrames:1 \n"
                     "a=x-nv-video[0].clientRefreshRateX100:0 \n"
                     "a=x-nv-audio.surround.numChannels:2 \n"
                     "a=x-nv-audio.surround.channelMask:3 \n"
                     "a=x-nv-audio.surround.enable:0 \n"
                     "a=x-nv-audio.surround.AudioQuality:0 \n"
                     "a=x-nv-aqos.packetDuration:5 \n"
                     "a=x-nv-video[0].encoderCscMode:0 \n"
                     "t=0 0\n"
                     "m=video 47998 \n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 6);
                     });
  }

  SECTION("Non valid payload") {
    wolf_client->run("ANNOUNCE streamid=control/13/0 RTSP/1.0\n"
                     "CSeq: 7\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Content-type: application/sdp\n"
                     "Content-length: 170\n"
                     "\n"
                     "v=0\n"
                     "a=x-nv-video[0].timeoutLengthM\n" // Missing :
                     "a=x-nv-vqos[0].fec.enable:YES\n"  // Non number after :
                     // The following are required fields
                     "a=x-nv-video[0].clientViewportWd:1920 \n"
                     "a=x-nv-video[0].clientViewportHt:1080 \n"
                     "a=x-nv-video[0].maxFPS:60 \n"
                     "\n\n\n\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 7);
                     });
  }
}

TEST_CASE("Commands (IP Matching)", "[RTSP]") {
  constexpr int port = 8080;
  boost::asio::io_context ioc;
  auto state = test_init_state();
  auto wolf_server = tcp_server(ioc, port, state);
  auto wolf_client = tcp_tester::create_client(ioc, port, state);

  SECTION("MissingNo") {
    wolf_client->run("MissingNo rtsp://127.0.0.1:48010 RTSP/1.0\r\n"
                     "CSeq: 1\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 404);
                       REQUIRE(response.value().seq_number == 1);
                     });
  }

  SECTION("OPTION") {
    wolf_client->run("OPTIONS rtsp://127.0.0.1:48010 RTSP/1.0\r\n"
                     "CSeq: 1\r\n"
                     "X-GS-ClientVersion: 14\r\n"
                     "Host: 0.0.0.0"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 1);
                     });
  }

  SECTION("DESCRIBE") {
    wolf_client->run("DESCRIBE rtsp://127.0.0.1:48010 RTSP/1.0\n"
                     "CSeq: 2\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Accept: application/sdp"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response);
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 2);
                       REQUIRE(response.value().payloads.size() == 5);
                       REQUIRE_THAT(response.value().payloads[0].first, Equals("sprop-parameter-sets"));
                       REQUIRE_THAT(response.value().payloads[0].second, Equals("AAAAAU"));
                       REQUIRE_THAT(response.value().payloads[1].second, Equals("fmtp:97 surround-params=21101"));
                       REQUIRE_THAT(response.value().payloads[2].second, Equals("fmtp:97 surround-params=642014235"));
                       REQUIRE_THAT(response.value().payloads[3].second, Equals("fmtp:97 surround-params=85301423675"));
                       REQUIRE_THAT(response.value().payloads[4].second, Equals("x-ss-general.featureFlags: 3"));
                     });
  }

  SECTION("SETUP audio") {
    wolf_client->run("SETUP streamid=audio/0/0 RTSP/1.0\n"
                     "CSeq: 3\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Transport: unicast;X-GS-ClientPort=50000-50001\n"
                     "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 3);

                       REQUIRE_THAT(response.value().options["Session"], Equals("DEADBEEFCAFE;timeout = 90"));
                       REQUIRE_THAT(response.value().options["Transport"], Equals(fmt::format("server_port={}", 1235)));
                     });
  }

  SECTION("SETUP video") {
    wolf_client->run("SETUP streamid=video/0/0 RTSP/1.0\n"
                     "CSeq: 4\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Transport: unicast;X-GS-ClientPort=50000-50001\n"
                     "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 4);

                       REQUIRE_THAT(response.value().options["Session"], Equals("DEADBEEFCAFE;timeout = 90"));
                       REQUIRE_THAT(response.value().options["Transport"], Equals(fmt::format("server_port={}", 1234)));
                     });
  }

  SECTION("SETUP control") {
    wolf_client->run("SETUP streamid=control/0/0 RTSP/1.0\n"
                     "CSeq: 5\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Transport: unicast;X-GS-ClientPort=50000-50001\n"
                     "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"
                     "\r\n\r\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 5);

                       REQUIRE_THAT(response.value().options["Session"], Equals("DEADBEEFCAFE;timeout = 90"));
                       REQUIRE_THAT(response.value().options["Transport"], Equals(fmt::format("server_port={}", 1236)));
                     });
  }

  SECTION("ANNOUNCE control") {
    // This is a very long message, it'll kick the recursion in receive_message()
    wolf_client->run("ANNOUNCE streamid=control/13/0 RTSP/1.0\n"
                     "CSeq: 6\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Content-type: application/sdp\n"
                     "Content-length: 1308"
                     "\r\n\r\n" // start of payload
                     "v=0\n"
                     "o=android 0 14 IN IPv4 0.0.0.0\n"
                     "s=NVIDIA Streaming Client\n"
                     "a=x-nv-video[0].clientViewportWd:1920 \n"
                     "a=x-nv-video[0].clientViewportHt:1080 \n"
                     "a=x-nv-video[0].maxFPS:60 \n"
                     "a=x-nv-video[0].packetSize:1024 \n"
                     "a=x-nv-video[0].rateControlMode:4 \n"
                     "a=x-nv-video[0].timeoutLengthMs:7000 \n"
                     "a=x-nv-video[0].framesWithInvalidRefThreshold:0 \n"
                     "a=x-nv-video[0].initialBitrateKbps:15500 \n"
                     "a=x-nv-video[0].initialPeakBitrateKbps:15500 \n"
                     "a=x-nv-vqos[0].bw.minimumBitrateKbps:15500 \n"
                     "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \n"
                     "a=x-nv-vqos[0].fec.enable:1 \n"
                     "a=x-nv-vqos[0].videoQualityScoreUpdateTime:5000 \n"
                     "a=x-nv-vqos[0].qosTrafficType:0 \n"
                     "a=x-nv-aqos.qosTrafficType:0 \n"
                     "a=x-nv-general.featureFlags:167 \n"
                     "a=x-nv-general.useReliableUdp:13 \n"
                     "a=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
                     "a=x-nv-vqos[0].drc.enable:0 \n"
                     "a=x-nv-general.enableRecoveryMode:0 \n"
                     "a=x-nv-video[0].videoEncoderSlicesPerFrame:1 \n"
                     "a=x-nv-clientSupportHevc:0 \n"
                     "a=x-nv-vqos[0].bitStreamFormat:0 \n"
                     "a=x-nv-video[0].dynamicRangeMode:0 \n"
                     "a=x-nv-video[0].maxNumReferenceFrames:1 \n"
                     "a=x-nv-video[0].clientRefreshRateX100:0 \n"
                     "a=x-nv-audio.surround.numChannels:2 \n"
                     "a=x-nv-audio.surround.channelMask:3 \n"
                     "a=x-nv-audio.surround.enable:0 \n"
                     "a=x-nv-audio.surround.AudioQuality:0 \n"
                     "a=x-nv-aqos.packetDuration:5 \n"
                     "a=x-nv-video[0].encoderCscMode:0 \n"
                     "t=0 0\n"
                     "m=video 47998 \n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 6);
                     });
  }

  SECTION("Non valid payload") {
    wolf_client->run("ANNOUNCE streamid=control/13/0 RTSP/1.0\n"
                     "CSeq: 7\n"
                     "X-GS-ClientVersion: 14\n"
                     "Host: 0.0.0.0\n"
                     "Session:  DEADBEEFCAFE\n"
                     "Content-type: application/sdp\n"
                     "Content-length: 170\n"
                     "\n"
                     "v=0\n"
                     "a=x-nv-video[0].timeoutLengthM\n" // Missing :
                     "a=x-nv-vqos[0].fec.enable:YES\n"  // Non number after :
                     // The following are required fields
                     "a=x-nv-video[0].clientViewportWd:1920 \n"
                     "a=x-nv-video[0].clientViewportHt:1080 \n"
                     "a=x-nv-video[0].maxFPS:60 \n"
                     "\n\n\n\n"sv,
                     [](std::optional<RTSP_PACKET> response) {
                       REQUIRE(response.has_value());
                       REQUIRE(response.value().response.status_code == 200);
                       REQUIRE(response.value().seq_number == 7);
                     });
  }
}
TEST_CASE("RTSP sessions sharing an IP", "[RTSP][shared-ip]") {
  auto first = test_init_state()->load()->at(0);
  auto second = first;
  second.session_id = 5678;
  second.rtsp_fake_ip = "11.22.33.44";
  const immer::vector<events::StreamSession> sessions{first, second};
  auto packet = rtsp::parse("OPTIONS rtsp://11.22.33.44:48010 RTSP/1.0\r\nCSeq: 1\r\n\r\n").value();

  SECTION("URI identifier wins over an earlier IP match") {
    for (const auto &host : {"", "0.0.0.0"}) {
      packet.options["Host"] = host;
      auto match = tcp_connection::get_session(sessions, packet, first.ip);
      REQUIRE(match.has_value());
      REQUIRE(match->session_id == second.session_id);
    }
  }
  SECTION("Host identifier routes to the second session") {
    packet.request.uri.ip = "0.0.0.0";
    packet.options["Host"] = second.rtsp_fake_ip;
    auto match = tcp_connection::get_session(sessions, packet, first.ip);
    REQUIRE(match.has_value());
    REQUIRE(match->session_id == second.session_id);
  }
  SECTION("Ambiguous legacy IP is rejected, then recovers when one session leaves") {
    packet.request.uri.ip = "0.0.0.0";
    REQUIRE_FALSE(tcp_connection::get_session(sessions, packet, first.ip).has_value());
    auto match = tcp_connection::get_session({second}, packet, first.ip);
    REQUIRE(match.has_value());
    REQUIRE(match->session_id == second.session_id);
  }
  SECTION("Unknown IP and explicit unknown Host are rejected") {
    packet.request.uri.ip = "0.0.0.0";
    REQUIRE_FALSE(tcp_connection::get_session(sessions, packet, "192.0.2.1").has_value());
    packet.options["Host"] = "unknown";
    REQUIRE_FALSE(tcp_connection::get_session({first}, packet, first.ip).has_value());
  }
}

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <gst-plugin/video.hpp>
#include <moonlight/dual-display.hpp>

using namespace moonlight::dual_display;

TEST_CASE("Dual display negotiation preserves legacy and validates geometry", "[dual-display]") {
  REQUIRE(parse({}, 1920, 1080, 60).version == 0);
  REQUIRE_THROWS(parse({{"gd_mode", "dual_app"}}, 1920, 1080, 60));
  std::map<std::string, std::string> query{{"gd_version", "1"},
                                           {"gd_mode", "dual_monitor"},
                                           {"gd_layout", "vertical"},
                                           {"gd_primary", "1"},
                                           {"gd_width2", "1280"},
                                           {"gd_height2", "720"},
                                           {"gd_fps2", "30"}};
  auto cfg = parse(query, 1920, 1080, 60);
  REQUIRE(cfg.primary == 1);
  REQUIRE(cfg.displays[1].x == 0);
  REQUIRE(cfg.displays[1].y == 1080);
  REQUIRE(cfg.canvas_width() == 1920);
  REQUIRE(cfg.canvas_height() == 1800);
  query["gd_layout"] = "horizontal";
  cfg = parse(query, 1920, 1080, 60);
  REQUIRE(cfg.displays[1].x == 1920);
  REQUIRE(cfg.displays[1].y == 0);
  query["gd_mode"] = "dual_app";
  REQUIRE(parse(query, 1920, 1080, 60).displays[1].x == 0);
  query["gd_width2"] = "1281";
  REQUIRE_THROWS(parse(query, 1920, 1080, 60));
  query.erase("gd_width2");
  REQUIRE_THROWS(parse(query, 1920, 1080, 60));
  query["gd_mode"] = "controls";
  REQUIRE_FALSE(parse(query, 1920, 1080, 60).dual());
  query["gd_mode"] = "unknown";
  REQUIRE_THROWS(parse(query, 1920, 1080, 60));
}

TEST_CASE("Display input wrapper is bounded authenticated-session opt-in", "[dual-display]") {
  // NV keyboard packet: BE length excluding size, LE key-down magic, payload.
  std::vector<std::uint8_t> key{0, 0, 0, 10, 3, 0, 0, 0, 0, 65, 0, 0, 0, 0};
  REQUIRE(unwrap_input(key, false));
  REQUIRE(unwrap_input(key, true, 1)->display == 1);
  std::vector<std::uint8_t> wrapped{0, 0, 0, 22, 0x10, 0, 0, 0x55, 1, 0, 0, 0};
  wrapped.insert(wrapped.end(), key.begin(), key.end());
  REQUIRE_FALSE(unwrap_input(wrapped, false));
  auto inner = unwrap_input(wrapped, true);
  REQUIRE(inner);
  REQUIRE(inner->display == 1);
  REQUIRE(std::equal(inner->packet.begin(), inner->packet.end(), key.begin(), key.end()));
  wrapped[8] = 2;
  REQUIRE_FALSE(unwrap_input(wrapped, true));
  wrapped[8] = 1;
  wrapped[9] = 1;
  REQUIRE_FALSE(unwrap_input(wrapped, true));
  wrapped[9] = 0;
  wrapped[15] = 9;
  REQUIRE_FALSE(unwrap_input(wrapped, true));
  REQUIRE_FALSE(unwrap_input(std::span(key).first(7), true));
}

TEST_CASE("Mux packetizer keeps each track identity on data and parity", "[dual-display][GSTPlugin]") {
  moonlight::fec::init();
  std::array<gst_rtp_moonlight_pay_video *, 2> pay;
  std::array<std::vector<std::vector<unsigned char>>, 2> packets;
  std::ofstream fixture;
  if (const auto path = std::getenv("WOLF_DUAL_FIXTURE_OUT"))
    fixture.open(path, std::ios::binary);
  for (int id = 0; id < 2; ++id) {
    pay[id] = static_cast<gst_rtp_moonlight_pay_video *>(g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr));
    pay[id]->ssrc = ssrc(id);
    pay[id]->payload_size = 128;
    pay[id]->fec_percentage = 50;
    pay[id]->min_required_fec_packets = 2;
    pay[id]->frame_num = 1;
  }
  for (int frame = 0; frame < 3; ++frame) {
    for (int id = 0; id < 2; ++id) {
      const unsigned char marker = id ? 0xb0 : 0xa0;
      std::vector<unsigned char> h264{0, 0, 0, 1, 0x67, marker, 0, 0, 0, 1, 0x68, marker, 0, 0, 0, 1, 0x65, marker};
      h264.resize(600, 0x55);
      auto input = gst_buffer_new_and_fill(h264.size(), reinterpret_cast<const char *>(h264.data()));
      auto list = gst_moonlight_video::split_into_rtp(pay[id], input);
      REQUIRE(gst_buffer_list_length(list) > 6); // Data and actual Reed-Solomon parity.
      for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
        auto data = gst_buffer_copy_content(gst_buffer_list_get(list, i));
        const auto header = reinterpret_cast<const gst_moonlight_video::VideoRTPHeaders *>(data.data());
        REQUIRE(boost::endian::big_to_native(header->rtp.ssrc) == ssrc(id));
        REQUIRE(header->packet.frameIndex == frame + 1);
        // Deliberately drop one data shard on the secondary track. The real
        // Moonlight receiver fixture must reconstruct it from parity.
        if (!(id == 1 && i == 2))
          packets[id].push_back(std::move(data));
      }
      gst_buffer_list_unref(list);
      gst_buffer_unref(input);
    }
  }
  // Interleave the two independent packet sequences into a single transport.
  for (std::size_t i = 0; i < std::max(packets[0].size(), packets[1].size()); ++i)
    for (int id = 0; id < 2; ++id)
      if (i < packets[id].size() && fixture) {
        const auto &data = packets[id][i];
        const auto length = boost::endian::native_to_big(static_cast<std::uint32_t>(data.size()));
        fixture.write(reinterpret_cast<const char *>(&length), sizeof(length));
        fixture.write(reinterpret_cast<const char *>(data.data()), data.size());
      }
  for (auto element : pay)
    gst_object_unref(element);
}

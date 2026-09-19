#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <rfl/toml.hpp>

using Catch::Matchers::Equals;
using namespace wolf::core;

TEST_CASE("Serialize to JSON", "[serialization]") {
  SECTION("example from the README") {

    struct Person {
      std::string first_name;
      std::string last_name;
      int age;
    };

    const auto homer = Person{.first_name = "Homer", .last_name = "Simpson", .age = 45};

    REQUIRE_THAT(rfl::json::write(homer), Equals("{\"first_name\":\"Homer\",\"last_name\":\"Simpson\",\"age\":45}"));
  }

  SECTION("Wolf events") {
    auto event = events::PlugDeviceEvent{.session_id = "123",
                                         .udev_events = {{{"add", "usb"}}},
                                         .udev_hw_db_entries = {{"usb", {"usb1", "usb2"}}}};

    REQUIRE_THAT(rfl::json::write(event),
                 Equals("{\"session_id\":\"123\","
                        "\"udev_events\":[{\"add\":\"usb\"}],"
                        "\"udev_hw_db_entries\":[[\"usb\",[\"usb1\",\"usb2\"]]]}"));

    auto event2 = events::PairSignal{.client_ip = "192.168.1.1", .host_ip = "0.0.0.0"};

    REQUIRE_THAT(rfl::json::write(event2),
                 Equals("{\"client_ip\":\"192.168.1.1\","
                        "\"host_ip\":\"0.0.0.0\"}"));
  }

  // TODO: test the inverse operation, rfl::json::read
}

// TEST_CASE("Serialize to msgpack", "[serialization]") {
//   SECTION("example from the README") {
//
//     struct Person {
//       std::string first_name;
//       std::string last_name;
//       int age;
//     };
//
//     const auto homer = Person{.first_name = "Homer", .last_name = "Simpson", .age = 45};
//
//     auto result = rfl::msgpack::read<Person>(rfl::msgpack::write(homer));
//
//     REQUIRE_THAT(result.value().first_name, Equals("Homer"));
//     REQUIRE_THAT(result.value().last_name, Equals("Simpson"));
//     REQUIRE(result.value().age == 45);
//   }
// }
TEST_CASE("App GPU intent survives API and TOML round trips", "[serialization][gpu]") {
  auto bus = std::make_shared<events::EventBusType>();
  events::App app{
      .base = {.title = "GPU app", .id = "gpu-app"},
      .render_node = "/dev/dri/renderD129",
      .gpu_auto_select = true,
      .video = wolf::config::BaseAppVideoOverride{.source = "custom-source", .h264_encoder = "custom-encoder"},
      .audio = wolf::config::BaseAppAudioOverride{.source = "custom-audio"},
      .start_virtual_compositor = true,
      .start_audio_server = true,
      .runner = state::get_runner(wolf::config::AppCMD{.run_cmd = "true"}, bus)};

  SECTION("Automatic apps do not persist a resolved device") {
    auto wire = rfl::json::read<rfl::Reflector<events::App>::ReflType>(rfl::json::write(app)).value();
    REQUIRE(wire.gpu_auto_select == true);
    auto restored = rfl::Reflector<events::App>::to(wire, bus);
    auto saved = state::serialise_app(restored);
    auto persisted = rfl::toml::read<wolf::config::BaseApp>(rfl::toml::write(saved)).value();
    REQUIRE_FALSE(persisted.render_node.has_value());
    REQUIRE(persisted.video->source == "custom-source");
    REQUIRE(persisted.video->h264_encoder == "custom-encoder");
    REQUIRE(persisted.audio->source == "custom-audio");
  }

  SECTION("Explicit per-app devices remain pinned") {
    app.gpu_auto_select = false;
    auto saved = state::serialise_app(app);
    REQUIRE(saved.render_node == "/dev/dri/renderD129");
  }

  SECTION("Legacy API clients retain explicit node semantics") {
    auto wire = rfl::Reflector<events::App>::from(app);
    wire.gpu_auto_select.reset();
    auto legacy = rfl::json::read<rfl::Reflector<events::App>::ReflType>(rfl::json::write(wire)).value();
    auto restored = rfl::Reflector<events::App>::to(legacy, bus);
    REQUIRE_FALSE(restored.gpu_auto_select);
    REQUIRE(state::serialise_app(restored).render_node == "/dev/dri/renderD129");
  }
}

#include <api/api.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <gpu/discovery.hpp>
#include <gpu/pipeline.hpp>
#include <unistd.h>
using namespace wolf::gpu;
using namespace wolf::core;

TEST_CASE("Manual GPU pipeline stays bound to the requested device", "[gpu]") {
  EncoderBinding binding{"varenderD129h264enc", "va", Codec::h264, "/dev/dri/renderD129", {}};
  std::vector<PipelineTemplate> templates{{"va", "vah264lpenc bitrate={bitrate} ! h264parse"},
                                          {"va", "vah264enc bitrate={bitrate} ! h264parse"}};
  binding.converter = "varenderD129postproc";
  binding.producer_caps = "video/x-raw(memory:DMABuf)";
  auto pipeline = bind_pipeline(binding, templates, "source", "sink");
  REQUIRE(pipeline.find("varenderD129h264enc bitrate={bitrate}") != std::string::npos);
  REQUIRE(pipeline.find("varenderD129postproc") != std::string::npos);
  REQUIRE(pipeline.find("memory:DMABuf") != std::string::npos);
  REQUIRE(pipeline.find("memory:VAMemory") != std::string::npos);
  REQUIRE(pipeline.find("videoconvertscale") == std::string::npos);
  REQUIRE(pipeline.find("lpenc") == std::string::npos);
  binding.factory = "varenderD129h264lpenc";
  REQUIRE(bind_pipeline(binding, templates, "source", "sink").find(binding.factory) != std::string::npos);
  binding.factory = "varenderD129h264enc ! x264enc";
  REQUIRE_THROWS(bind_pipeline(binding, templates, "source", "sink"));
  binding = {"varenderD129av1enc", "va", Codec::av1, "/dev/dri/renderD129", {}};
  REQUIRE_THROWS(bind_pipeline(binding, templates, "source", "sink"));
  binding = {"nvh264device7enc", "nvcodec", Codec::h264, "/dev/dri/renderD129", 7};
  binding.converter = "cudaconvertscale";
  binding.producer_caps = "video/x-raw(memory:CUDAMemory)";
  templates.push_back({"nvcodec", "nvh264enc bitrate={bitrate} ! h264parse"});
  REQUIRE(bind_pipeline(binding, templates, "source", "sink").find("nvh264device7enc bitrate={bitrate}") !=
          std::string::npos);
  auto nv_pipeline = bind_pipeline(binding, templates, "source", "sink");
  REQUIRE(nv_pipeline.find("memory:CUDAMemory") != std::string::npos);
  REQUIRE(nv_pipeline.find("cudaupload") == std::string::npos);
  binding.producer_caps.clear();
  REQUIRE_THROWS(bind_pipeline(binding, templates, "source", "sink"));
  binding.cuda_device.reset();
  REQUIRE_THROWS(bind_pipeline(binding, templates, "source", "sink"));
}

TEST_CASE("GPU discovery keeps missing metrics distinct from zero", "[gpu]") {
  auto root = std::filesystem::temp_directory_path() / ("wolf-manual-discovery-" + std::to_string(getpid()));
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() {
      std::filesystem::remove_all(root);
    }
  } cleanup{root};
  auto dri = root / "dri";
  auto sys = root / "drm";
  auto device = sys / "renderD129" / "device";
  std::filesystem::create_directories(dri);
  std::filesystem::create_directories(device);
  std::ofstream(dri / "renderD129") << "fixture";
  std::ofstream(device / "product_name") << "Test GPU";
  std::ofstream(device / "mem_info_vram_total") << "8589934592";
  std::ofstream(device / "mem_info_vram_used") << "0";
  std::ofstream(device / "gpu_busy_percent") << "0";
  auto devices = discover({dri, sys});
  REQUIRE(devices.size() == 1);
  REQUIRE(devices[0].name == "Test GPU");
  REQUIRE(devices[0].vram_bytes == 8589934592ULL);
  REQUIRE(devices[0].vram_used_bytes == 0);
  REQUIRE(devices[0].gpu_percent == 0);
  REQUIRE_FALSE(devices[0].accessible); // Regular files never qualify as GPUs.
  std::ofstream(device / "gpu_busy_percent") << "101";
  std::filesystem::remove(device / "mem_info_vram_used");
  devices = discover({dri, sys});
  REQUIRE_FALSE(devices[0].gpu_percent.has_value());
  REQUIRE_FALSE(devices[0].vram_used_bytes.has_value());
}

TEST_CASE("Manual GPU API is additive and reports nullable telemetry", "[gpu]") {
  wolf::api::GpusResponse response;
  response.gpus.push_back({.device = {.id = "pci-one", .render_node = "/dev/dri/renderD129", .name = "Test GPU"},
                           .codecs = {true, false, false},
                           .users = 2,
                           .apps = 1});
  auto json = rfl::json::write(response);
  auto decoded = rfl::json::read<wolf::api::GpusResponse>(json);
  REQUIRE(decoded);
  REQUIRE(decoded->gpus[0].users == 2);
  REQUIRE(decoded->gpus[0].codecs[0]);
  REQUIRE_FALSE(decoded->gpus[0].device.vram_bytes.has_value());
  auto schema = rfl::json::to_schema<wolf::api::CreateLobbyRequest>();
  REQUIRE(schema.find("gpu_id") != std::string::npos);
  REQUIRE(schema.find("source_session_id") != std::string::npos);
}

#include <sessions/handlers.hpp>

TEST_CASE("Lobby join rejects incompatible manual GPU before switching the viewer", "[gpu]") {
  auto bus = std::make_shared<events::EventBusType>();
  events::StreamSession viewer{};
  viewer.session_id = 42;
  auto home = std::make_shared<events::GpuStreamTarget>();
  home->render_node = "/dev/dri/renderD128";
  viewer.gpu_route->home = home;
  viewer.gpu_route->target.store(home);
  auto gpu = std::make_shared<events::GpuStreamTarget>();
  gpu->render_node = "/dev/dri/renderD129";
  gpu->pipelines[1] = "HEVC only";
  events::Lobby lobby{.id = "manual", .name = "test", .gpu_target = gpu, .multi_user = true};
  auto app = immer::box<state::AppState>(state::AppState{
      .event_bus = bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(immer::vector<events::Lobby>{lobby}),
      .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(
          immer::vector<events::StreamSession>{viewer})});
  auto handlers = wolf::core::sessions::setup_lobbies_handlers(app, "/tmp", std::nullopt);
  auto join = events::JoinLobbyEvent{.lobby_id = "manual", .moonlight_session_id = 42};
  auto result = join.error_message.get()->get_future();
  bus->fire_event(immer::box<events::JoinLobbyEvent>(join));
  REQUIRE(result.get().find("cannot encode") != std::string::npos);
  REQUIRE(viewer.gpu_route->target.load() == home);
  REQUIRE(lobby.connected_sessions->load()->empty());
  viewer.gpu_route->codec.store(1);
  viewer.gpu_route->sdr_420.store(false);
  auto hdr_join = events::JoinLobbyEvent{.lobby_id = "manual", .moonlight_session_id = 42};
  auto hdr_result = hdr_join.error_message.get()->get_future();
  bus->fire_event(immer::box<events::JoinLobbyEvent>(hdr_join));
  REQUIRE(hdr_result.get().find("cannot encode") != std::string::npos);
  REQUIRE(viewer.gpu_route->target.load() == home);
  // Handler registrations are released with their immutable owning boxes.
}

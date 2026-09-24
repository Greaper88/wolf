#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <gpu/admission.hpp>
#include <gpu/discovery.hpp>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

using namespace wolf::gpu;
unsigned checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
Device device(std::string id = "0000:01:00.0") {
  return {.id = id,
          .render_node = "/dev/dri/renderD128",
          .by_path = "/dev/dri/by-path/pci-" + id + "-render",
          .driver = "amdgpu",
          .name = "Test GPU",
          .accessible = true,
          .hardware_encoder = true,
          .vram_bytes = 8ULL * 1024 * 1024 * 1024,
          .vram_percent = 20,
          .gpu_percent = 20,
          .encoder_percent = 0};
}
Options options(std::map<std::string, std::string> env = {}) {
  return read_options([&](const std::string &key) -> std::optional<std::string> {
    auto it = env.find(key);
    return it == env.end() ? std::nullopt : std::optional(it->second);
  });
}
void config_tests() {
  auto o = options();
  check(o.enabled() && o.auto_blacklist, "default-on flags");
  check(o.use_zero_copy && !o.require_zero_copy, "zero-copy preferred with fallback by default");
  check(options({{"WOLF_GPU_REQUIRE_ZERO_COPY", "true"}}).require_zero_copy, "strict zero-copy policy is configurable");
  check(!options({{"WOLF_USE_ZERO_COPY", "FALSE"}}).use_zero_copy, "explicit zero-copy disable");
  bool conflict = false;
  try {
    options({{"WOLF_USE_ZERO_COPY", "false"}, {"WOLF_GPU_REQUIRE_ZERO_COPY", "true"}});
  } catch (const std::invalid_argument &) {
    conflict = true;
  }
  check(conflict, "contradictory zero-copy settings rejected");
  check(o.gpu_threshold == 90 && o.gpu_retry_threshold == 87 && o.gpu_variance == 5, "GPU defaults");
  check(o.vram_threshold == 85 && o.vram_retry_threshold == 80, "VRAM defaults");
  check(o.encoder_threshold == 90 && o.encoder_retry_threshold == 85 && o.encoder_hard_limit == 98 &&
            o.encoder_variance == 5,
        "encoder defaults");
  for (auto key : {"WOLF_RENDER_NODE", "WOLF_ENCODER_NODE"}) {
    check(!options({{key, "/dev/dri/renderD129"}}).enabled(), "manual override");
    check(!options({{key, ""}, {"WOLF_GPU_PERCENT_THRESHOLD", "invalid"}}).enabled(),
          "manual bypass includes empty override and ignores tunables");
  }
  check(!options({{"WOLF_GPU_AUTO_SELECT", "FALSE"}, {"WOLF_GPU_PERCENT_THRESHOLD", "bad"}}).enabled(),
        "explicit off preserves legacy");
  for (auto invalid : {"NaN", "inf", "-1", "101", "90junk", ""}) {
    bool rejected = false;
    try {
      options({{"WOLF_GPU_PERCENT_THRESHOLD", invalid}});
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    check(rejected, "invalid percentage rejected");
  }
  for (auto key : {"WOLF_GPU_AUTO_SELECT",
                   "WOLF_GPU_AUTO_BLACKLIST",
                   "WOLF_ENCODER_UNKNOWN_POLICY",
                   "WOLF_USE_ZERO_COPY",
                   "WOLF_GPU_REQUIRE_ZERO_COPY"}) {
    bool rejected = false;
    try {
      options({{key, "typo"}});
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    check(rejected, "invalid policy rejected");
  }
  bool rejected = false;
  try {
    options({{"WOLF_GPU_PERCENT_RETRY_THRESHOLD", "95"}});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "retry stricter than primary");
  auto d = device();
  for (auto identifier : {d.id, d.render_node, std::string("renderD128"), d.by_path}) {
    auto blocked = options({{"WOLF_GPU_BLACKLIST", "0000:ff:00.0, " + identifier}});
    check(!select({d}, blocked).device, "all identifier formats blacklist");
  }
}
void policy_tests() {
  auto o = options();
  auto d = device();
  for (auto driver : {"virtio_gpu", "qxl", "bochs-drm", "vmwgfx", "simpledrm", "hyperv_drm", "ast", "mgag200"}) {
    d.driver = driver;
    check(!select({d}, o).device, "virtual driver rejected");
    o.auto_blacklist = false;
    check(select({d}, o).device.has_value(), "auto blacklist override still requires capability");
    o.auto_blacklist = true;
  }
  d = device();
  d.hardware_encoder = false;
  check(!select({d}, o).device, "no hardware encoder never software fallback");
  d = device();
  d.accessible = false;
  check(!select({d}, o).device, "inaccessible node");
  d = device();
  d.gpu_percent = 90;
  check(!select({d}, o).device, "GPU exact threshold");
  d.gpu_percent = 87;
  check(select({d}, o).device && !select({d}, o, true).device, "GPU retry threshold");
  d = device();
  d.vram_percent = 85;
  check(!select({d}, o).device, "VRAM exact threshold");
  d.vram_percent = 80;
  check(select({d}, o).device && !select({d}, o, true).device, "VRAM retry threshold");
  d = device();
  d.encoder_percent = 100;
  check(!select({d}, o).device, "100 percent encoder rejected");
  d.encoder_percent = 98;
  o.encoder_threshold = 100;
  check(!select({d}, o).device, "hard limit independent of admission threshold");
  o = options();
  d.active_sessions = 1;
  d.encoder_percent = 51;
  check(!select({d}, o).device && projected_encoder(d) == 102, "51 percent / one user rejects second");
  d.active_sessions = 2;
  d.encoder_percent = 59;
  check(select({d}, o).device && projected_encoder(d) == 88.5, "59 percent / two users admits third");
  check(!select({d}, o, true).device, "retry encoder threshold");
  d.pending_sessions = 1;
  check(!select({d}, o).device && projected_encoder(d) == 118, "pending session cannot dilute encoder estimate");
  d = device();
  d.encoder_percent = 0;
  d.pending_sessions = 1;
  check(!select({d}, o).device, "serialize first startup per GPU");
  d = device();
  d.encoder_percent = std::nullopt;
  check(select({d}, o).device && !select({d}, o).projected_encoder_percent, "unknown is not zero");
  o.encoder_unknown_policy = "reject";
  check(!select({d}, o).device, "strict unknown policy");
  o = options();
  d = device();
  d.encoder_percent = std::numeric_limits<double>::quiet_NaN();
  check(!select({d}, o).device, "bad sample cannot pass gates");
  d = device();
  auto other = device("0000:02:00.0");
  other.render_node = "/dev/dri/renderD129";
  d.active_sessions = 1;
  d.gpu_percent = 20;
  other.gpu_percent = 35;
  check(select({d, other}, o).device->id == other.id, "session distribution beats utilization");
  other.last_resort = true;
  check(select({d, other}, o).device->id == d.id, "preferred pool beats session count");
  d.encoder_percent = 100;
  check(select({d, other}, o).device->id == other.id, "saturated preferred permits last resort");
  d = device();
  other = device("0000:02:00.0");
  other.render_node = "/dev/dri/renderD129";
  o.last_resort = {d.id};
  check(select({d, other}, o).device->id == other.id, "explicit last resort classification");
  o.blacklist = {other.id};
  check(select({d, other}, o).device->id == d.id, "blacklist wins over pool preference");
  o = options();
  d.active_sessions = other.active_sessions = 2;
  d.encoder_percent = 20;
  other.encoder_percent = 30;
  d.gpu_percent = 70;
  other.gpu_percent = 10;
  check(select({d, other}, o).device->id == d.id, "projected encoder precedes core");
  other.encoder_percent = std::nullopt;
  check(select({d, other}, o).device->id == other.id, "mixed unknown falls through encoder rank without fake zero");
  d = device();
  other = device("0000:02:00.0");
  other.render_node = "/dev/dri/renderD129";
  d.gpu_percent = 32;
  d.vram_percent = 40;
  other.gpu_percent = 35;
  other.vram_percent = 20;
  check(select({d, other}, o).device->id == other.id, "variance delegates to VRAM");
  other.gpu_percent = 38;
  check(select({d, other}, o).device->id == d.id, "outside variance prefers lower core");
  std::vector<Device> chain{device("a"), device("b"), device("c")};
  chain[0].gpu_percent = 31;
  chain[0].vram_percent = 30;
  chain[1].gpu_percent = 35;
  chain[1].vram_percent = 20;
  chain[2].gpu_percent = 39;
  chain[2].vram_percent = 10;
  do {
    check(select(chain, o).device->id == "b", "variance ranking invariant under all permutations");
  } while (std::next_permutation(chain.begin(), chain.end(), [](const auto &a, const auto &b) { return a.id < b.id; }));
  check(select({device("b"), device("a")}, o).device->id == "a", "deterministic tie break");
  auto status = metadata(d, 20, 3);
  check(status.render_node == status.encoder_node && status.session_count_on_gpu == 3, "zero-copy metadata and count");
  check(select({}, o).error().find("/dev/dri") != std::string::npos, "actionable empty discovery failure");
}
void admission_tests() {
  Admission admission;
  auto o = options();
  unsigned samples = 0, delays = 0;
  auto sample = [&]() {
    ++samples;
    return std::vector{device()};
  };
  auto delay = [&]() { ++delays; };
  auto bypass = admission.acquire("manual", options({{"WOLF_ENCODER_NODE", "manual"}}), sample, delay);
  check(bypass.bypass && samples == 0, "manual bypass never samples");
  auto first = admission.acquire("one", o, sample, delay);
  check(first.reservation && admission.count(device().id) == 1, "launch reservation");
  check(!admission.acquire("one", o, sample, delay).reservation, "duplicate session refused");
  check(!admission.acquire("two", o, sample, delay).reservation && delays == 1,
        "pending startup blocks oversubscription");
  first.reservation->activate();
  auto busy_sample = [] {
    auto d = device();
    d.encoder_percent = 51;
    return std::vector{d};
  };
  check(!admission.acquire("two", o, busy_sample, delay).reservation, "active count projects measured load");
  first.reservation.reset();
  check(admission.count(device().id) == 0, "RAII releases on failure/stop");
  unsigned pass = 0;
  auto retry = admission.acquire(
      "retry",
      o,
      [&] {
        auto d = device();
        d.gpu_percent = pass++ ? 86 : 95;
        return std::vector{d};
      },
      delay);
  check(retry.reservation && pass == 2, "fresh retry telemetry");
  retry.reservation.reset();
  auto after_failure = admission.acquire("retry", o, sample, delay);
  check(after_failure.reservation != nullptr, "failed launch ID reusable");
  after_failure.reservation.reset();

  // Two concurrent launches must reserve different idle devices, even before either encoder starts.
  std::promise<void> go;
  auto ready = go.get_future().share();
  auto launch = [&](std::string id) {
    ready.wait();
    return admission.acquire(
        id,
        o,
        [] {
          auto a = device("a"), b = device("b");
          b.render_node = "/dev/dri/renderD129";
          return std::vector{a, b};
        },
        [] {});
  };
  auto a = std::async(std::launch::async, launch, "parallel-a");
  auto b = std::async(std::launch::async, launch, "parallel-b");
  go.set_value();
  auto ar = a.get(), br = b.get();
  check(ar.reservation && br.reservation && ar.reservation->gpu().id != br.reservation->gpu().id,
        "concurrent admission is serialized");
}
void nvml_tests() {
  if (!std::getenv("WOLF_GPU_TEST_NVML"))
    return;
  auto d = device("0000:02:00.0");
  d.driver = "nvidia";
  sample_nvidia(d);
  check(d.name == "Mock GPU" && d.vram_percent == 25 && d.vram_bytes == (8ULL << 30),
        "NVML PCI-bound identity and VRAM");
  check(d.gpu_percent == 31 && d.encoder_percent == 59, "NVML encoder independent from GPU core");
  d.id = "0000:99:00.0";
  d.encoder_percent.reset();
  sample_nvidia(d);
  check(!d.encoder_percent && !d.gpu_percent && !d.vram_bytes, "missing NVML PCI handle clears stale telemetry");
}
void discovery_tests() {
  auto dir = std::filesystem::temp_directory_path() /
             ("wolf-gpu-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::filesystem::remove_all(path);
    }
  } cleanup{dir};
  std::filesystem::create_directories(dir / "dri/by-path");
  std::filesystem::create_directories(dir / "sys/renderD128");
  auto pci = dir / "devices/0000:02:00.0";
  std::filesystem::create_directories(pci);
  std::filesystem::create_directories(dir / "drivers/amdgpu");
  std::filesystem::create_directory_symlink(pci, dir / "sys/renderD128/device");
  std::filesystem::create_directory_symlink(dir / "drivers/amdgpu", pci / "driver");
  std::ofstream(dir / "dri/renderD128") << "not a real device";
  std::filesystem::create_symlink(dir / "dri/renderD128", dir / "dri/by-path/pci-0000:02:00.0-render");
  std::ofstream(pci / "vendor") << "0x1002";
  std::ofstream(pci / "device") << "0x1234";
  std::ofstream(pci / "mem_info_vram_total") << "8589934592";
  std::ofstream(pci / "mem_info_vram_used") << "4294967296";
  std::ofstream(pci / "gpu_busy_percent") << "41";
  auto result = discover({dir / "dri", dir / "sys"});
  check(result.size() == 1 && result[0].id == "0000:02:00.0", "PCI identity discovery");
  check(!result[0].by_path.empty() && result[0].driver == "amdgpu", "by-path and driver resolution");
  check(result[0].vram_percent == 50 && result[0].gpu_percent == 41, "sysfs telemetry");
  check(!result[0].accessible && !result[0].hardware_encoder && !result[0].encoder_percent,
        "regular file cannot masquerade as GPU; unknown encoder stays unknown");
  check(discover({dir / "missing", dir / "sys"}).empty(), "missing DRM directory is safe");
}
int main() {
  try {
    config_tests();
    policy_tests();
    admission_tests();
    discovery_tests();
    nvml_tests();
    std::cout << checks << " checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAILED: " << e.what() << '\n';
    return 1;
  }
}

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gpu/numa_load.hpp>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

using namespace wolf::gpu;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
#ifdef __linux__
bool near(const std::optional<double> &actual, double expected) {
  return actual && std::abs(*actual - expected) < 0.000001;
}
#endif
Device device(std::string id, std::string render) {
  Device result;
  result.id = std::move(id);
  result.render_node = std::move(render);
  result.accessible = true;
  return result;
}

void disabled_tests() {
  unsigned reads = 0;
  auto reader = [&]() -> std::optional<std::string> {
    ++reads;
    return "unexpected";
  };
  check(!make_numa_load_ranking({}, wolf::numa::Mode::off, {}, {}, reader), "off returns no callback");
  check(reads == 0, "off never samples CPU counters");
  check(!make_numa_load_ranking({}, wolf::numa::Mode::automatic, {}, {}, reader),
        "empty inventory returns no callback");
  check(reads == 0, "empty inventory never samples CPU counters");
#ifndef __linux__
  const std::vector<Device> devices{device("a", "/dev/dri/renderD128"), device("b", "/dev/dri/renderD129")};
  check(!make_numa_load_ranking(devices, wolf::numa::Mode::on, {}, {}, reader),
        "unsupported platforms preserve optional no-op telemetry");
  check(reads == 0, "unsupported platforms never sample CPU counters");
#endif
}

#ifdef __linux__
void write(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << value;
}

struct Fixture {
  fs::path root;
  NumaLoadPaths paths;
  std::vector<Device> devices;
  std::chrono::steady_clock::time_point now{100s};
  std::map<unsigned, std::array<std::uint64_t, 8>> counters;
  unsigned reads = 0;
  std::uint64_t guest = 0;
  bool fail_read = false, throw_read = false, throw_clock = false;
  std::optional<std::string> text;
  Fixture() {
    auto pattern = (fs::temp_directory_path() / "wolf-numa-load-test-XXXXXX").string();
    auto made = mkdtemp(pattern.data());
    if (!made)
      throw std::runtime_error("Cannot create NUMA load fixture");
    root = made;
    paths = {{root / "sys/class/drm", root / "sys/devices/system/node", root / "proc/self/status"}, root / "proc/stat"};
    write(paths.topology.nodes / "has_memory", "2,8\n");
    write(paths.topology.nodes / "node2/cpulist", "0-3,32-35\n");
    write(paths.topology.nodes / "node8/cpulist", "4-7,36-39\n");
    write(paths.topology.status, "Cpus_allowed_list:\t1-2,33,36-37\nMems_allowed_list:\t2,8\n");
    for (unsigned index = 0; index < 3; ++index) {
      auto render = root / "dev/dri" / ("renderD" + std::to_string(128 + index));
      auto pci = root / "sys/devices" / ("0000:81:0" + std::to_string(index) + ".0");
      write(render, "");
      write(pci / "numa_node", index == 1 ? "8" : "2");
      fs::create_directories(paths.topology.drm_class / render.filename());
      fs::create_directory_symlink(pci, paths.topology.drm_class / render.filename() / "device");
      devices.push_back(device(pci.filename().string(), render.string()));
    }
    for (unsigned cpu : {0, 1, 2, 33, 36, 37})
      counters[cpu].fill(1000);
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  std::string stat() const {
    if (text)
      return *text;
    std::string value = "cpu 999999 0 0 0 0 0 0 0 0 0\nintr 123\n";
    for (const auto &[cpu, fields] : counters) {
      value += "cpu" + std::to_string(cpu);
      for (auto counter : fields)
        value += " " + std::to_string(counter);
      value += " " + std::to_string(guest) + " " + std::to_string(guest) + "\n";
    }
    return value;
  }
  void tick(unsigned first = 25, unsigned second = 75, unsigned iowait = 0) {
    now += 250ms;
    for (auto &[cpu, fields] : counters) {
      const unsigned busy = cpu == 0 ? 100 : cpu < 36 ? first : second;
      fields[0] += busy;
      const auto waiting = cpu == 0 ? 0 : iowait;
      fields[3] += 100 - busy - waiting;
      fields[4] += waiting;
    }
    guest += 1000000;
  }
  LoadClock clock() {
    return [&] {
      if (throw_clock)
        throw std::runtime_error("Clock failed");
      return now;
    };
  }
  LoadRanking provider(wolf::numa::Mode mode = wolf::numa::Mode::automatic) {
    return make_numa_load_ranking(devices, mode, paths, clock(), [&]() -> std::optional<std::string> {
      ++reads;
      if (throw_read)
        throw std::runtime_error("Counter read failed");
      return fail_read ? std::nullopt : std::optional(stat());
    });
  }
};

void topology_tests() {
  Fixture f;
  check(static_cast<bool>(f.provider()), "distinct usable nodes enable CPU load ranking");
  check(f.reads == 0, "enabled provider creation does not read counters");
  check(static_cast<bool>(f.provider(wolf::numa::Mode::on)), "explicit on also enables useful comparison");
  f.devices[1].accessible = false;
  check(!f.provider(), "inaccessible GPU does not enable comparison");
  f.devices[1].accessible = true;
  write(f.paths.topology.nodes / "has_memory", "2\n");
  check(!f.provider(wolf::numa::Mode::on), "even explicit on does not rank CPUs on single-memory-node topology");
  write(f.paths.topology.nodes / "has_memory", "2,8\n");
  write(f.paths.topology.status, "Cpus_allowed_list:\t1-2,33\nMems_allowed_list:\t2,8\n");
  check(!f.provider(), "GPU without permitted local CPUs does not enable comparison");
  write(f.paths.topology.status, "Cpus_allowed_list:\t1-2,33,36-37\nMems_allowed_list:\t2\n");
  check(!f.provider(), "GPU without permitted local memory does not enable comparison");
  write(f.paths.topology.status, "Cpus_allowed_list:\t1-2,33,36-37\nMems_allowed_list:\t2,8\n");
  fs::remove(f.paths.topology.drm_class / "renderD129/device");
  check(!f.provider(), "missing GPU topology is not guessed");
  check(f.reads == 0, "every inactive topology path avoids counter reads");
}

void sampling_tests() {
  Fixture f;
  auto ranking = f.provider();
  for (auto &gpu : f.devices)
    gpu.local_cpu_percent = 99;
  ranking(f.devices);
  check(f.reads == 1 && !f.devices[0].local_cpu_percent && !f.devices[1].local_cpu_percent,
        "first sample clears stale metrics and establishes only a baseline");
  f.now += 100ms;
  ranking(f.devices);
  check(f.reads == 1 && !f.devices[0].local_cpu_percent, "short initial interval remains unknown without another read");
  f.now -= 100ms;
  f.tick();
  ranking(f.devices);
  check(f.reads == 2 && near(f.devices[0].local_cpu_percent, 25) && near(f.devices[1].local_cpu_percent, 75),
        "permitted GPU-local CPU sets show distinct loads, excluding forbidden busy CPUs and guest double count");
  check(f.devices[2].local_cpu_percent == f.devices[0].local_cpu_percent,
        "GPUs on the same allowed local CPU set share one load sample");
  f.throw_read = true;
  f.now += 249ms;
  ranking(f.devices);
  check(f.reads == 2 && near(f.devices[0].local_cpu_percent, 25), "sub-250ms admission uses recent cached counters");
  f.throw_read = false;
  f.now -= 249ms;
  f.tick(20, 60, 20);
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 20) && near(f.devices[1].local_cpu_percent, 60),
        "iowait and idle are both excluded from CPU busy time");
  auto changed = f.devices;
  changed[0].id = "new-pci-device";
  changed[1].render_node += "-replaced";
  changed.push_back(device("new", "/dev/dri/renderD999"));
  for (auto &gpu : changed)
    gpu.local_cpu_percent = 99;
  ranking(changed);
  check(!changed[0].local_cpu_percent && !changed[1].local_cpu_percent && !changed.back().local_cpu_percent,
        "new identity or render node never inherits a different startup GPU's metric");
  check(near(changed[2].local_cpu_percent, 20), "unchanged startup identity still receives its cached metric");
  f.now += 31s;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent && !f.devices[1].local_cpu_percent,
        "long sampling gaps invalidate cache and rebaseline instead of ranking old averages");
  f.tick();
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25), "sampling resumes after two recent readings");
  f.now -= 1s;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent, "backward clock invalidates a cached sample and rebaselines");
  f.tick();
  ranking(f.devices);
  check(near(f.devices[1].local_cpu_percent, 75), "sampling recovers after a backward clock");
}

void invalid_counter_tests() {
  Fixture f;
  auto ranking = f.provider();
  ranking(f.devices);
  f.tick();
  f.counters.erase(33);
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent && near(f.devices[1].local_cpu_percent, 75),
        "missing allowed CPU invalidates only its local set");
  f.tick();
  f.counters[33].fill(2000);
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent, "newly online CPU requires a baseline before its set is ranked");
  f.tick();
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25), "restored CPU is ranked after a complete interval");
  f.tick();
  f.counters[1][0] = 0;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent && near(f.devices[1].local_cpu_percent, 75),
        "a decreasing CPU counter produces unknown rather than zero or wrapped load");
  f.tick();
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25), "reset CPU counter establishes a fresh usable baseline");
  f.tick();
  f.counters[36][4] = 0;
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25) && !f.devices[1].local_cpu_percent,
        "decreasing iowait invalidates the interval without fabricating headroom");
  f.now += 250ms;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent && !f.devices[1].local_cpu_percent,
        "unchanged counters are unknown rather than idle zero percent");
  for (const std::string malformed :
       {"garbage\n", "cpu1 -1 0 0 0 0 0 0 0\n", "cpu1 1 2 3\n", "cpu1 18446744073709551616 0 0 0 0 0 0 0\n"}) {
    f.now += 250ms;
    f.text = malformed;
    ranking(f.devices);
    check(!f.devices[0].local_cpu_percent && !f.devices[1].local_cpu_percent,
          "malformed and overflowing counters clear telemetry without failing");
  }
  f.text.reset();
  f.tick();
  ranking(f.devices);
  f.tick();
  f.text = f.stat() + "cpu1 1 2 3 4 5 6 7 8\n";
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent && near(f.devices[1].local_cpu_percent, 75),
        "duplicate CPU lines cannot silently replace an allowed CPU's counters");
}

void failure_tests() {
  Fixture f;
  auto ranking = f.provider();
  ranking(f.devices);
  f.tick();
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25), "failure fixture starts with valid telemetry");
  f.tick();
  f.fail_read = true;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent, "missing proc stat clears previous load");
  auto reads = f.reads;
  ranking(f.devices);
  check(f.reads == reads, "failed reads are also throttled");
  f.fail_read = false;
  f.tick();
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent, "reader recovery first rebaselines");
  f.tick();
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25), "reader recovery produces fresh load");
  f.tick();
  f.throw_read = true;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent && !f.devices[1].local_cpu_percent,
        "reader exceptions remain nonfatal and clear telemetry");
  f.throw_read = false;
  f.tick();
  ranking(f.devices);
  f.tick();
  ranking(f.devices);
  f.throw_clock = true;
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent, "clock exceptions remain nonfatal and cannot preserve cached telemetry");
  f.throw_clock = false;

  auto independent = f.provider();
  independent(f.devices);
  check(!f.devices[0].local_cpu_percent, "separate providers do not share counter baseline or cache");
  f.tick();
  auto copy = f.devices;
  std::thread first([&] { independent(f.devices); });
  std::thread second([&] { independent(copy); });
  first.join();
  second.join();
  check(near(f.devices[0].local_cpu_percent, 25) && near(copy[1].local_cpu_percent, 75),
        "simultaneous refresh and admission safely share one provider sample");
}

void file_reader_tests() {
  Fixture f;
  write(f.paths.proc_stat, f.stat());
  auto ranking = make_numa_load_ranking(f.devices, wolf::numa::Mode::automatic, f.paths, f.clock());
  ranking(f.devices);
  f.tick();
  write(f.paths.proc_stat, f.stat());
  ranking(f.devices);
  check(near(f.devices[0].local_cpu_percent, 25) && near(f.devices[1].local_cpu_percent, 75),
        "default file reader samples the injected proc stat path");
  fs::remove(f.paths.proc_stat);
  f.tick();
  ranking(f.devices);
  check(!f.devices[0].local_cpu_percent, "unreadable proc stat path leaves load unknown");
}
#endif
} // namespace

int main() {
  try {
    disabled_tests();
#ifdef __linux__
    topology_tests();
    sampling_tests();
    invalid_counter_tests();
    failure_tests();
    file_reader_tests();
#endif
    std::cout << checks << " NUMA CPU load checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "NUMA CPU load test failure: " << error.what() << '\n';
    return 1;
  }
}

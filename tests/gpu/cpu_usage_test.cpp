#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gpu/cpu_usage.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace wolf::cpu;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
bool near(const std::optional<double> &actual, double expected) {
  return actual && std::abs(*actual - expected) < 0.000001;
}

struct Fixture {
  std::chrono::steady_clock::time_point now{100s};
  CpuSets sockets{{2, {0, 3}}, {8, {9}}};
  Sample counters{{0, {}}, {3, {}}, {9, {}}};
  std::uint64_t guest = 0;
  unsigned reads = 0;
  bool fail = false, throw_reader = false, throw_clock = false;
  std::optional<std::string> override;
  std::string stat() const {
    if (override)
      return *override;
    std::string text = "cpu 999999 0 0 0 0 0 0 0 0 0\nintr 123\n";
    for (const auto &[id, fields] : counters) {
      text += "cpu" + std::to_string(id);
      for (auto value : fields)
        text += " " + std::to_string(value);
      text += " " + std::to_string(guest) + " " + std::to_string(guest) + "\n";
    }
    return text;
  }
  void tick(unsigned busy = 25, unsigned iowait = 0) {
    now += 5s;
    for (auto &[id, fields] : counters) {
      fields[0] += busy;
      fields[3] += 100 - busy - iowait;
      fields[4] += iowait;
    }
    guest += 1000000;
  }
  Clock clock() {
    return [&] {
      if (throw_clock)
        throw std::runtime_error("Clock failed");
      return now;
    };
  }
  Reader reader() {
    return [&]() -> std::optional<std::string> {
      ++reads;
      if (throw_reader)
        throw std::runtime_error("Read failed");
      return fail ? std::nullopt : std::optional(stat());
    };
  }
  UsageSampler sampler() {
    return make_usage_sampler(sockets, {}, clock(), reader());
  }
};

void sampling_tests() {
  Fixture f;
  auto sampler = f.sampler();
  check(f.reads == 0, "creating monitoring does no I/O or background sampling");
  auto usage = sampler();
  check(f.reads == 1 && !usage.at(2) && !usage.at(8), "first request establishes only a baseline");
  f.now += 100ms;
  check(!sampler().at(2) && f.reads == 1, "short warmup requests share an unknown cached interval");
  f.tick();
  f.counters[3][0] += 75;
  f.counters[3][3] += 25;
  f.counters[9][0] += 50;
  f.counters[9][3] -= 50;
  usage = sampler();
  check(near(usage.at(2), 100.0 * 125 / 300) && near(usage.at(8), 75),
        "socket load weights all its non-contiguous logical CPUs by counter deltas, ignoring guests");
  f.now += 249ms;
  check(sampler() == usage && f.reads == 2, "simultaneous viewers reuse bounded recent utilization");
  f.tick(0, 20);
  usage = sampler();
  check(near(usage.at(2), 0) && near(usage.at(8), 0), "genuine idle and iowait preserve a real zero percent");
  f.now += 31s;
  check(!sampler().at(2), "long inactivity drops old utilization and only rebaselines");
  f.tick(80);
  check(near(sampler().at(8), 80), "next fresh request resumes utilization after inactivity");
  f.now -= 1s;
  check(!sampler().at(8), "backwards clock clears utilization and rebaselines");
  f.tick(50);
  check(near(sampler().at(8), 50), "sampling recovers from a backwards clock");
}

void invalid_tests() {
  Fixture f;
  auto sampler = f.sampler();
  sampler();
  f.tick();
  f.counters.erase(3);
  auto usage = sampler();
  check(!usage.at(2) && near(usage.at(8), 25), "missing CPU invalidates only its socket");
  f.counters[3] = {};
  f.tick();
  check(!sampler().at(2), "new CPU requires a complete baseline");
  f.tick();
  check(near(sampler().at(2), 25), "restored CPU recovers after two complete samples");
  f.tick();
  f.counters[0][0] = 0;
  usage = sampler();
  check(!usage.at(2) && near(usage.at(8), 25), "decreasing counters cannot manufacture socket idle or wrap");
  f.tick(25, 10);
  check(near(sampler().at(2), 25), "counter reset becomes a usable new baseline");
  f.tick();
  f.counters[3][4] = 0;
  check(!sampler().at(2), "decreasing iowait invalidates the interval too");
  f.now += 5s;
  check(!sampler().at(8), "unchanged counters are unavailable rather than zero percent");
  f.tick();
  f.override = f.stat() + "cpu3 1 2 3 4 5 6 7 8\n";
  usage = sampler();
  check(!usage.at(2) && near(usage.at(8), 25), "duplicate CPU lines cannot replace valid counters");
  for (const auto &text :
       {"garbage\n", "cpu0 1 2 3\n", "cpu0 -1 0 0 0 0 0 0 0\n", "cpu0 18446744073709551616 0 0 0 0 0 0 0\n"}) {
    f.now += 5s;
    f.override = text;
    usage = sampler();
    check(!usage.at(2) && !usage.at(8), "malformed, negative or overflowing counters stay unavailable");
  }
}

void failure_tests() {
  Fixture f;
  auto sampler = f.sampler();
  sampler();
  f.tick();
  check(near(sampler().at(2), 25), "failure fixture starts with known load");
  f.tick();
  f.fail = true;
  check(!sampler().at(2), "read failure clears previous load");
  const auto reads = f.reads;
  sampler();
  check(f.reads == reads, "failed counter reads are also cached to bound requests");
  f.fail = false;
  f.tick();
  check(!sampler().at(2), "reader recovery first rebaselines");
  f.tick();
  check(near(sampler().at(2), 25), "reader recovery produces fresh utilization");
  f.tick();
  f.throw_reader = true;
  check(!sampler().at(2), "reader exceptions cannot fail the dashboard or retain load");
  f.throw_reader = false;
  f.tick();
  sampler();
  f.tick();
  sampler();
  f.throw_clock = true;
  check(!sampler().at(2), "clock exceptions clear previous load");
}

void policy_and_sharing_tests() {
  Fixture f;
  f.sockets = {{7, {9}}};
  // Monitoring must work on one socket with NUMA placement explicitly disabled.
  setenv("WOLF_NUMA_MODE", "off", 1);
  auto sampler = f.sampler();
  auto copy = sampler;
  sampler();
  f.tick(75);
  Usage first, second;
  std::thread a([&] { first = sampler(); });
  std::thread b([&] { second = copy(); });
  a.join();
  b.join();
  check(first == second && near(first.at(7), 75) && f.reads == 2,
        "one-socket NUMA-off monitoring copies safely share one counter read across concurrent viewers");
  auto independent = f.sampler();
  check(!independent().at(7), "independent providers have independent baselines");
  auto empty = make_usage_sampler({}, {}, f.clock(), f.reader());
  const auto reads = f.reads;
  check(empty().empty() && f.reads == reads, "no CPU topology avoids pointless counter reads");
  unsetenv("WOLF_NUMA_MODE");
}

void file_reader_tests() {
  Fixture f;
  auto pattern = (std::filesystem::temp_directory_path() / "wolf-cpu-usage-test-XXXXXX").string();
  const auto root = mkdtemp(pattern.data());
  if (!root)
    throw std::runtime_error("Cannot create CPU usage fixture");
  const auto path = std::filesystem::path(root) / "stat";
  auto sampler = make_usage_sampler(f.sockets, path, f.clock());
  std::ofstream(path) << f.stat();
  sampler();
  f.tick(60);
  std::ofstream(path) << f.stat();
  check(near(sampler().at(2), 60), "default reader samples its injected proc stat path");
  std::filesystem::remove(path);
  f.tick();
  check(!sampler().at(2), "missing proc stat is safely unavailable");
  std::filesystem::remove(root);
}
} // namespace

int main() {
  try {
    sampling_tests();
    invalid_tests();
    failure_tests();
    policy_and_sharing_tests();
    file_reader_tests();
    std::cout << checks << " CPU usage checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "CPU usage test failure: " << error.what() << '\n';
    return 1;
  }
}

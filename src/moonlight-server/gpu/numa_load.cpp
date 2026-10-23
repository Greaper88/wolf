#include "numa_load.hpp"
#include "cpu_usage.hpp"
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace wolf::gpu {
namespace {
using Clock = std::chrono::steady_clock;
using wolf::cpu::Sample;
using Identity = std::pair<std::string, std::string>;
constexpr auto minimum_interval = std::chrono::milliseconds(250);
constexpr auto maximum_interval = std::chrono::seconds(30);

struct Provider {
  std::map<Identity, std::size_t> devices;
  std::vector<std::vector<unsigned>> cpu_sets;
  LoadClock clock;
  LoadReader reader;
  std::mutex mutex;
  std::optional<Sample> baseline;
  Clock::time_point baseline_at{};
  std::optional<Clock::time_point> last_read_at;
  std::optional<Clock::time_point> cached_at;
  std::vector<std::optional<double>> cached;

  void invalidate() {
    baseline.reset();
    cached_at.reset();
    for (auto &value : cached)
      value.reset();
  }

  void sample(Clock::time_point now) {
    if (last_read_at && now >= *last_read_at && now - *last_read_at < minimum_interval) {
      if (!cached_at || now < *cached_at || now - *cached_at > maximum_interval)
        for (auto &value : cached)
          value.reset();
      return;
    }
    last_read_at = now;
    auto text = reader();
    if (!text) {
      invalidate();
      return;
    }
    auto current = wolf::cpu::parse_sample(*text);
    if (current.empty()) {
      invalidate();
      return;
    }
    if (!baseline || now <= baseline_at || now - baseline_at > maximum_interval) {
      for (auto &value : cached)
        value.reset();
      cached_at.reset();
    } else {
      for (std::size_t set = 0; set < cpu_sets.size(); ++set)
        cached[set] = wolf::cpu::usage_percent(cpu_sets[set], *baseline, current);
      cached_at = now;
    }
    baseline = std::move(current);
    baseline_at = now;
  }

  void annotate(std::vector<Device> &snapshot) {
    for (auto &device : snapshot)
      device.local_cpu_percent.reset();
    std::lock_guard lock(mutex);
    try {
      sample(clock());
      for (auto &device : snapshot)
        if (const auto found = devices.find({device.id, device.render_node}); found != devices.end())
          device.local_cpu_percent = cached[found->second];
    } catch (...) {
      // CPU telemetry is optional; a failing reader must not affect GPU
      // availability or leave a previously valid metric attached.
      invalidate();
      for (auto &device : snapshot)
        device.local_cpu_percent.reset();
    }
  }
};
} // namespace

LoadRanking make_numa_load_ranking(const std::vector<Device> &initial_devices,
                                   wolf::numa::Mode mode,
                                   NumaLoadPaths paths,
                                   LoadClock clock,
                                   LoadReader reader) {
  if (mode == wolf::numa::Mode::off)
    return {};
  try {
    auto provider = std::make_shared<Provider>();
    std::map<std::vector<unsigned>, std::size_t> sets;
    for (const auto &device : initial_devices) {
      if (!device.accessible)
        continue;
      auto locality = wolf::numa::resolve(device.render_node, paths.topology, wolf::numa::Mode::automatic);
      if (!locality.placement)
        continue;
      const auto &cpus = locality.placement->cpus;
      const auto [set, inserted] = sets.emplace(cpus, sets.size());
      if (inserted)
        provider->cpu_sets.push_back(cpus);
      provider->devices.emplace(Identity{device.id, device.render_node}, set->second);
    }
    if (sets.size() < 2)
      return {};
    provider->cached.resize(sets.size());
    provider->clock = clock ? std::move(clock) : LoadClock([] { return Clock::now(); });
    provider->reader = reader ? std::move(reader) : LoadReader([path = std::move(paths.proc_stat)]() {
      std::ifstream input(path);
      if (!input)
        return std::optional<std::string>{};
      std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      return input.bad() ? std::nullopt : std::optional(std::move(text));
    });
    return [provider = std::move(provider)](std::vector<Device> &snapshot) { provider->annotate(snapshot); };
  } catch (...) {
    return {};
  }
}

} // namespace wolf::gpu

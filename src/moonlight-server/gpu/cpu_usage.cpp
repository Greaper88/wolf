#include "cpu_usage.hpp"
#include <charconv>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace wolf::cpu {
namespace {
template <typename T> bool number(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}
} // namespace

Sample parse_sample(const std::string &text) {
  Sample result;
  std::set<unsigned> seen;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string label;
    fields >> label;
    unsigned cpu = 0;
    if (!label.starts_with("cpu") || label.size() == 3 || !number(std::string_view(label).substr(3), cpu))
      continue;
    // Duplicate or malformed CPU lines must never look like idle CPUs.
    if (!seen.insert(cpu).second) {
      result.erase(cpu);
      continue;
    }
    Counters counters{};
    bool valid = true;
    for (auto &counter : counters) {
      std::string token;
      if (!(fields >> token) || !number(token, counter)) {
        valid = false;
        break;
      }
    }
    // guest and guest_nice are already included in user and nice. Only the
    // first eight fields contribute to the total.
    if (valid)
      result.emplace(cpu, counters);
  }
  return result;
}

std::optional<double> usage_percent(const std::vector<unsigned> &cpus, const Sample &before, const Sample &after) {
  long double busy = 0, total = 0;
  for (const auto cpu : cpus) {
    auto previous = before.find(cpu), current = after.find(cpu);
    if (previous == before.end() || current == after.end())
      return std::nullopt;
    long double cpu_total = 0, cpu_idle = 0;
    for (std::size_t field = 0; field < current->second.size(); ++field) {
      // This also handles the kernel's occasionally decreasing iowait field:
      // discard the interval rather than manufacture a zero or negative load.
      if (current->second[field] < previous->second[field])
        return std::nullopt;
      auto delta = current->second[field] - previous->second[field];
      cpu_total += delta;
      if (field == 3 || field == 4)
        cpu_idle += delta;
    }
    if (cpu_total == 0)
      return std::nullopt;
    busy += cpu_total - cpu_idle;
    total += cpu_total;
  }
  if (total == 0)
    return std::nullopt;
  return static_cast<double>(100 * busy / total);
}

namespace {
using Time = std::chrono::steady_clock;
constexpr auto minimum_interval = std::chrono::milliseconds(250);
constexpr auto maximum_interval = std::chrono::seconds(30);

struct Provider {
  CpuSets cpu_sets;
  Clock clock;
  Reader reader;
  std::mutex mutex;
  std::optional<Sample> baseline;
  Time::time_point baseline_at{};
  std::optional<Time::time_point> last_read_at;
  Usage cached;

  void invalidate() {
    baseline.reset();
    for (auto &[id, value] : cached)
      value.reset();
  }

  Usage sample() {
    std::lock_guard lock(mutex);
    if (cpu_sets.empty())
      return cached;
    try {
      const auto now = clock();
      if (last_read_at && now >= *last_read_at && now - *last_read_at < minimum_interval)
        return cached;
      last_read_at = now;
      const auto text = reader();
      if (!text) {
        invalidate();
        return cached;
      }
      auto current = parse_sample(*text);
      if (current.empty()) {
        invalidate();
        return cached;
      }
      const bool recent = baseline && now > baseline_at && now - baseline_at <= maximum_interval;
      for (const auto &[id, cpus] : cpu_sets)
        cached[id] = recent ? usage_percent(cpus, *baseline, current) : std::nullopt;
      baseline = std::move(current);
      baseline_at = now;
    } catch (...) {
      // Monitoring failures must not affect availability or retain stale load.
      invalidate();
    }
    return cached;
  }
};
} // namespace

UsageSampler make_usage_sampler(CpuSets cpu_sets, std::filesystem::path proc_stat, Clock clock, Reader reader) {
  auto provider = std::make_shared<Provider>();
  provider->cpu_sets = std::move(cpu_sets);
  for (const auto &[id, cpus] : provider->cpu_sets)
    provider->cached.emplace(id, std::nullopt);
  provider->clock = clock ? std::move(clock) : Clock([] { return Time::now(); });
  provider->reader = reader ? std::move(reader) : Reader([path = std::move(proc_stat)]() {
    std::ifstream input(path);
    if (!input)
      return std::optional<std::string>{};
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return input.bad() ? std::nullopt : std::optional(std::move(text));
  });
  return [provider = std::move(provider)] { return provider->sample(); };
}

} // namespace wolf::cpu

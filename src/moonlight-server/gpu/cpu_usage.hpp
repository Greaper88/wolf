#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wolf::cpu {

using Counters = std::array<std::uint64_t, 8>;
using Sample = std::map<unsigned, Counters>;
Sample parse_sample(const std::string &text);
std::optional<double> usage_percent(const std::vector<unsigned> &cpus, const Sample &before, const Sample &after);

using CpuSets = std::map<int, std::vector<unsigned>>;
using Usage = std::map<int, std::optional<double>>;
using Clock = std::function<std::chrono::steady_clock::time_point()>;
using Reader = std::function<std::optional<std::string>()>;
using UsageSampler = std::function<Usage()>;

// Request-driven whole-device counters, independent of placement policy. Creation
// does no I/O. Copies share a synchronized 250 ms cache; a 30 s gap rebaselines.
// A first, incomplete or invalid interval is unknown, rather than idle zero.
UsageSampler make_usage_sampler(CpuSets cpu_sets,
                                std::filesystem::path proc_stat = "/proc/stat",
                                Clock clock = {},
                                Reader reader = {});

} // namespace wolf::cpu

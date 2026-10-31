#pragma once

#include "numa.hpp"
#include "selector.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace wolf::gpu {

using LoadRanking = std::function<void(std::vector<Device> &)>;
using LoadClock = std::function<std::chrono::steady_clock::time_point()>;
using LoadReader = std::function<std::optional<std::string>()>;

struct NumaLoadPaths {
  wolf::numa::Paths topology;
  std::filesystem::path proc_stat = "/proc/stat";
};

// Optional selection telemetry, with no worker or affinity changes. Only the
// startup GPUs' permitted local CPU sets are compared. A callback first needs
// two counter samples; missing or stale data clears the corresponding metric.
// Returns empty when disabled or there are no distinct usable local CPU sets.
LoadRanking make_numa_load_ranking(const std::vector<Device> &initial_devices,
                                   wolf::numa::Mode mode = wolf::numa::read_mode(),
                                   NumaLoadPaths paths = {},
                                   LoadClock clock = {},
                                   LoadReader reader = {});

} // namespace wolf::gpu

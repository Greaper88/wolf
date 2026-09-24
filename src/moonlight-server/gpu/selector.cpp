#include "selector.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace wolf::gpu {
namespace {
std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}
std::vector<std::string> list(std::string value) {
  std::replace(value.begin(), value.end(), ',', ' ');
  std::istringstream stream(value);
  std::vector<std::string> result;
  for (std::string item; stream >> item;)
    result.push_back(normalize_identifier(item));
  return result;
}
bool valid(std::optional<double> value) {
  return !value || (std::isfinite(*value) && *value >= 0 && *value <= 100);
}
const std::vector<std::string> virtual_drivers = {
    "virtio_gpu", "qxl", "bochs-drm", "bochs", "vmwgfx", "simpledrm", "hyperv_drm", "ast", "mgag200"};

// A comparator based on abs(a-b) <= tolerance is not a strict weak ordering.
// Narrow the set relative to its minimum instead; this is independent of enumeration order.
template <typename Metric> void band(std::vector<Device> &devices, Metric metric, double variance) {
  if (std::any_of(devices.begin(), devices.end(), [&](const auto &d) { return !metric(d); }))
    return;
  double best = std::numeric_limits<double>::infinity();
  for (const auto &d : devices)
    best = std::min(best, *metric(d));
  devices.erase(
      std::remove_if(devices.begin(), devices.end(), [&](const auto &d) { return *metric(d) > best + variance; }),
      devices.end());
}
} // namespace

bool blacklisted(const Device &device, const Options &options) {
  return matches(device, options.blacklist) ||
         (options.auto_blacklist &&
          std::find(virtual_drivers.begin(), virtual_drivers.end(), device.driver) != virtual_drivers.end());
}

Options read_options(const Environment &env) {
  Options o;
  auto boolean = [&](const char *key, bool fallback) {
    auto value = env(key);
    if (!value)
      return fallback;
    auto text = lower(*value);
    if (text == "true" || text == "1")
      return true;
    if (text == "false" || text == "0")
      return false;
    throw std::invalid_argument(std::string(key) + " must be true or false");
  };
  auto number = [&](const char *key, double fallback) {
    auto value = env(key);
    if (!value)
      return fallback;
    try {
      std::size_t consumed = 0;
      auto result = std::stod(*value, &consumed);
      if (consumed != value->size() || !std::isfinite(result) || result < 0 || result > 100)
        throw std::invalid_argument("");
      return result;
    } catch (const std::exception &) {
      throw std::invalid_argument(std::string(key) + " must be a finite percentage from 0 to 100");
    }
  };
  // Presence is intentional, including empty strings: never reinterpret an explicit manual override.
  o.manual_override = env("WOLF_RENDER_NODE").has_value() || env("WOLF_ENCODER_NODE").has_value();
  // The legacy path must not start validating unrelated selector settings.
  if (o.manual_override)
    return o;
  o.auto_select = boolean("WOLF_GPU_AUTO_SELECT", true);
  if (!o.auto_select)
    return o;
  o.auto_blacklist = boolean("WOLF_GPU_AUTO_BLACKLIST", true);
  o.use_zero_copy = boolean("WOLF_USE_ZERO_COPY", true);
  o.require_zero_copy = boolean("WOLF_GPU_REQUIRE_ZERO_COPY", false);
  if (o.require_zero_copy && !o.use_zero_copy)
    throw std::invalid_argument("WOLF_GPU_REQUIRE_ZERO_COPY requires WOLF_USE_ZERO_COPY=true");
  o.blacklist = list(env("WOLF_GPU_BLACKLIST").value_or(""));
  o.last_resort = list(env("WOLF_GPU_LAST_RESORT").value_or(""));
  o.gpu_threshold = number("WOLF_GPU_PERCENT_THRESHOLD", 90);
  o.gpu_retry_threshold = number("WOLF_GPU_PERCENT_RETRY_THRESHOLD", 87);
  o.gpu_variance = number("WOLF_GPU_PERCENT_VARIANCE", 5);
  o.vram_threshold = number("WOLF_VRAM_PERCENT_THRESHOLD", 85);
  o.vram_retry_threshold = number("WOLF_VRAM_PERCENT_RETRY_THRESHOLD", 80);
  o.encoder_threshold = number("WOLF_ENCODER_PERCENT_THRESHOLD", 90);
  o.encoder_retry_threshold = number("WOLF_ENCODER_PERCENT_RETRY_THRESHOLD", 85);
  o.encoder_hard_limit = number("WOLF_ENCODER_PERCENT_HARD_LIMIT", 98);
  o.encoder_variance = number("WOLF_ENCODER_PERCENT_VARIANCE", 5);
  o.encoder_unknown_policy = lower(env("WOLF_ENCODER_UNKNOWN_POLICY").value_or("session_count"));
  if (o.encoder_unknown_policy != "session_count" && o.encoder_unknown_policy != "reject")
    throw std::invalid_argument("WOLF_ENCODER_UNKNOWN_POLICY must be session_count or reject");
  if (o.gpu_retry_threshold > o.gpu_threshold || o.vram_retry_threshold > o.vram_threshold ||
      o.encoder_retry_threshold > o.encoder_threshold)
    throw std::invalid_argument("GPU retry thresholds must not exceed primary thresholds");
  return o;
}
Options read_options() {
  return read_options([](const std::string &key) -> std::optional<std::string> {
    if (const char *value = std::getenv(key.c_str()))
      return std::string(value);
    return std::nullopt;
  });
}

std::string normalize_identifier(std::string value) {
  value = lower(value);
  auto base = std::filesystem::path(value).filename().string();
  if (base.rfind("pci-", 0) == 0 && base.size() > 11 && base.substr(base.size() - 7) == "-render")
    return base.substr(4, base.size() - 11);
  if (base.rfind("renderd", 0) == 0)
    return base;
  return value;
}
bool matches(const Device &device, const std::vector<std::string> &identifiers) {
  for (const auto &id : identifiers) {
    auto normalized = normalize_identifier(id);
    if (normalized == normalize_identifier(device.id) || normalized == normalize_identifier(device.render_node) ||
        (!device.by_path.empty() && normalized == normalize_identifier(device.by_path)))
      return true;
  }
  return false;
}

std::optional<double> projected_encoder(const Device &d) {
  if (!d.encoder_percent)
    return std::nullopt;
  if (d.active_sessions == 0)
    return d.encoder_percent;
  // Pending reservations consume headroom but must not dilute the measured per-active-session estimate.
  return *d.encoder_percent + (*d.encoder_percent / d.active_sessions) * (1.0 + d.pending_sessions);
}

Selection select(const std::vector<Device> &snapshot, const Options &o, bool retry, bool existing_app) {
  Selection result;
  if (!o.enabled())
    return result; // Caller executes the unchanged legacy path.
  std::vector<Device> candidates;
  for (auto d : snapshot) {
    std::string reason;
    auto projected = projected_encoder(d);
    if (matches(d, o.blacklist))
      reason = "explicit blacklist";
    else if (o.auto_blacklist &&
             std::find(virtual_drivers.begin(), virtual_drivers.end(), d.driver) != virtual_drivers.end())
      reason = "auto-blacklisted driver " + d.driver;
    else if (!d.accessible)
      reason = "render node inaccessible; check device mounts and permissions";
    else if (!d.hardware_encoder)
      reason = "no verified hardware encoder on this node; check drivers and GStreamer";
    else if (!valid(d.encoder_percent) || !valid(d.gpu_percent) || !valid(d.vram_percent))
      reason = "invalid telemetry";
    else if (d.vram_percent && *d.vram_percent >= (retry ? o.vram_retry_threshold : o.vram_threshold))
      reason = "VRAM saturated";
    else if (!existing_app && d.gpu_percent && *d.gpu_percent >= (retry ? o.gpu_retry_threshold : o.gpu_threshold))
      reason = "GPU core saturated";
    else if (d.encoder_percent && *d.encoder_percent >= o.encoder_hard_limit)
      reason = "encoder hard limit";
    else if (projected && *projected >= (retry ? o.encoder_retry_threshold : o.encoder_threshold))
      reason = "projected encoder saturated";
    else if (!d.encoder_percent && o.encoder_unknown_policy == "reject")
      reason = "encoder utilization unavailable";
    else if (d.pending_sessions && d.active_sessions == 0)
      reason = "first session still starting; retry after encoder telemetry settles";
    if (!reason.empty()) {
      result.rejected.push_back({d.id, reason});
      continue;
    }
    d.last_resort = d.last_resort || matches(d, o.last_resort);
    candidates.push_back(std::move(d));
  }
  if (candidates.empty())
    return result;
  if (std::any_of(candidates.begin(), candidates.end(), [](const auto &d) { return !d.last_resort; }))
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const auto &d) { return d.last_resort; }),
                     candidates.end());
  auto count = [](const Device &d) {
    return static_cast<std::uint64_t>(d.active_sessions) + d.pending_sessions + d.retained_sessions;
  };
  auto fewest = count(*std::min_element(candidates.begin(), candidates.end(), [&](const auto &a, const auto &b) {
    return count(a) < count(b);
  }));
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(), [&](const auto &d) { return count(d) != fewest; }),
      candidates.end());
  // In session_count mode, a group containing unknown encoder load proceeds to GPU/VRAM ranking.
  band(candidates, projected_encoder, o.encoder_variance);
  band(candidates, [](const auto &d) { return d.gpu_percent; }, o.gpu_variance);
  band(candidates, [](const auto &d) { return d.vram_percent; }, 0);
  auto winner = std::min_element(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
    return std::make_pair(a.id, a.render_node) < std::make_pair(b.id, b.render_node);
  });
  result.device = *winner;
  result.projected_encoder_percent = projected_encoder(*winner);
  return result;
}
SessionGpu metadata(const Device &d, std::optional<double> projected, unsigned int count) {
  return {d.id, d.name, d.render_node, d.render_node, d.vram_bytes, d.encoder_percent, projected, count, std::nullopt};
}
std::string Selection::error() const {
  if (device)
    return {};
  std::string message =
      "No usable GPU/encoder capacity. Check /dev/dri mounts, GPU drivers, hardware encoders, "
      "WOLF_GPU_BLACKLIST and load thresholds, or stop another session. Software rendering is disabled.";
  for (const auto &r : rejected)
    message += " [" + r.id + ": " + r.reason + "]";
  return message;
}
} // namespace wolf::gpu

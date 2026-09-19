#pragma once
#include "admission.hpp"
#include "capability_cache.hpp"
#include <condition_variable>
#include <thread>

namespace wolf::gpu {
// One runtime per server. Expensive capability work is confined to the refresh worker.
class Runtime {
public:
  using Inventory = Admission::Snapshot;
  using Generation = std::function<std::string()>;
  using Probe = std::function<EncoderProbeResult(const Device &, Codec, std::stop_token)>;
  Runtime(Options options, Inventory inventory, Generation generation, Probe probe);
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  // Only cached capability + fresh telemetry. Does not wait for verification or run test encodes.
  Admission::Result acquire(const std::string &session_id);
  // Session startup owns this immutable bundle until teardown. Never resolve encoder bindings
  // independently after admission: a refresh may have changed the device or its capabilities.
  struct Launch {
    std::shared_ptr<Admission::Reservation> reservation;
    Device device;
    CapabilityCache::Snapshot capabilities;
  };
  struct LaunchResult {
    std::shared_ptr<const Launch> launch;
    bool bypass = false;
    std::string error;
  };
  LaunchResult prepare(const std::string &session_id, const std::string &device_id = {});
  LaunchResult retain(const std::string &app_id, const Launch &parent);
  std::string resume(const Launch &launch);
  bool supports(Codec codec) const;
  std::optional<CapabilityCache::Snapshot> capabilities(const Device &device) const;
  void invalidate(const std::string &device_id);
  void refresh();

private:
  void verify(std::stop_token stop);
  Options options_;
  Inventory inventory_;
  Generation generation_;
  Probe probe_;
  CapabilityCache cache_;
  Admission admission_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  bool refresh_ = true;
  std::jthread worker_;
};
} // namespace wolf::gpu

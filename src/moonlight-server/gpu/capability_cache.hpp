#pragma once
#include "encoder_types.hpp"
#include <array>
#include <map>
#include <mutex>

namespace wolf::gpu {
// Own one cache per server. A startup/device-refresh worker calls begin(), probes all codecs outside
// the lock, then complete(). Launches only call lookup()/usable(): they never initiate test encodes.
class CapabilityCache {
public:
  enum class Status {
    pending,
    ready,
    unavailable
  };
  using Results = std::array<EncoderProbeResult, 3>; // H.264, HEVC, AV1
  struct Snapshot {
    Status status = Status::pending;
    Results codecs;
    std::uint64_t revision = 0;
  };
  struct Ticket {
    std::string id;
    std::uint64_t sequence;
  };
  // generation must change after a driver reset/update or GStreamer registry refresh.
  // Device nodes must be canonical discovery paths, not user-supplied aliases.
  std::optional<Ticket> begin(const Device &device, const std::string &generation);
  bool complete(const Ticket &ticket, Results results);
  using Probe = std::function<EncoderProbeResult(const Device &, Codec)>;
  // Run on the startup/refresh worker with an isolated, deadline-limited probe callback.
  // No cache lock is held during driver work; exceptions become cached failures.
  bool verify(const Device &device, const std::string &generation, const Probe &probe);
  std::optional<Snapshot> lookup(const Device &device, const std::string &generation) const;
  bool usable(const Device &device, const std::string &generation) const;
  // Encoder failure: invalidate immediately, then enqueue a new background probe.
  void invalidate(const std::string &id);
  // Inventory refresh: remove disappeared devices and reject their late probe completions.
  void retain(const std::vector<std::string> &present_ids);

private:
  struct Entry {
    std::string node, driver, generation;
    std::uint64_t sequence;
    Snapshot snapshot;
  };
  static bool matches(const Entry &entry, const Device &device, const std::string &generation);
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::uint64_t sequence_ = 0;
};
} // namespace wolf::gpu

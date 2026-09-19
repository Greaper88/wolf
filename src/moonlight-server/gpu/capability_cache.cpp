#include "capability_cache.hpp"
#include <algorithm>

namespace wolf::gpu {
bool CapabilityCache::matches(const Entry &entry, const Device &device, const std::string &generation) {
  return entry.node == device.render_node && entry.driver == device.driver && entry.generation == generation;
}
std::optional<CapabilityCache::Ticket> CapabilityCache::begin(const Device &device, const std::string &generation) {
  std::lock_guard lock(mutex_);
  if (device.id.empty() || device.render_node.empty() || !device.accessible) {
    entries_.erase(device.id);
    return std::nullopt;
  }
  auto found = entries_.find(device.id);
  if (found != entries_.end() && matches(found->second, device, generation))
    return std::nullopt; // Pending, successful and failed results are all cached until invalidated.
  auto sequence = ++sequence_;
  entries_.insert_or_assign(device.id, Entry{device.render_node, device.driver, generation, sequence, {}});
  return Ticket{device.id, sequence};
}
bool CapabilityCache::complete(const Ticket &ticket, Results results) {
  std::lock_guard lock(mutex_);
  auto found = entries_.find(ticket.id);
  if (found == entries_.end() || found->second.sequence != ticket.sequence ||
      found->second.snapshot.status != Status::pending)
    return false;
  auto &entry = found->second;
  const std::array codecs{Codec::h264, Codec::hevc, Codec::av1};
  for (std::size_t i = 0; i < results.size(); ++i) {
    auto &result = results[i];
    if (result.encoder && (result.encoder->render_node != entry.node || result.encoder->codec != codecs[i])) {
      result.encoder.reset();
      result.failures.emplace_back("Probe result does not match the requested device/codec");
    }
  }
  entry.snapshot = {results[0].encoder ? Status::ready : Status::unavailable, std::move(results), entry.sequence};
  return true;
}
bool CapabilityCache::verify(const Device &device, const std::string &generation, const Probe &probe) {
  auto ticket = begin(device, generation);
  if (!ticket)
    return false;
  Results results;
  const std::array codecs{Codec::h264, Codec::hevc, Codec::av1};
  for (std::size_t i = 0; i < codecs.size(); ++i) {
    try {
      results[i] = probe(device, codecs[i]);
    } catch (...) {
      results[i].failures.emplace_back("Encoder capability worker failed");
    }
  }
  return complete(*ticket, std::move(results));
}
std::optional<CapabilityCache::Snapshot> CapabilityCache::lookup(const Device &device,
                                                                 const std::string &generation) const {
  std::lock_guard lock(mutex_);
  auto found = entries_.find(device.id);
  if (!device.accessible || found == entries_.end() || !matches(found->second, device, generation))
    return std::nullopt;
  return found->second.snapshot;
}
bool CapabilityCache::usable(const Device &device, const std::string &generation) const {
  auto snapshot = lookup(device, generation);
  return snapshot && snapshot->status == Status::ready;
}
void CapabilityCache::invalidate(const std::string &id) {
  std::lock_guard lock(mutex_);
  entries_.erase(id);
}
void CapabilityCache::retain(const std::vector<std::string> &present_ids) {
  std::lock_guard lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (std::find(present_ids.begin(), present_ids.end(), it->first) == present_ids.end())
      it = entries_.erase(it);
    else
      ++it;
  }
}
} // namespace wolf::gpu

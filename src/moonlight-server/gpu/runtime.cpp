#include "runtime.hpp"

namespace wolf::gpu {
Runtime::Runtime(Options options, Inventory inventory, Generation generation, Probe probe)
    : options_(std::move(options)), inventory_(std::move(inventory)), generation_(std::move(generation)),
      probe_(std::move(probe)) {
  if (options_.enabled())
    worker_ = std::jthread([this](std::stop_token stop) { verify(stop); });
}
Runtime::~Runtime() {
  worker_.request_stop();
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}
void Runtime::refresh() {
  {
    std::lock_guard lock(mutex_);
    refresh_ = true;
  }
  wake_.notify_all();
}
void Runtime::invalidate(const std::string &id) {
  cache_.invalidate(id);
  refresh();
}
std::optional<CapabilityCache::Snapshot> Runtime::capabilities(const Device &device) const {
  return cache_.lookup(device, generation_());
}
Admission::Result Runtime::acquire(const std::string &session_id) {
  auto result = prepare(session_id);
  return {result.launch ? result.launch->reservation : nullptr, result.bypass, std::move(result.error)};
}
Runtime::LaunchResult Runtime::prepare(const std::string &session_id, const std::string &device_id) {
  bool pending = false;
  std::map<std::string, std::pair<Device, CapabilityCache::Snapshot>> verified;
  std::string sampled_generation;
  auto result = admission_.acquire(
      session_id,
      options_,
      [&] {
        verified.clear();
        auto devices = inventory_();
        if (!device_id.empty())
          std::erase_if(devices, [&](const auto &device) { return device.id != device_id; });
        auto generation = generation_();
        sampled_generation = generation;
        for (auto &device : devices) {
          auto capability = cache_.lookup(device, generation);
          device.hardware_encoder = capability && capability->status == CapabilityCache::Status::ready;
          if (device.hardware_encoder)
            verified.emplace(device.id, std::make_pair(device, *capability));
          if (!blacklisted(device, options_) && device.accessible &&
              (!capability || capability->status == CapabilityCache::Status::pending))
            pending = true;
        }
        return devices;
      },
      [] {}); // Fresh retry is nonblocking; neither pass waits for a startup probe.
  if (!result.reservation && !result.bypass && pending) {
    result.error = "GPU verification in progress; retry the launch shortly";
    refresh();
  }
  if (!result.reservation)
    return {nullptr, result.bypass, std::move(result.error)};

  auto found = verified.find(result.reservation->gpu().id);
  if (found != verified.end()) {
    const auto &[device, capability] = found->second;
    auto generation = generation_();
    auto current = cache_.lookup(device, generation);
    if (generation == sampled_generation && current && current->status == CapabilityCache::Status::ready &&
        current->revision == capability.revision) {
      return {std::make_shared<const Launch>(Launch{std::move(result.reservation), device, capability}), false, {}};
    }
  }
  // The local reservation releases on return, including invalidation racing with admission.
  refresh();
  return {nullptr, false, "GPU verification changed during admission; retry the launch shortly"};
}
Runtime::LaunchResult Runtime::retain(const std::string &app_id, const Launch &parent) {
  auto cached = capabilities(parent.device);
  if (!cached || cached->status != CapabilityCache::Status::ready || cached->revision != parent.capabilities.revision)
    return {nullptr, false, "GPU verification changed; retry after restarting the launcher"};
  auto held = admission_.retain(app_id, *parent.reservation);
  if (!held.reservation)
    return {nullptr, false, held.error};
  return {std::make_shared<const Launch>(Launch{held.reservation, parent.device, *cached}), false, {}};
}
std::string Runtime::resume(const Launch &launch) {
  return launch.reservation->resume(options_, [&] {
    auto devices = inventory_();
    for (auto &device : devices) {
      auto cached = cache_.lookup(device, generation_());
      device.hardware_encoder = cached && cached->status == CapabilityCache::Status::ready &&
                                cached->revision == launch.capabilities.revision;
    }
    return devices;
  });
}
bool Runtime::supports(Codec codec) const {
  if (!options_.enabled())
    return false;
  try {
    auto generation = generation_();
    for (const auto &device : inventory_()) {
      if (!device.accessible || blacklisted(device, options_))
        continue;
      auto capability = cache_.lookup(device, generation);
      if (capability && capability->status == CapabilityCache::Status::ready) {
        const auto &binding = capability->codecs[static_cast<std::size_t>(codec)].encoder;
        if (binding && binding->plugin == "va")
          return true;
      }
    }
  } catch (...) {
  }
  return false;
}
void Runtime::verify(std::stop_token stop) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, stop, std::chrono::seconds(5), [this] { return refresh_; });
      if (stop.stop_requested())
        return;
      refresh_ = false;
    }
    try {
      auto devices = inventory_();
      auto generation = generation_();
      std::vector<std::string> present;
      for (const auto &device : devices)
        if (device.accessible && !blacklisted(device, options_))
          present.push_back(device.id);
      cache_.retain(present);
      for (const auto &device : devices) {
        if (stop.stop_requested())
          return;
        if (!device.accessible || blacklisted(device, options_))
          continue;
        cache_.verify(device, generation, [&](const Device &d, Codec codec) {
          if (stop.stop_requested())
            return EncoderProbeResult{};
          return probe_(d, codec, stop);
        });
      }
    } catch (...) {
      // Failed discovery cannot authorize a formerly present device.
      cache_.retain({});
    }
  }
}
} // namespace wolf::gpu

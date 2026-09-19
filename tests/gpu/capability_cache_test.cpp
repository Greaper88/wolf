#include <cstdlib>
#include <future>
#include <gpu/capability_cache.hpp>
#include <iostream>
using namespace wolf::gpu;
int checks = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
  ++checks;
}
CapabilityCache::Results success(const Device &d) {
  CapabilityCache::Results results;
  results[0].encoder = EncoderBinding{.factory = "vah264enc",
                                      .plugin = "va",
                                      .codec = Codec::h264,
                                      .render_node = d.render_node,
                                      .cuda_device = std::nullopt};
  results[1].failures = {"HEVC unavailable"};
  results[2].failures = {"AV1 unavailable"};
  return results;
}
int main() {
  Device d;
  d.id = "0000:01:00.0";
  d.render_node = "/dev/dri/renderD129";
  d.driver = "amdgpu";
  d.accessible = true;
  CapabilityCache cache;
  check(!cache.usable(d, "v1"), "unverified device excluded");
  auto ticket = cache.begin(d, "v1");
  check(ticket.has_value(), "startup schedules verification");
  check(cache.lookup(d, "v1")->status == CapabilityCache::Status::pending, "pending visible to launch");
  check(!cache.usable(d, "v1"), "pending device excluded");
  check(!cache.begin(d, "v1"), "duplicate startup tasks suppressed");
  check(cache.complete(*ticket, success(d)), "publish verified results");
  check(cache.usable(d, "v1"), "H264 sufficient with optional codecs unavailable");
  check(!cache.complete(*ticket, {}), "late duplicate cannot overwrite success");
  bool reusable = true;
  for (int i = 0; i < 100; ++i)
    reusable = reusable && cache.usable(d, "v1");
  check(reusable, "100 launches reuse capability without probe");
  check(!cache.begin(d, "v1"), "success stays cached");
  d.encoder_percent = 99;
  check(cache.usable(d, "v1"), "utilization is not cached as capability");
  check(!cache.usable(d, "v2"), "registry generation invalidates lookup");
  ticket = cache.begin(d, "v2");
  check(ticket.has_value(), "new generation scheduled");
  cache.invalidate(d.id);
  check(!cache.complete(*ticket, success(d)), "failure invalidation rejects stale in-flight result");
  auto replacement = cache.begin(d, "v2");
  check(replacement && replacement->sequence != ticket->sequence, "new probe has unique sequence");
  check(!cache.complete(*ticket, success(d)), "old completion cannot overwrite replacement");
  check(cache.complete(*replacement, {}), "unavailable result published");
  check(!cache.usable(d, "v2") && !cache.begin(d, "v2"), "failed checks cached, not repeated at launch");
  cache.invalidate(d.id);
  ticket = cache.begin(d, "v2");
  auto wrong = success(d);
  wrong[0].encoder->render_node = "/dev/dri/renderD128";
  cache.complete(*ticket, wrong);
  check(!cache.usable(d, "v2"), "wrong-device completion rejected");
  cache.invalidate(d.id);
  ticket = cache.begin(d, "v2");
  wrong = success(d);
  wrong[0].encoder->codec = Codec::av1;
  cache.complete(*ticket, wrong);
  check(!cache.usable(d, "v2"), "wrong-codec completion rejected");
  cache.invalidate(d.id);
  ticket = cache.begin(d, "v2");
  cache.retain({});
  check(!cache.complete(*ticket, success(d)), "removed device cannot be resurrected by worker");
  ticket = cache.begin(d, "v2");
  cache.complete(*ticket, success(d));
  d.render_node = "/dev/dri/renderD130";
  check(!cache.usable(d, "v2"), "node reassignment invalidates lookup");
  ticket = cache.begin(d, "v2");
  cache.complete(*ticket, success(d));
  d.driver = "other";
  check(!cache.usable(d, "v2"), "driver change invalidates lookup");
  d.accessible = false;
  check(!cache.begin(d, "v2") && !cache.lookup(d, "v2"), "inaccessible devices removed");
  d.accessible = true;
  std::promise<void> start;
  auto go = start.get_future().share();
  auto worker = [&] {
    go.wait();
    return cache.begin(d, "v3");
  };
  auto a = std::async(std::launch::async, worker), b = std::async(std::launch::async, worker);
  start.set_value();
  check(a.get().has_value() != b.get().has_value(), "concurrent refresh schedules only one probe");
  cache.invalidate(d.id);
  unsigned calls = 0;
  auto probe = [&](const Device &device, Codec codec) {
    ++calls;
    check(!cache.usable(device, "v4"), "worker publishes only complete capability snapshots");
    if (codec != Codec::h264)
      throw std::runtime_error("optional codec failure");
    return success(device)[0];
  };
  check(cache.verify(d, "v4", probe) && calls == 3, "worker checks each codec once");
  check(cache.usable(d, "v4"), "optional codec exception preserves H264 support");
  check(!cache.verify(d, "v4", probe) && calls == 3, "refresh reuses cached capabilities");
  cache.invalidate(d.id);
  check(cache.verify(d, "v4", [](const Device &, Codec) -> EncoderProbeResult { throw 1; }),
        "all worker exceptions published");
  check(cache.lookup(d, "v4")->status == CapabilityCache::Status::unavailable, "exceptions never leave device pending");
  std::cout << checks << " capability cache checks passed\n";
}
